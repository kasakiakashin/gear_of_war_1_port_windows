// xma.cpp — descodificador XMA por hardware (bloco MMIO 0x7FEA0000).
//
// O XAudio2 do título fala diretamente com o hardware (como no Xenia, xma_decoder.cc / xma_register_table.inc):
//   * registrador N em 0x7FEA0000 + 4N, guardado em little-endian (o jogo usa lwbrx/stwbrx);
//   * 0x600 ContextArrayAddress = endereço físico da tabela de 320 contextos de 64 bytes (XMA_CONTEXT_DATA);
//   * 0x606 CurrentContextIndex (contexto em processamento; o jogo compara-o ^0x200 com os seus ids);
//   * 0x650..0x659 Kick, 0x690..0x699 Lock, 0x6A0..0x6A9 Clear: máscaras de 32 contextos por registrador.
// Aqui o MMIO é RAM simples: uma thread do host varre os registradores de comando a cada ~1 ms, troca-os
// atomicamente por 0 e aplica-os (Lock → Clear → Kick). Um Kick ativa o contexto até ao Lock seguinte (como Enable/Disable
// do Xenia): enquanto ativo, cada volta (~1 ms) faz uma passagem de descodificação, como o Work() do Xenia, e o anel
// de saída volta a encher-se assim que o jogo o lê e o marca outra vez como válido, sem novo Kick. Com uma passagem por
// Kick, os sons curtos (1 Kick, anel cheio) eram libertados com metade do PCM por tocar. A passagem descodifica pacotes
// de 2048 bytes (xma_decoder.cpp) e enche o anel de saída com PCM de 16 bits BE intercalado, em blocos de 256 bytes.
//
// XMA_CONTEXT_DATA (16 palavras BE; campos de bits a partir do bit menos significativo de cada palavra):
//   0: in0_packet_count:12 loop_count:8 in0_valid:1 in1_valid:1 out_block_count:5 out_write_offset:5
//   1: in1_packet_count:12 loop_sub_start:2 loop_sub_end:3 loop_sub_skip:3 sub_decode_count:4 sub_skip_count:3
//      sample_rate:2 is_stereo:1 unk:1 out_valid:1
//   2: input_buffer_read_offset:26 (bits)     3: loop_start:26     4: loop_end:26 packet_metadata:5 current_buffer:1
//   5/6: input_buffer_0/1_ptr (físico)  7: output_buffer_ptr  8: work_buffer_ptr
//   9: out_read_offset:5 … stop_when_done:1(30) interrupt_when_done:1(31)
// Pacotes de 2048 bytes; cabeçalho de 32 bits: frame_count:6 | first_frame_offset_bits:15 | metadata:3 | skip:8.
// O skip salta pacotes de outros fluxos intercalados; loop_count (255 = infinito) repete loop_start..loop_end (bits).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>

#include "apu/apu.h"
#include "apu/xma_decoder.h"
#include "guest_memory.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace {

constexpr uint32_t kCtxSize = 64, kCtxCount = 320;
constexpr uint32_t kRegArray = 0x600, kRegKick = 0x650, kRegLock = 0x690, kRegClear = 0x6A0;
constexpr uint32_t kPacketBytes = 2048, kPacketBits = kPacketBytes * 8, kBlockBytes = 256;

std::mutex g_mu;
uint32_t g_array_va = 0, g_array_phys = 0;
bool g_used[kCtxCount] = {};
std::atomic<bool> g_enabled[kCtxCount];
std::atomic<uint64_t> g_kicks{0};
bool g_trace = false;
bool g_debug_drops = false;  // GOW_DEBUG_AUDIO_DROPS=1
const char* g_dump_dir = nullptr;  // GOW_XMA_DUMP_DIR: PCM de cada contexto, para análise
uint64_t g_stat_packets = 0, g_stat_skips = 0, g_stat_starved = 0, g_stat_jumps = 0, g_stat_trimmed = 0;
apu::XmaStream g_streams[kCtxCount];  // estado do descodificador por contexto (host)
uint64_t g_dbg_out_invalid[kCtxCount] = {}, g_dbg_kicks[kCtxCount] = {};

// Resumo de um contexto quando o jogo o limpa ou liberta: quanto entrou, quanto saiu e o que ficou por tocar.
void debug_summary(uint32_t i, const char* why) {
  apu::XmaStream& s = g_streams[i];
  if (!g_debug_drops || (!s.dbg_packets && !g_dbg_kicks[i])) return;
  std::fprintf(stderr, "[audio-drops] ctx %u %s: %u canais %u Hz, %llu kicks, %llu pacotes, %llu amostras, %llu recusados, %llu erros de "
                       "receive, %llu falhas de open, %llu esperas, %llu passagens sem saída válida, %zu amostras por escrever descartadas\n",
               i, why, s.channels, s.rate, (unsigned long long)g_dbg_kicks[i], (unsigned long long)s.dbg_packets,
               (unsigned long long)s.dbg_samples, (unsigned long long)s.dbg_rejected, (unsigned long long)s.dbg_recv_err,
               (unsigned long long)s.dbg_open_fail, (unsigned long long)s.dbg_starved, (unsigned long long)g_dbg_out_invalid[i],
               s.pcm.size() - s.pcm_pos);
  s.dbg_packets = s.dbg_samples = s.dbg_rejected = s.dbg_recv_err = s.dbg_open_fail = s.dbg_starved = 0;
  g_dbg_out_invalid[i] = g_dbg_kicks[i] = 0;
}

volatile LONG* reg(uint32_t i) { return reinterpret_cast<volatile LONG*>(gm::base() + gm::kXmaRegBase + 4 * i); }
uint8_t* phys_ptr(uint32_t phys) { return gm::base() + 0xA0000000u + (phys & 0x1FFFFFFFu); }

struct Ctx {
  uint32_t d[16];
  uint32_t get(int w, int lo, int n) const { return (d[w] >> lo) & ((n == 32 ? 0u : (1u << n)) - 1u); }
  void set(int w, int lo, int n, uint32_t v) {
    uint32_t m = (((n == 32) ? 0u : (1u << n)) - 1u) << lo;
    d[w] = (d[w] & ~m) | ((v << lo) & m);
  }
};

uint32_t ctx_va(uint32_t i) { return g_array_va + i * kCtxSize; }
void load(uint32_t i, Ctx& c) {
  for (int w = 0; w < 16; w++) c.d[w] = ppc_ld32(gm::base(), ctx_va(i) + 4 * w);
}
void store(uint32_t i, const Ctx& c) {
  for (int w = 0; w < 16; w++) ppc_st32(gm::base(), ctx_va(i) + 4 * w, c.d[w]);
}

void swap_input(Ctx& c) {
  uint32_t cur = c.get(4, 31, 1);
  c.set(0, 20 + cur, 1, 0);
  c.set(4, 31, 1, cur ^ 1);
  c.set(2, 0, 26, 0);
}

// Quadros que começam em from_bit ou depois (bits a contar do início do pacote) e acabam dentro do pacote. Cada quadro começa
// com o seu comprimento em 15 bits; first_frame_offset diz onde está o 1º depois do cabeçalho de 32 bits.
uint32_t read_bits15(const uint8_t* p, uint32_t bit) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < 15; i++, bit++) v = v << 1 | ((p[bit >> 3] >> (7 - (bit & 7))) & 1);
  return v;
}

uint32_t frames_from(const uint8_t* pkt, uint32_t from_bit) {
  uint32_t bit = 32 + (((pkt[0] & 3u) << 13) | (pkt[1] << 5) | (pkt[2] >> 3)), n = 0;
  while (bit + 15 <= kPacketBits) {
    const uint32_t len = read_bits15(pkt, bit);
    if (len == 0x7FFF || len < 15 || bit + len > kPacketBits) break;
    if (bit >= from_bit) n++;
    bit += len;
  }
  return n;
}

// Uma passagem de descodificação (Work do Xenia).
void decode(uint32_t i, Ctx& c) {
  if (!c.get(1, 31, 1)) {  // saída inválida
    g_dbg_out_invalid[i]++;
    return;
  }
  apu::XmaStream& st = g_streams[i];
  // Sem mais entrada: o resto do último quadro (menos de um bloco) sai completado com silêncio, em vez de se perder.
  if (!c.get(0, 20, 1) && !c.get(0, 21, 1) && st.pcm.size() > st.pcm_pos && st.pcm.size() - st.pcm_pos < kBlockBytes / 2)
    st.pcm.resize(st.pcm_pos + kBlockBytes / 2, 0);
  if (!c.get(0, 20, 1) && !c.get(0, 21, 1) && st.pcm.size() - st.pcm_pos < kBlockBytes / 2) {
    c.set(1, 31, 1, 0);
    return;
  }
  const uint32_t stereo = c.get(1, 29, 1), channels = stereo + 1;
  const uint32_t block_count = c.get(0, 22, 5);
  uint32_t wr = c.get(0, 27, 5);
  const uint32_t rd = c.get(9, 0, 5);
  if (!block_count) return;
  if (wr >= block_count) wr = 0;
  // Anel como o RingBuffer do Xenia: read == write = vazio (capacidade inteira livre).
  uint32_t free_blocks = rd == wr ? block_count : (rd > wr ? rd - wr : block_count - wr + rd);
  static const uint32_t kRates[4] = {24000, 32000, 44100, 48000};
  const uint32_t rate = kRates[c.get(1, 27, 2)];
  uint8_t* out = phys_ptr(c.d[7]);
  apu::XmaStream& s = g_streams[i];
  bool wrote = false;
  constexpr size_t kBlockSamples = kBlockBytes / 2;
  for (int guard = 0; free_blocks > 0 && guard < 8192; guard++) {
    // PCM já descodificado → anel de saída, um bloco de 256 bytes de cada vez (int16 BE intercalado).
    if (s.pcm.size() - s.pcm_pos >= kBlockSamples) {
      uint8_t* dst = out + wr * kBlockBytes;
      const int16_t* src = s.pcm.data() + s.pcm_pos;
      for (size_t k = 0; k < kBlockSamples; k++) {
        uint16_t v = (uint16_t)src[k];
        dst[2 * k] = (uint8_t)(v >> 8);
        dst[2 * k + 1] = (uint8_t)v;
      }
      s.pcm_pos += kBlockSamples;
      wr = (wr + 1) % block_count;
      free_blocks--;
      wrote = true;
      continue;
    }
    // Falta PCM: descodifica o pacote seguinte do buffer de entrada atual.
    uint32_t cur = c.get(4, 31, 1);
    if (!c.get(0, 20 + cur, 1)) {
      if (!c.get(0, 20 + (cur ^ 1), 1)) {  // à espera que o jogo entregue mais pacotes
        g_stat_starved++;
        s.dbg_starved++;
        break;
      }
      swap_input(c);
      continue;
    }
    uint32_t packet_count = cur ? c.get(1, 0, 12) : c.get(0, 0, 12);
    uint32_t packet = c.get(2, 0, 26) / kPacketBits;
    if (packet >= packet_count) {
      swap_input(c);
      continue;
    }
    const uint8_t* pkt = phys_ptr(c.d[5 + cur]) + packet * kPacketBytes;
    const size_t pcm_before = s.pcm.size(), pos_before = s.pcm_pos;
    apu::xma_decode_packet(s, pkt, channels, rate);
    // Loop a meio do pacote: o FFmpeg entrega o pacote inteiro, mas os quadros a partir de loop_end já não fazem parte do
    // loop (até 0,2 s de outro som a cada volta). Retira-os do PCM.
    if (const uint32_t lc = c.get(0, 12, 8), le = c.get(4, 0, 26); lc && le > c.get(3, 0, 26) && (packet + 1) * kPacketBits > le && !pkt[3]) {
      const uint32_t drop = frames_from(pkt, le - packet * kPacketBits);
      const size_t samples = (size_t)drop * 512 * channels, got = s.pcm_pos == pos_before ? s.pcm.size() - pcm_before : s.pcm.size();
      if (samples && samples <= got) {
        s.pcm.resize(s.pcm.size() - samples);
        g_stat_trimmed += drop;
      }
    }
    if (const char* dd = g_dump_dir) {
      char name[260];
      std::snprintf(name, sizeof name, "%s/c%03u.pcm", dd, i);
      if (FILE* f = std::fopen(name, "ab")) {
        const size_t from = s.pcm.size() >= pcm_before && s.pcm_pos == pos_before ? pcm_before : 0;
        std::fwrite(s.pcm.data() + from, 2, s.pcm.size() - from, f);
        std::fclose(f);
      }
      std::snprintf(name, sizeof name, "%s/c%03u.txt", dd, i);
      if (FILE* f = std::fopen(name, "a")) {
        std::fprintf(f, "P %u pkt=%u cnt=%u skip=%u ch=%u rate=%u frames=%u first=%u\n", cur, packet, packet_count, pkt[3], channels, rate, pkt[0] >> 2, ((pkt[0] & 3) << 13) | (pkt[1] << 5) | (pkt[2] >> 3));
        std::fclose(f);
      }
    }
    // packet_skip (último byte do cabeçalho): pacotes de outros fluxos intercalados até ao seguinte deste contexto.
    // 0xFF = não há mais pacotes deste fluxo no buffer.
    const uint32_t skip = pkt[3], next = skip == 0xFF ? packet_count : packet + 1 + skip;
    g_stat_packets++;
    if (skip) g_stat_skips++;
    // Loop por hardware (loop_count 255 = infinito): os pontos são posições em bits de quadros; aqui a passagem é por
    // pacote, ao fim do pacote que contém loop_end, porque o descodificador só recebe pacotes inteiros.
    const uint32_t loop_count = c.get(0, 12, 8), loop_start = c.get(3, 0, 26), loop_end = c.get(4, 0, 26);
    static const bool no_loop = std::getenv("GOW_XMA_NOLOOP") != nullptr;
    if (!no_loop && loop_count && loop_end > loop_start && (packet + 1) * kPacketBits > loop_end) {
      if (g_trace) std::fprintf(stderr, "[xma] ctx %u loop: count %u bits %u..%u (pacotes %u..%u de %u)\n", i, loop_count, loop_start, loop_end, loop_start / kPacketBits, loop_end / kPacketBits, packet_count);
      if (loop_count != 255) c.set(0, 12, 8, loop_count - 1);
      c.set(2, 0, 26, loop_start);
      // A partir do 1º quadro de um pacote o fluxo continua limpo: sem flush, a sobreposição do MDCT mantém-se e não há
      // um buraco a cada volta. Só um início a meio de um pacote é que não é contíguo.
      if (loop_start % kPacketBits != 32) apu::xma_flush(s);
      g_stat_jumps++;
      continue;
    }
    // Fim do buffer: só se marca como consumido (swap_input, no ramo "falta PCM") depois de o PCM deste pacote chegar ao anel.
    // Marcá-lo logo, com o anel cheio e o resto na fila do host, fazia o XAudio dar a voz por acabada e libertá-la a meio.
    c.set(2, 0, 26, next >= packet_count ? packet_count * kPacketBits : next * kPacketBits + 32);
  }
  c.set(0, 27, 5, wr);
  if (wrote && wr == rd) c.set(1, 31, 1, 0);  // anel cheio
}

void apply(uint32_t base_reg, int kind) {
  for (uint32_t g = 0; g < 10; g++) {
    uint32_t m = (uint32_t)InterlockedExchange(reg(base_reg + g), 0);
    while (m) {
      uint32_t bit = (uint32_t)__builtin_ctz(m);
      m &= m - 1;
      uint32_t i = g * 32 + bit;
      if (i >= kCtxCount) continue;
      if (kind == 0) {
        g_enabled[i] = false;  // Lock
      } else if (kind == 1) {  // Clear
        debug_summary(i, "clear");
        Ctx c;
        load(i, c);
        c.set(0, 20, 2, 0);
        c.set(1, 31, 1, 0);
        c.set(9, 0, 5, 0);
        c.set(0, 27, 5, 0);
        store(i, c);
        apu::xma_reset(g_streams[i]);
      } else {
        g_enabled[i] = true;  // Kick
        g_kicks.fetch_add(1, std::memory_order_relaxed);
        g_dbg_kicks[i]++;
      }
      if (g_trace) std::fprintf(stderr, "[xma] %s contexto %u\n", kind == 0 ? "lock" : kind == 1 ? "clear" : "kick", i);
    }
  }
}

DWORD WINAPI xma_thread(LPVOID) {

  for (;;) {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      apply(kRegLock, 0);
      apply(kRegClear, 1);
      apply(kRegKick, 2);
      for (uint32_t i = 0; i < kCtxCount; i++) {
        if (!g_enabled[i].load(std::memory_order_relaxed)) continue;
        Ctx c;
        load(i, c);
        decode(i, c);
        store(i, c);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

namespace apu {

void xma_stats(uint64_t& packets, uint64_t& skips, uint64_t& starved, uint64_t& loops) {
  std::lock_guard<std::mutex> lock(g_mu);
  packets = g_stat_packets, skips = g_stat_skips, starved = g_stat_starved, loops = g_stat_jumps;
  if (g_trace) std::fprintf(stderr, "[xma] %llu quadros cortados no fim de loops\n", (unsigned long long)g_stat_trimmed);
}

void xma_init() {
  apu::xma_decoder_init();
  g_trace = std::getenv("GOW_TRACE_XMA") != nullptr;
  g_debug_drops = std::getenv("GOW_DEBUG_AUDIO_DROPS") != nullptr;
  g_dump_dir = std::getenv("GOW_XMA_DUMP_DIR");
  g_array_va = gm::physical_alloc(kCtxSize * kCtxCount, gm::kPageReadWrite, 0, 0, 0x1000);
  g_array_phys = gm::physical_address(g_array_va);
  std::memset(phys_ptr(g_array_phys), 0, kCtxSize * kCtxCount);
  *reg(kRegArray) = (LONG)g_array_phys;  // little-endian, lido com lwbrx
  std::fprintf(stderr, "[xma] tabela de contextos: virtual 0x%08X físico 0x%08X\n", g_array_va, g_array_phys);
  // Acima das threads do jogo e do FXC: uma passagem atrasada deixa a voz sem amostras (um corte audível). Só código do host,
  // sem esperas por locks do convidado, por isso não há inversão de prioridade.
  HANDLE h = CreateThread(nullptr, 1u << 20, xma_thread, nullptr, 0, nullptr);
  if (h) {
    SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
    CloseHandle(h);
  }
}

}  // namespace apu

// XMACreateContext(PDWORD Context) -> NTSTATUS: contexto livre da tabela publicada em ContextArrayAddress.
XBOX_IMPORT(XMACreateContext) {
  std::lock_guard<std::mutex> lock(g_mu);
  for (uint32_t i = 0; i < kCtxCount && g_array_va; i++) {
    if (g_used[i]) continue;
    g_used[i] = true;
    g_enabled[i] = false;
    apu::xma_reset(g_streams[i]);
    for (uint32_t o = 0; o < kCtxSize; o += 4) ppc_st32(mem_base, ctx_va(i) + o, 0);
    out32(mem_base, arg(ctx, 0), ctx_va(i));
    return ret_status(ctx, kStatusSuccess);
  }
  ret_status(ctx, kStatusNoMemory);
}

// XMAReleaseContext(PVOID Context)
XBOX_IMPORT(XMAReleaseContext) {
  std::lock_guard<std::mutex> lock(g_mu);
  uint32_t c = arg(ctx, 0);
  if (c >= g_array_va && c < g_array_va + kCtxSize * kCtxCount) {
    uint32_t i = (c - g_array_va) / kCtxSize;
    debug_summary(i, "libertado");
    g_used[i] = false;
    g_enabled[i] = false;
  }
}
