// xma.cpp — descodificador XMA por hardware (bloco MMIO 0x7FEA0000).
//
// O XAudio2 do título fala diretamente com o hardware (como no Xenia, xma_decoder.cc / xma_register_table.inc):
//   * registrador N em 0x7FEA0000 + 4N, guardado em little-endian (o jogo usa lwbrx/stwbrx);
//   * 0x600 ContextArrayAddress = endereço físico da tabela de 320 contextos de 64 bytes (XMA_CONTEXT_DATA);
//   * 0x606 CurrentContextIndex (contexto em processamento; o jogo compara-o ^0x200 com os seus ids);
//   * 0x650..0x659 Kick, 0x690..0x699 Lock, 0x6A0..0x6A9 Clear: máscaras de 32 contextos por registrador.
// O jogo escreve Kick/Lock/Clear com stwbrx; o ppc_st32r desvia essas escritas para xma_mmio_store, que as aplica antes de o
// jogo continuar (um Clear aplicado 1 ms depois apagava a configuração que o jogo já tinha escrito para a voz nova). O resto
// do bloco é RAM simples, e a thread do XMA ainda varre os registos de comando a cada ~1 ms. Um Kick ativa o contexto até ao Lock seguinte (como Enable/Disable
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
#include <algorithm>
#include <cstring>
#include <vector>
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
uint64_t g_stat_packets = 0, g_stat_skips = 0, g_stat_starved = 0, g_stat_jumps = 0, g_stat_drains = 0;
apu::XmaStream g_streams[kCtxCount];  // estado do descodificador por contexto (host)
uint64_t g_dbg_out_invalid[kCtxCount] = {}, g_dbg_kicks[kCtxCount] = {}, g_dbg_full_kept[kCtxCount] = {};
// Anel de saída deixado cheio pela última passagem (read == write também é "vazio"): enquanto o jogo não mexer no read offset,
// não há espaço, mesmo que volte a marcar a saída como válida.
bool g_ring_full[kCtxCount] = {};
bool g_wrote_since_empty[kCtxCount] = {};
uint64_t g_stat_voice_empty = 0;

// GOW_XMA_DUMP_DIR: por instância de voz (contexto entre Create/Clear), o PCM escrito no anel do jogo (int16 nativo) e um
// registo de eventos com a posição, em amostras escritas, onde começa o PCM do evento (pacote, loop, troca de buffer, flush).
uint32_t g_dump_seq[kCtxCount] = {};
uint64_t g_dump_written[kCtxCount] = {};
FILE* g_dump_pcm[kCtxCount] = {};
FILE* g_dump_txt[kCtxCount] = {};
uint32_t g_dump_next_seq = 0;
void dump_open(uint32_t i) {
  if (!g_dump_dir) return;
  if (g_dump_pcm[i]) std::fclose(g_dump_pcm[i]);
  if (g_dump_txt[i]) std::fclose(g_dump_txt[i]);
  g_dump_seq[i] = g_dump_next_seq++;
  g_dump_written[i] = 0;
  char name[260];
  std::snprintf(name, sizeof name, "%s/v%05u_c%03u.pcm", g_dump_dir, g_dump_seq[i], i);
  g_dump_pcm[i] = std::fopen(name, "wb");
  std::snprintf(name, sizeof name, "%s/v%05u_c%03u.txt", g_dump_dir, g_dump_seq[i], i);
  g_dump_txt[i] = std::fopen(name, "w");
  if (g_dump_txt[i]) std::fprintf(g_dump_txt[i], "0 time %llu 0 0\n", (unsigned long long)GetTickCount64());
}
void dump_event(uint32_t i, const char* what, uint32_t a, uint32_t b, uint32_t c) {
  if (!g_dump_txt[i]) return;
  const apu::XmaStream& s = g_streams[i];
  std::fprintf(g_dump_txt[i], "%llu %s %u %u %u\n", (unsigned long long)(g_dump_written[i] + (s.pcm.size() - s.pcm_pos)), what, a, b, c);
}
void dump_pcm(uint32_t i, const int16_t* p, size_t n) {
  if (!g_dump_pcm[i]) return;
  std::fwrite(p, 2, n, g_dump_pcm[i]);
  g_dump_written[i] += n;
}

// Resumo de um contexto quando o jogo o limpa ou liberta: quanto entrou, quanto saiu e o que ficou por tocar.
void debug_summary(uint32_t i, const char* why) {
  apu::XmaStream& s = g_streams[i];
  if (!g_debug_drops || (!s.dbg_packets && !g_dbg_kicks[i])) return;
  std::fprintf(stderr, "[audio-drops] ctx %u %s: %u canais %u Hz, %llu kicks, %llu pacotes, %llu amostras, %llu recusados, %llu erros de "
                       "receive, %llu falhas de open, %llu esperas, %llu passagens sem saída válida, %llu com o anel ainda cheio, %zu amostras por escrever descartadas\n",
               i, why, s.channels, s.rate, (unsigned long long)g_dbg_kicks[i], (unsigned long long)s.dbg_packets,
               (unsigned long long)s.dbg_samples, (unsigned long long)s.dbg_rejected, (unsigned long long)s.dbg_recv_err,
               (unsigned long long)s.dbg_open_fail, (unsigned long long)s.dbg_starved, (unsigned long long)g_dbg_out_invalid[i],
               (unsigned long long)g_dbg_full_kept[i], s.pcm.size() - s.pcm_pos);
  // Estado que o jogo vê nesse momento: entradas por consumir, saída válida, anel (leitura/escrita/blocos), ativo.
  uint32_t d[16];
  for (int w = 0; w < 16; w++) d[w] = ppc_ld32(gm::base(), g_array_va + i * kCtxSize + 4 * w);
  std::fprintf(stderr, "[audio-drops]   entradas válidas %u/%u (atual %u, pacote %u de %u/%u), saída válida %u, anel lido %u escrito %u de %u, loop %u, %s\n",
               (d[0] >> 20) & 1, (d[0] >> 21) & 1, d[4] >> 31, (d[2] & 0x3FFFFFF) / kPacketBits, d[0] & 0xFFF, d[1] & 0xFFF,
               d[1] >> 31, d[9] & 31, (d[0] >> 27) & 31, (d[0] >> 22) & 31, (d[0] >> 12) & 0xFF, g_enabled[i] ? "ativo" : "trancado");
  s.dbg_packets =s.dbg_samples = s.dbg_rejected = s.dbg_recv_err = s.dbg_open_fail = s.dbg_starved = 0;
  g_dbg_out_invalid[i] = g_dbg_kicks[i] = g_dbg_full_kept[i] = 0;
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

std::atomic<uint64_t> g_dbg_conflicts{0};  // palavras que o jogo mudou durante uma passagem (GOW_DEBUG_AUDIO_DROPS)

// Fim de uma passagem: só os bits que a passagem mudou, por compare-and-swap sobre o valor atual. O XAudio do jogo escreve no
// contexto sem Lock enquanto ele está ativo (buffers de entrada novos e válidos, deslocamento de leitura da saída, saída válida
// outra vez); gravar as 16 palavras lidas no início apagava essas escritas, e a voz ficava sem o buffer que o jogo tinha entregue.
void store_changes(uint32_t i, const Ctx& before, const Ctx& c) {
  for (int w = 0; w < 16; w++) {
    const uint32_t changed = before.d[w] ^ c.d[w];
    auto* p = reinterpret_cast<volatile LONG*>(PPC_HOST(gm::base(), ctx_va(i) + 4 * w));
    if (!changed) {
      if (_byteswap_ulong((uint32_t)*p) != before.d[w]) g_dbg_conflicts.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    for (;;) {
      const uint32_t cur_be = (uint32_t)*p, cur = _byteswap_ulong(cur_be);
      if (cur != before.d[w]) g_dbg_conflicts.fetch_add(1, std::memory_order_relaxed);
      const uint32_t next = (cur & ~changed) | (c.d[w] & changed);
      if ((uint32_t)InterlockedCompareExchange(p, (LONG)_byteswap_ulong(next), (LONG)cur_be) == cur_be) break;
    }
  }
}

void swap_input(Ctx& c) {
  uint32_t cur = c.get(4, 31, 1);
  c.set(0, 20 + cur, 1, 0);
  c.set(4, 31, 1, cur ^ 1);
  c.set(2, 0, 26, 0);
}

// Pacote só com os bits [from, to) (from = início de um quadro ou 32; to = fim de um quadro ou do pacote).
// Devolve o próprio pacote quando não há nada a retirar, out quando foi reescrito, nullptr quando não sobra nada.
const uint8_t* packet_part(const uint8_t* pkt, uint32_t from, uint32_t to, uint8_t* out) {
  const uint32_t ffo = ((pkt[0] & 3u) << 13) | (pkt[1] << 5) | (pkt[2] >> 3), first = 32 + ffo, frames = pkt[0] >> 2;
  if (from < 32) from = 32;
  if (to > kPacketBits) to = kPacketBits;
  if (from <= 32 && to >= kPacketBits) return pkt;
  if (to <= from) return nullptr;
  auto bit_at = [&](uint32_t b) { return (pkt[b >> 3] >> (7 - (b & 7))) & 1u; };
  // Quadros que começam em [max(from, first), to).
  uint32_t kept = 0, at = first;
  for (uint32_t k = 0; k < frames && at + 15 <= kPacketBits; k++) {
    if (at >= to) break;
    if (at >= from) kept++;
    uint32_t len = 0;
    for (uint32_t j = 0; j < 15; j++) len = len << 1 | bit_at(at + j);
    if (len < 15 || len == 0x7FFF) break;
    at += len;
  }
  std::memset(out, 0, kPacketBytes);
  out[3] = pkt[3];
  if (from <= 32) {  // mantém o início (resto do quadro anterior + quadros) e corta a partir de to
    std::memcpy(out, pkt, (to + 7) / 8);
    if (to & 7) out[to >> 3] &= (uint8_t)(0xFF00 >> (to & 7));
    out[0] = (uint8_t)((kept << 2) | (pkt[0] & 3));
    return out;
  }
  // Começa a meio: os quadros a partir de from passam para o bit 32 (first_frame_offset 0).
  out[0] = (uint8_t)(kept << 2);
  out[1] = 0;
  out[2] = 0;
  for (uint32_t b = from, w = 32; b < to; b++, w++)
    if (bit_at(b)) out[w >> 3] |= (uint8_t)(0x80 >> (w & 7));
  return out;
}

// Uma passagem de descodificação (Work do Xenia): enche o anel de saída a partir do buffer de entrada atual.
void decode(uint32_t i, Ctx& c) {
  if (!c.get(1, 31, 1)) {  // saída inválida
    g_dbg_out_invalid[i]++;
    return;
  }
  apu::XmaStream& st = g_streams[i];
  constexpr size_t kBlockSamples = kBlockBytes / 2;
  // Sem entrada (o jogo tirou-a): o que o descodificador ainda guarda sai, e o resto do último quadro (menos de um bloco) é
  // completado com silêncio, em vez de se perder.
  if (!c.get(0, 20, 1) && !c.get(0, 21, 1) && st.fed) {
    apu::xma_drain(st);
    g_stat_drains++;
  }
  if (!c.get(0, 20, 1) && !c.get(0, 21, 1) && st.pcm.size() > st.pcm_pos && (st.pcm.size() - st.pcm_pos) % kBlockSamples)
    st.pcm.resize(st.pcm.size() + kBlockSamples - (st.pcm.size() - st.pcm_pos) % kBlockSamples, 0);
  if (!c.get(0, 20, 1) && !c.get(0, 21, 1) && st.pcm.size() - st.pcm_pos < kBlockSamples) {
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
  if (g_ring_full[i] && rd == wr) {  // ainda cheio: nada lido desde a última passagem
    g_dbg_full_kept[i]++;
    return;
  }
  g_ring_full[i] = false;
  // Anel vazio com a voz ainda a tocar: o XAudio leu tudo antes de a passagem seguinte o voltar a encher (um corte audível).
  if (free_blocks == block_count && g_wrote_since_empty[i] && (c.get(0, 20, 1) || c.get(0, 21, 1) || st.pcm.size() > st.pcm_pos)) {
    g_wrote_since_empty[i] = false;
    g_stat_voice_empty++;
    dump_event(i, "empty", rd, wr, 0);
  }
  static const uint32_t kRates[4] = {24000, 32000, 44100, 48000};
  const uint32_t rate = kRates[c.get(1, 27, 2)];
  uint8_t* out = phys_ptr(c.d[7]);
  apu::XmaStream& s = g_streams[i];
  bool wrote = false;
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
      dump_pcm(i, src, kBlockSamples);
      s.pcm_pos += kBlockSamples;
      wr = (wr + 1) % block_count;
      free_blocks--;
      wrote = true;
      g_wrote_since_empty[i] = true;
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
      // Fim da entrada: o "xma2" do FFmpeg retém sempre 4096 amostras na sua FIFO (alinha fluxos intercalados) e só as entrega
      // com mais pacotes ou com o fim do fluxo. Sem este EOF o fim de cada som (85-170 ms) perdia-se e sons de menos de 4096
      // amostras não tocavam. Com o outro buffer válido o fluxo continua e não se esvazia. O resto completa-se com silêncio até
      // ao bloco, e o buffer só é dado como consumido depois de esse PCM chegar ao anel.
      if (s.fed && !c.get(0, 20 + (cur ^ 1), 1)) {
        apu::xma_drain(s);
        g_stat_drains++;
        dump_event(i, "drain", 0, 0, 0);
        if (s.pcm.size() > s.pcm_pos && (s.pcm.size() - s.pcm_pos) % kBlockSamples)
          s.pcm.resize(s.pcm.size() + kBlockSamples - (s.pcm.size() - s.pcm_pos) % kBlockSamples, 0);
        continue;
      }
      swap_input(c);
      continue;
    }
    const uint8_t* pkt = phys_ptr(c.d[5 + cur]) + packet * kPacketBytes;
    dump_event(i, "pkt", cur, packet, pkt[3]);
    // Loop por hardware (loop_count 255 = infinito): loop_start/loop_end são posições em bits de quadros (loop_end não incluído).
    static const bool no_loop = std::getenv("GOW_XMA_NOLOOP") != nullptr;
    const uint32_t loop_count = c.get(0, 12, 8), loop_start = c.get(3, 0, 26), loop_end = c.get(4, 0, 26);
    const bool loop_here = !no_loop && loop_count && loop_end > loop_start && (packet + 1) * kPacketBits > loop_end;
    // Ao descodificador só vão os quadros [deslocamento de leitura, loop_end) deste pacote: o pacote de loop_end sem os quadros
    // de fora do loop e o de loop_start (a meio de um pacote) sem os anteriores. O fluxo continua sem flush, como no hardware.
    // Antes o pacote ia inteiro e cortava-se o PCM a seguir, mas com a FIFO do FFmpeg o PCM que sai é de 4096 amostras antes:
    // tirava-se o sítio errado (um clique em cada volta) e os quadros depois de loop_end tocavam na volta seguinte.
    {
      const uint32_t from = c.get(2, 0, 26) - packet * kPacketBits;
      const uint32_t to = loop_here && loop_end >= packet * kPacketBits ? loop_end - packet * kPacketBits : kPacketBits;
      uint8_t part[kPacketBytes];
      if (const uint8_t* feed = packet_part(pkt, from, to, part)) {
        apu::xma_decode_packet(s, feed, channels, rate);
        s.fed = true;
      }
    }
    // packet_skip (último byte do cabeçalho): pacotes de outros fluxos intercalados até ao seguinte deste contexto.
    // 0xFF = não há mais pacotes deste fluxo no buffer.
    const uint32_t skip = pkt[3], next = skip == 0xFF ? packet_count : packet + 1 + skip;
    g_stat_packets++;
    if (skip) g_stat_skips++;
    if (loop_here) {
      if (g_trace) std::fprintf(stderr, "[xma] ctx %u loop: count %u bits %u..%u (pacotes %u..%u de %u)\n", i, loop_count, loop_start, loop_end, loop_start / kPacketBits, loop_end / kPacketBits, packet_count);
      if (loop_count != 255) c.set(0, 12, 8, loop_count - 1);
      c.set(2, 0, 26, loop_start);
      dump_event(i, "loop", loop_start, loop_end, 0);
      g_stat_jumps++;
      continue;
    }
    // Fim do buffer: só se marca como consumido (swap_input, no ramo "falta PCM") depois de o PCM deste pacote chegar ao anel.
    // Marcá-lo logo, com o anel cheio e o resto na fila do host, fazia o XAudio dar a voz por acabada e libertá-la a meio.
    c.set(2, 0, 26, next >= packet_count ? packet_count * kPacketBits : next * kPacketBits + 32);
  }
  c.set(0, 27, 5, wr);
  if (wrote && wr == rd) {  // anel cheio
    c.set(1, 31, 1, 0);
    g_ring_full[i] = true;
  }
}


void work(uint32_t i) {
  Ctx c;
  load(i, c);
  const Ctx before = c;
  decode(i, c);
  store_changes(i, before, c);
}

// kind: 0 = Lock, 1 = Clear, 2 = Kick; m = máscara do registo g (32 contextos).
void apply_mask(uint32_t g, uint32_t m, int kind) {
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
      g_ring_full[i] = false;
      g_wrote_since_empty[i] = false;
      dump_open(i);
    } else {  // Kick
      g_enabled[i] = true;
      g_kicks.fetch_add(1, std::memory_order_relaxed);
      g_dbg_kicks[i]++;
    }
    if (g_trace) std::fprintf(stderr, "[xma] %s contexto %u\n", kind == 0 ? "lock" : kind == 1 ? "clear" : "kick", i);
  }
}

void apply(uint32_t base_reg, int kind) {
  for (uint32_t g = 0; g < 10; g++) apply_mask(g, (uint32_t)InterlockedExchange(reg(base_reg + g), 0), kind);
}

DWORD WINAPI xma_thread(LPVOID) {
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      // Comandos que não passaram pelo gancho do stwbrx (xma_mmio_store).
      apply(kRegLock, 0);
      apply(kRegClear, 1);
      apply(kRegKick, 2);
      for (uint32_t i = 0; i < kCtxCount; i++)
        if (g_enabled[i].load(std::memory_order_relaxed)) work(i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

}  // namespace

namespace apu {

void xma_stats(uint64_t& packets, uint64_t& skips, uint64_t& starved, uint64_t& loops) {
  std::lock_guard<std::mutex> lock(g_mu);
  packets = g_stat_packets, skips = g_stat_skips, starved = g_stat_starved, loops = g_stat_jumps;
  std::fprintf(stderr, "[xma] %llu vozes com o anel de saída vazio a meio\n", (unsigned long long)g_stat_voice_empty);
  if (g_debug_drops)
    for (uint32_t i = 0; i < kCtxCount; i++) {
      if (!g_used[i]) continue;
      uint32_t d[16];
      for (int w = 0; w < 16; w++) d[w] = ppc_ld32(gm::base(), g_array_va + i * kCtxSize + 4 * w);
      const apu::XmaStream& s = g_streams[i];
      std::fprintf(stderr, "[audio-drops] estado ctx %u: in %u/%u atual %u off %u (pkt %u de %u/%u) out_valid %u anel r%u w%u/%u loop %u %u..%u st%u %s pend %zu fed %u exh %u full %u kicks %llu w1 %08X w9 %08X\n", i,
                   (d[0] >> 20) & 1, (d[0] >> 21) & 1, d[4] >> 31, d[2] & 0x3FFFFFF, (d[2] & 0x3FFFFFF) / kPacketBits, d[0] & 0xFFF, d[1] & 0xFFF,
                   d[1] >> 31, d[9] & 31, (d[0] >> 27) & 31, (d[0] >> 22) & 31, (d[0] >> 12) & 0xFF, d[3] & 0x3FFFFFF, d[4] & 0x3FFFFFF,
                   (d[1] >> 29) & 1, g_enabled[i] ? "ativo" : "trancado", s.pcm.size() - s.pcm_pos, s.fed, 0u, g_ring_full[i],
                   (unsigned long long)g_dbg_kicks[i], d[1], d[9]);
    }
  if (g_debug_drops)
    std::fprintf(stderr, "[audio-drops] %llu palavras de contexto mudadas pelo jogo durante uma passagem (preservadas)\n",
                 (unsigned long long)g_dbg_conflicts.load());
}

// Escrita stwbrx no bloco MMIO do XMA (gancho do ppc_st32r). Kick/Lock/Clear aplicam-se já, com o contexto atualizado
// antes de o jogo continuar; os outros registos são RAM.
void xma_mmio_store(uint32_t ea, uint32_t v) {
  const uint32_t r = (ea - gm::kXmaRegBase) / 4;
  if ((ea & 3) == 0 && g_array_va) {
    const int kind = r >= kRegLock && r < kRegLock + 10 ? 0 : r >= kRegClear && r < kRegClear + 10 ? 1 : r >= kRegKick && r < kRegKick + 10 ? 2 : -1;
    if (kind >= 0) {
      const uint32_t base = kind == 0 ? kRegLock : kind == 1 ? kRegClear : kRegKick;
      std::lock_guard<std::mutex> lock(g_mu);
      apply_mask(r - base, v, kind);
      return;
    }
  }
  std::memcpy(PPC_HOST(gm::base(), ea), &v, 4);
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
    if (g_debug_drops) {
      uint32_t n = 0;
      for (uint32_t k = 0; k < kCtxCount; k++) n += g_used[k];
      std::fprintf(stderr, "[audio-drops] t=%llu cria ctx %u (%u em uso)\n", (unsigned long long)GetTickCount64(), i, n);
    }
    g_enabled[i] = false;
    apu::xma_reset(g_streams[i]);
    g_ring_full[i] = false;
    dump_open(i);
    g_wrote_since_empty[i] = false;
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
