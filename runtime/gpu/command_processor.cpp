// command_processor.cpp — processador de comandos PM4 da Xenos, sem renderização.
//
// Uma thread host consome o ring buffer primário (CP_RB_WPTR, escrito pelo D3D) e interpreta só o que o CPU
// observa: escritas em memória (MEM_WRITE, EVENT_WRITE_SHD/EXT, COND_WRITE, registradores de scratch com
// write-back), esperas (WAIT_REG_MEM), buffers indiretos e interrupções (PM4_INTERRUPT). Desenhos, shaders e
// estado são ignorados. Depois de cada lote atualiza CP_RB_RPTR e o write-back do RPTR.
//
// Interrupções gráficas: a callback de VdSetGraphicsInterruptCallback(cb, data) é chamada como
// cb(source, data) numa thread convidada dedicada — source 0 = VBlank (60 Hz, CPU 2), 1 = PM4_INTERRUPT (uma
// entrega por bit da máscara de CPUs do pacote). A callback lê a CPU de KPCR+0x10C, que é ajustado a cada entrega.
//
// Registradores 0..0x3FFF vivem no bloco MMIO (BE, partilhado com o CPU); 0x4000+ (constantes ALU/fetch/bool/
// loop) só no host.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <thread>
#include <vector>

#include "config.h"
#include "gpu/gpu.h"
#include "gpu/renderer.h"
#include "guest_memory.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"

namespace gpu {
namespace {

// Registradores
constexpr uint32_t kRegCpRbRptr = 0x01C4, kRegCpRbWptr = 0x01C5, kRegScratchUmsk = 0x01DC,
                   kRegScratchAddr = 0x01DD, kRegScratch0 = 0x0578, kRegCoherStatusHost = 0x0A31,
                   kRegVgtEventInitiator = 0x21F9, kRegRbSampleCountAddr = 0x2325;
// Opcodes tipo 3
enum : uint32_t {
  kPm4Nop = 0x10, kPm4RegRmw = 0x21, kPm4DrawIndx = 0x22, kPm4SetConstant = 0x2D, kPm4IndirectBufferPfd = 0x37,
  kPm4WaitRegMem = 0x3C, kPm4MemWrite = 0x3D, kPm4IndirectBuffer = 0x3F, kPm4CondWrite = 0x45,
  kPm4EventWrite = 0x46, kPm4MeInit = 0x48, kPm4Interrupt = 0x54, kPm4SetConstant2 = 0x55,
  kPm4EventWriteShd = 0x58, kPm4EventWriteExt = 0x5A, kPm4EventWriteZpd = 0x5B, kPm4XeSwap = 0x64,
  kPm4ImLoad = 0x27, kPm4ImLoadImmediate = 0x2B, kPm4LoadAluConstant = 0x2F, kPm4DrawIndx2 = 0x36,
  kPm4SetShaderConstants = 0x56,
  kPm4SetBinMask = 0x50, kPm4SetBinSelect = 0x51, kPm4SetBinMaskLo = 0x60, kPm4SetBinMaskHi = 0x61,
  kPm4SetBinSelectLo = 0x62, kPm4SetBinSelectHi = 0x63,
};

std::atomic<uint32_t> g_rb_base{0}, g_rb_dwords{0}, g_rptr_writeback{0};
std::atomic<uint32_t> g_interrupt_cb{0}, g_interrupt_data{0};
std::atomic<uint32_t> g_swap_count{0};
std::atomic<bool> g_started{false};
std::vector<uint32_t> g_high_regs(0x10000 - 0x4000);
std::atomic<uint32_t> g_opcode_seen[128];
// Pacote em execução (diagnóstico): opcode tipo 3, posição do cabeçalho (dwords) e profundidade de IB.
std::atomic<uint32_t> g_cur_op{0}, g_cur_pos{0}, g_cur_depth{0}, g_batch_start{0}, g_batch_end{0};
std::atomic<uint64_t> g_packets{0};
uint32_t g_cur_ib = 0;  // endereço físico do buffer indireto em execução (0 = ring); só a thread da GPU
uint64_t g_bin_mask = ~0ull, g_bin_select = ~0ull;  // predicação (SET_BIN_MASK/SELECT); só a thread da GPU
// WAIT_REG_MEM por condição {função|mem, endereço/registrador, referência, máscara}: chamadas e tempo de espera.
using WaitKey = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>;
struct WaitStat {
  uint64_t count = 0, micros = 0;
};
std::mutex g_wait_stats_mu;
std::map<WaitKey, WaitStat> g_wait_stats;

uint8_t* mem() { return gm::base(); }
uint32_t phys(uint32_t a) { return 0xA0000000u | (a & 0x1FFFFFFFu); }  // físico -> espelho virtual

uint32_t g_scratch_seen[8] = {};  // último valor de cada SCRATCH_REGn já espelhado (só a thread da GPU)

uint32_t reg(uint32_t i) {
  i &= 0xFFFF;
  return i < 0x4000 ? ppc_ld32(mem(), gm::kGpuRegBase + 4 * i) : g_high_regs[i - 0x4000];
}

void set_reg(uint32_t i, uint32_t v) {
  i &= 0xFFFF;
  if (i >= 0x4000) {
    g_high_regs[i - 0x4000] = v;
    return;
  }
  if (i == kRegCoherStatusHost) v &= ~0x80000000u;  // pedido de coerência satisfeito de imediato
  ppc_st32(mem(), gm::kGpuRegBase + 4 * i, v);
  // Registradores de scratch com write-back: SCRATCH_ADDR + 4*n, se o bit n de SCRATCH_UMSK estiver ativo.
  if (i >= kRegScratch0 && i < kRegScratch0 + 8) {
    uint32_t n = i - kRegScratch0;
    g_scratch_seen[n] = v;
    if (reg(kRegScratchUmsk) & (1u << n)) ppc_st32(mem(), phys(reg(kRegScratchAddr) + 4 * n), v);
  }
}

// O CPU também escreve nos registradores de scratch por MMIO e no hardware isso dispara o mesmo write-back;
// como o MMIO é RAM simples, a thread da GPU deteta escritas (valor diferente do último visto) e espelha-as.
// Só o registrador escreve na memória: o CPU pode zerar a cópia em memória sem tocar no registrador (o VBlank
// do D3D faz isso), e essa escrita não pode ser desfeita.
void sync_scratch() {
  uint32_t umsk = reg(kRegScratchUmsk) & 0xFF, addr = reg(kRegScratchAddr);
  for (uint32_t n = 0; n < 8; n++) {
    uint32_t v = reg(kRegScratch0 + n);
    if (v == g_scratch_seen[n]) continue;
    g_scratch_seen[n] = v;
    if (addr && (umsk & (1u << n))) ppc_st32(mem(), phys(addr + 4 * n), v);
  }
}

// Os 2 bits baixos de um endereço PM4 indicam a troca de bytes: 0 nenhuma, 1 8in16, 2 8in32, 3 16in32.
uint32_t gpu_swap(uint32_t v, uint32_t mode) {
  switch (mode & 3) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}
// Valores lidos/escritos como o hardware vê a memória (bytes em ordem "little" + troca pedida).
uint32_t mem_read(uint32_t addr) {
  uint32_t raw;
  std::memcpy(&raw, mem() + phys(addr & ~3u), 4);
  return gpu_swap(raw, addr);
}
void mem_write(uint32_t addr, uint32_t v) {
  uint32_t raw = gpu_swap(v, addr);
  std::memcpy(mem() + phys(addr & ~3u), &raw, 4);
}

bool compare(uint32_t func, uint32_t value, uint32_t ref) {
  switch (func & 7) {
    case 0: return false;
    case 1: return value < ref;
    case 2: return value <= ref;
    case 3: return value == ref;
    case 4: return value != ref;
    case 5: return value >= ref;
    case 6: return value > ref;
    default: return true;
  }
}

// ---- Interrupções (thread convidada dedicada) ----
const bool g_trace_irq = [] {
  const char* e = std::getenv("GOW_TRACE_PM4");
  return e && *e == '1';
}();
std::mutex g_irq_mu;
std::condition_variable g_irq_cv;
struct Irq {
  uint32_t source, cpu;
};
std::deque<Irq> g_irq_queue;

// Devolve o número de ordem da entrega (0 se não foi enfileirada); wait_interrupt espera que seja entregue.
uint64_t g_irq_queued = 0, g_irq_delivered = 0;  // protegidos por g_irq_mu
std::condition_variable g_irq_done_cv;

uint64_t raise_interrupt(uint32_t source, uint32_t cpu) {
  if (!g_interrupt_cb.load()) return 0;
  std::lock_guard<std::mutex> lock(g_irq_mu);
  if (g_irq_queue.size() >= 64) return 0;
  g_irq_queue.push_back({source, cpu});
  g_irq_cv.notify_one();
  return ++g_irq_queued;
}

// PM4_INTERRUPT é síncrono no hardware (e no Xenia): o processador de comandos só continua depois da callback.
// Correr à frente deixa o D3D reciclar command buffers que a sua thread de extents ainda vai ler.
void wait_interrupt(uint64_t ticket) {
  if (!ticket) return;
  std::unique_lock<std::mutex> lock(g_irq_mu);
  g_irq_done_cv.wait_for(lock, std::chrono::seconds(1), [&] { return g_irq_delivered >= ticket; });
}

DWORD WINAPI interrupt_thread(LPVOID) {
  auto* ctx = new PPCContext;
  kernel::GuestThread* t = kernel::create_guest_thread(*ctx, 0x10000);
  std::fprintf(stderr, "[gpu] thread de interrupções = thread convidada %u\n", t->thread_id);
  PPCContext saved = *ctx;
  for (;;) {
    Irq irq;
    {
      std::unique_lock<std::mutex> lock(g_irq_mu);
      g_irq_cv.wait(lock, [] { return !g_irq_queue.empty(); });
      irq = g_irq_queue.front();
      g_irq_queue.pop_front();
    }
    *ctx = saved;  // pilha/r13 limpos a cada entrega
    ppc_st8(mem(), t->pcr + 0x10C, (uint8_t)irq.cpu);
    ctx->r[3] = irq.source;
    ctx->r[4] = g_interrupt_data.load();
    uint32_t pending = ppc_ld32(mem(), g_interrupt_data.load() + 0x2A14);
    uint32_t before = pending ? ppc_ld32(mem(), pending) : 0;
    kernel::run_guest(t, *ctx, g_interrupt_cb.load());
    {
      std::lock_guard<std::mutex> lock(g_irq_mu);
      g_irq_delivered++;
    }
    g_irq_done_cv.notify_all();
    if (irq.source == 1 && g_trace_irq)
      std::fprintf(stderr, "[gpu] irq 1 cpu %u: [0x%08X] 0x%08X -> 0x%08X\n", irq.cpu, pending, before,
                   pending ? ppc_ld32(mem(), pending) : 0);
  }
}

// GOW_TIMELINE=N[,K]: linha do tempo (ms) dos swaps N..N+K (padrão 6): lotes do ring, esperas WAIT_REG_MEM,
// interrupções de VBlank e swaps — para ver em que vblank cai cada frame.
struct Timeline {
  uint32_t first = 0, count = 0;
  bool on = false;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  Timeline() {
    const char* e = std::getenv("GOW_TIMELINE");
    if (!e) return;
    char* end;
    first = (uint32_t)std::strtoul(e, &end, 0);
    count = *end == ',' ? (uint32_t)std::strtoul(end + 1, nullptr, 0) : 6;
    on = true;
  }
};
Timeline g_timeline;
void tl(const char* fmt, ...) {
  if (!g_timeline.on) return;
  uint32_t sc = g_swap_count.load();
  if (sc < g_timeline.first || sc >= g_timeline.first + g_timeline.count) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_timeline.t0).count();
  std::fprintf(stderr, "[tl] %10.2f swap#%u %s\n", ms, sc, buf);
}

void vblank_thread() {
  // O D3D do jogo espera 2 VBlanks por flip: é daí que vem o limite de 30 FPS. O VBlank só serve para esse ritmo, porque o
  // jogo mede o tempo pelo mftb (tempo real): 120 Hz dão 60 FPS sem câmara rápida. GOW_VBLANK_HZ fixa a frequência.
  const char* e = std::getenv("GOW_VBLANK_HZ");
  const double forced = e ? std::atof(e) : 0.0;
  auto next = std::chrono::steady_clock::now();
  // AdaptiveFps: com um limite acima de 30 que a cena não aguenta (o corredor com 6000 desenhos faz 40–50), os frames alternam
  // entre 2, 3 e 4 VBlanks (17/25/33 ms) e a imagem dá solavancos. Nesse caso o VBlank cai para metade (30 FPS certos) e, de
  // tempos a tempos, experimenta-se o limite inteiro durante 2 s: se o ritmo chegar, fica; se não, a próxima tentativa demora o
  // dobro (8 s até 64 s).
  bool half = false, probing = false;
  int low_windows = 0, probe_windows = 0;
  double probe_delay = 8.0, half_since = 0.0;
  auto window_start = next;
  uint32_t window_swaps = g_swap_count.load();
  for (;;) {
    const config::Config& cfg = config::get();
    const int limit = cfg.fps_limit;
    const bool adaptive = cfg.adaptive_fps && forced <= 0 && limit >= 60;
    if (!adaptive) half = probing = false, low_windows = 0;
    const double hz = forced > 0 ? forced : limit > 0 ? (half ? limit : limit * 2.0) : 480.0;  // 0 = ilimitado
    if (const auto now = std::chrono::steady_clock::now(); adaptive && now - window_start >= std::chrono::seconds(1)) {
      const double secs = std::chrono::duration<double>(now - window_start).count();
      const uint32_t sc = g_swap_count.load();
      const double fps = (sc - window_swaps) / secs, want = hz / 2.0;
      window_start = now;
      window_swaps = sc;
      if (!half) {
        if (probing) {
          // 1.ª janela da tentativa: ainda a sair do ritmo anterior; decide-se na 2.ª.
          if (++probe_windows >= 2) {
            probing = false;
            if (fps >= 0.92 * want) {
              probe_delay = 8.0;
              std::fprintf(stderr, "[gpu] FPS adaptativo: %d FPS aguentam-se (%.1f)\n", limit, fps);
            } else {
              half = true, half_since = 0.0, probe_delay = std::min(probe_delay * 2.0, 64.0);
              std::fprintf(stderr, "[gpu] FPS adaptativo: %.1f de %d, volta a %d (próxima tentativa em %.0f s)\n", fps, limit, limit / 2, probe_delay);
            }
          }
        } else if (fps < 0.85 * want && fps > 1.0) {
          if (++low_windows >= 2) {
            half = true, half_since = 0.0, low_windows = 0;
            std::fprintf(stderr, "[gpu] FPS adaptativo: %.1f de %d, passa a %d\n", fps, limit, limit / 2);
          }
        } else {
          low_windows = 0;
        }
      } else if ((half_since += secs) >= probe_delay) {
        half = false, probing = true, probe_windows = 0;
      }
    }
    if (const double hz2 = forced > 0 ? forced : limit > 0 ? (half ? limit : limit * 2.0) : 480.0; hz2 != hz) continue;
    const auto period = std::chrono::microseconds((int64_t)(1e6 / hz));
    next += period;
    // Dorme até ~1 ms antes e acaba com yields: o VBlank tem de cair a 16,67 ms certos (um VBlank perdido atrasa o
    // flip do D3D um VBlank inteiro).
    std::this_thread::sleep_until(next - std::chrono::milliseconds(1));
    while (std::chrono::steady_clock::now() < next) std::this_thread::yield();
    if (std::chrono::steady_clock::now() - next > period) next = std::chrono::steady_clock::now();  // muito atrasado
    tl("vblank");
    raise_interrupt(0, 2);
  }
}

// ---- Execução de pacotes ----
struct Reader {
  uint32_t base, dwords, pos;  // base virtual; pos em dwords (o ring dá a volta)
  bool ring;
  uint32_t next() {
    uint32_t v = ppc_ld32(mem(), base + 4 * pos);
    pos = ring ? (pos + 1) % dwords : pos + 1;
    return v;
  }
  void skip(uint32_t n) { pos = ring ? (pos + n) % dwords : pos + n; }
};

void execute(Reader& r, uint32_t end, int depth);

void note_opcode(uint32_t op) {
  if (!g_opcode_seen[op].fetch_add(1))
    std::fprintf(stderr, "[gpu] 1º pacote PM4 tipo 3 opcode 0x%02X\n", op);
}

const bool g_trace_pm4 = [] {
  const char* e = std::getenv("GOW_TRACE_PM4");
  return e && *e == '1';
}();

// GOW_PM4_HISTORY=<endereço>: nas 3 primeiras esperas WAIT_REG_MEM por esse endereço, despeja os últimos 24
// pacotes tipo 3 (para ver que comandos rodeiam uma espera).
const uint32_t g_history_addr = [] {
  const char* e = std::getenv("GOW_PM4_HISTORY");
  return e && *e ? (uint32_t)std::strtoul(e, nullptr, 0) : 0u;
}();
struct HistoryEntry {
  uint32_t op, count, depth, d[5];
};
HistoryEntry g_history[24];
uint32_t g_history_pos = 0;

void dump_history(uint32_t addr, bool force = false) {
  static int dumps = 0;
  if (++dumps > 3 && !force) return;
  std::fprintf(stderr, "[pm4] histórico antes da espera em 0x%08X:\n", addr);
  for (uint32_t i = 0; i < 24; i++) {
    const HistoryEntry& h = g_history[(g_history_pos + i) % 24];
    if (!h.count) continue;
    std::fprintf(stderr, "[pm4]   %*sop 0x%02X n=%u: %08X %08X %08X %08X %08X\n", h.depth * 2, "", h.op, h.count,
                 h.d[0], h.d[1], h.d[2], h.d[3], h.d[4]);
  }
}

void execute_type3(Reader& r, uint32_t header, int depth) {
  uint32_t op = (header >> 8) & 0x7F, count = ((header >> 16) & 0x3FFF) + 1;
  g_cur_op = op;
  note_opcode(op);
  if (g_history_addr) {
    HistoryEntry& h = g_history[g_history_pos++ % 24];
    Reader peek = r;
    h = {op, count, (uint32_t)depth, {}};
    for (uint32_t i = 0; i < count && i < 5; i++) h.d[i] = peek.next();
  }
  if (g_trace_pm4 && op != kPm4SetConstant && op != kPm4SetConstant2) {
    Reader peek = r;
    std::fprintf(stderr, "[pm4] %*sop 0x%02X n=%u:", depth * 2, "", op, count);
    for (uint32_t i = 0; i < count && i < 6; i++) std::fprintf(stderr, " %08X", peek.next());
    std::fputc('\n', stderr);
  }
  // Predicated tiling: o D3D reexecuta o mesmo command buffer uma vez por tile da EDRAM, e os pacotes com o
  // bit 0 do cabeçalho só correm nos tiles selecionados (bin_mask & bin_select != 0). Fences, swaps e
  // esperas por VBlank são predicados para correr uma só vez; os swaps predicados nunca correm (como no Xenia).
  if ((header & 1) && ((g_bin_mask & g_bin_select) == 0 || op == kPm4XeSwap)) {
    r.skip(count);
    return;
  }
  uint32_t d[8] = {};
  auto take = [&](uint32_t n) {  // lê n dwords de dados (n <= count), salta o resto no fim
    for (uint32_t i = 0; i < n && i < 8; i++) d[i] = r.next();
    return n;
  };
  uint32_t used = 0;
  switch (op) {
    case kPm4SetBinMaskLo:
      used = take(1);
      g_bin_mask = (g_bin_mask & 0xFFFFFFFF00000000ull) | d[0];
      break;
    case kPm4SetBinMaskHi:
      used = take(1);
      g_bin_mask = (g_bin_mask & 0xFFFFFFFFull) | (uint64_t)d[0] << 32;
      break;
    case kPm4SetBinSelectLo:
      used = take(1);
      g_bin_select = (g_bin_select & 0xFFFFFFFF00000000ull) | d[0];
      break;
    case kPm4SetBinSelectHi:
      used = take(1);
      g_bin_select = (g_bin_select & 0xFFFFFFFFull) | (uint64_t)d[0] << 32;
      break;
    case kPm4SetBinMask:  // hi, lo
      used = take(2);
      g_bin_mask = (uint64_t)d[0] << 32 | d[1];
      break;
    case kPm4SetBinSelect:
      used = take(2);
      g_bin_select = (uint64_t)d[0] << 32 | d[1];
      break;
    case kPm4MemWrite: {
      uint32_t addr = r.next() & 0x3FFFFFFFu;
      for (uint32_t i = 1; i < count; i++, addr += 4) ppc_st32(mem(), phys(addr), r.next());
      used = count;
      break;
    }
    case kPm4EventWriteShd: {  // initiator, address, value (bit 31 do initiator: escreve o contador de swaps)
      used = take(3);
      set_reg(kRegVgtEventInitiator, d[0] & 0x3F);
      mem_write(d[1], (d[0] >> 31) ? g_swap_count.load() : d[2]);
      static const bool trace_ring = std::getenv("GOW_TRACE_RING") != nullptr;
      if (trace_ring) {  // fence que recua = comandos antigos reexecutados
        static std::map<uint32_t, uint32_t> last;
        static int logged = 0;
        uint32_t a = d[1] & ~3u, v = d[2];
        auto it = last.find(a);
        if (it != last.end() && (int32_t)(v - it->second) < 0 && logged < 20) {
          logged++;
          std::fprintf(stderr, "[ring] fence em 0x%08X recua 0x%X -> 0x%X (IB 0x%08X, profundidade %d)\n", a,
                       it->second, v, g_cur_ib, depth);
        }
        last[a] = v;
      }
      break;
    }
    case kPm4EventWriteExt: {  // initiator, address: extents do ecrã (6× u16)
      used = take(2);
      static const uint16_t extents[6] = {0, 8192 >> 3, 0, 8192 >> 3, 0, 1};
      uint32_t a = phys(d[1] & ~3u);
      for (int i = 0; i < 6; i++) ppc_st16(mem(), a + 2 * i, extents[i]);
      break;
    }
    case kPm4EventWrite:
      used = take(1);
      set_reg(kRegVgtEventInitiator, d[0] & 0x3F);
      break;
    case kPm4EventWriteZpd: {  // initiator: fim/início de uma query de oclusão
      // O bloco em RB_SAMPLE_COUNT_ADDR tem 8 contadores u32 little-endian {Total, ZFail, ZPass, StencilFail} × {A, B}.
      // No Issue(END) o D3D marca ZPass (ou ZFail, nas versões antigas do D3D, como a deste título) com 0xFFFFFEED e
      // GetData espera que a GPU os substitua. Sem rasterização, reporta-se uma contagem fixa (como o Xenia).
      used = take(1);
      set_reg(kRegVgtEventInitiator, d[0] & 0x3F);
      static const uint32_t fake = [] {
        const char* e = std::getenv("GOW_FAKE_SAMPLES");
        return e && *e ? (uint32_t)std::strtoul(e, nullptr, 0) : 1000u;
      }();
      uint32_t addr = reg(kRegRbSampleCountAddr);
      if (!addr) break;
      uint8_t* p = mem() + phys(addr & ~3u);
      uint32_t c[8];
      std::memcpy(c, p, sizeof(c));
      const uint32_t marker = __builtin_bswap32(0xFFFFFEEDu);  // bytes FF FF FE ED lidos como LE
      bool end = (c[4] == marker && c[5] == marker) || (c[2] == marker && c[3] == marker);
      std::memset(c, 0, sizeof(c));
      if (end) c[0] = c[4] = fake;  // Total_A, ZPass_A
      std::memcpy(p, c, sizeof(c));
      break;
    }
    case kPm4WaitRegMem: {  // wait_info, poll_addr, ref, mask, wait
      used = take(5);
      // Espera sem limite, como o hardware: o CPU pode demorar segundos (ex.: arma a próxima callback de
      // interrupção só depois de carregar um nível). Saltar a espera dessincroniza o protocolo com o D3D.
      if (g_history_addr && (d[1] & ~3u) == (g_history_addr & ~3u)) dump_history(d[1]);
      auto t0 = std::chrono::steady_clock::now();
      tl("espera %s 0x%08X ref 0x%X: início", (d[0] & 0x10) ? "mem" : "reg", d[1], d[2]);
      auto warn_at = t0 + std::chrono::seconds(5);
      for (;;) {
        uint32_t v = (d[0] & 0x10) ? mem_read(d[1]) : reg(d[1]);
        if (compare(d[0], v & d[3], d[2])) break;
        sync_scratch();
        if (std::chrono::steady_clock::now() > warn_at) {
          std::fprintf(stderr, "[gpu] WAIT_REG_MEM há %llds (%s 0x%08X & 0x%08X, ref 0x%08X, valor 0x%08X)\n",
                       (long long)std::chrono::duration_cast<std::chrono::seconds>(warn_at - t0).count(),
                       (d[0] & 0x10) ? "mem" : "reg", d[1], d[3], d[2], v);
          warn_at += std::chrono::seconds(5);
        }
        std::this_thread::yield();
      }
      {
        std::lock_guard<std::mutex> lock(g_wait_stats_mu);
        WaitStat& ws = g_wait_stats[{d[0] & 0x17, d[1], d[2], d[3]}];
        ws.count++;
        ws.micros += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t0).count();
      }
      tl("espera 0x%08X: fim", d[1]);
      break;
    }
    case kPm4CondWrite: {  // wait_info, poll_addr, ref, mask, write_addr, write_data
      used = take(6);
      uint32_t v = (d[0] & 0x10) ? mem_read(d[1]) : reg(d[1]);
      if (compare(d[0], v & d[3], d[2])) {
        if (d[0] & 0x100) mem_write(d[4], d[5]);
        else set_reg(d[4], d[5]);
      }
      break;
    }
    case kPm4RegRmw: {  // rmw_info, and_mask, or_mask
      used = take(3);
      uint32_t idx = d[0] & 0x1FFF;
      uint32_t andm = (d[0] & (1u << 31)) ? reg(d[1] & 0x1FFF) : d[1];
      uint32_t orm = (d[0] & (1u << 30)) ? reg(d[2] & 0x1FFF) : d[2];
      set_reg(idx, (reg(idx) & andm) | orm);
      break;
    }
    case kPm4SetConstant:
    case kPm4SetConstant2: {  // offset_type, valores...
      uint32_t ot = r.next();
      uint32_t index = ot & 0x7FF;
      static const uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      uint32_t type = (ot >> 16) & 0xFF;
      uint32_t base = type < 5 ? kBase[type] : 0x2000;
      for (uint32_t i = 1; i < count; i++) set_reg(base + index + i - 1, r.next());
      used = count;
      break;
    }
    case kPm4IndirectBuffer:
    case kPm4IndirectBufferPfd: {  // address (físico), size em dwords
      used = take(2);
      if (depth < 4) {
        Reader ib{phys(d[0] & ~3u), 0, 0, false};
        uint32_t outer = g_cur_ib;
        g_cur_ib = d[0];
        execute(ib, d[1] & 0xFFFFF, depth + 1);
        g_cur_ib = outer;
      }
      break;
    }
    case kPm4Interrupt:
      used = take(1);
      for (uint32_t cpu = 0; cpu < 6; cpu++)
        if (d[0] & (1u << cpu)) wait_interrupt(raise_interrupt(1, cpu));
      break;
    case kPm4XeSwap: {  // 'SWAP', frontbuffer físico, largura, altura, constante de fetch do frontbuffer (6 dwords)
      uint32_t w[10] = {};
      for (uint32_t i = 0; i < 10 && i < count; i++) w[i] = r.next();
      used = std::min<uint32_t>(10, count);
      tl("swap: início");
      render::swap(w[1], w[2], w[3], &w[4]);
      g_swap_count.fetch_add(1);
      tl("swap: fim (Present)");
      break;
    }
    case kPm4ImLoad: {  // addr | tipo (0 = vértices, 1 = pixels), start << 16 | tamanho em dwords
      used = take(2);
      uint32_t addr = d[0] & ~3u, n = d[1] & 0xFFFF;
      std::vector<uint32_t> u(n);
      for (uint32_t i = 0; i < n; i++) u[i] = ppc_ld32(mem(), phys(addr + 4 * i));
      render::load_shader((d[0] & 3) == 1, std::move(u));
      break;
    }
    case kPm4ImLoadImmediate: {  // tipo, start << 16 | tamanho, microcódigo
      used = take(2);
      uint32_t n = std::min<uint32_t>(d[1] & 0xFFFF, count - 2);
      std::vector<uint32_t> u(n);
      for (uint32_t i = 0; i < n; i++) u[i] = r.next();
      used += n;
      render::load_shader((d[0] & 3) == 1, std::move(u));
      break;
    }
    case kPm4LoadAluConstant: {  // endereço, offset | tipo << 16, tamanho em dwords: copia constantes da memória
      used = take(3);
      static const uint32_t kBase[] = {0x4000, 0x4800, 0x4900, 0x4908, 0x2000};
      uint32_t type = (d[1] >> 16) & 0xFF, index = d[1] & 0x7FF, n = d[2] & 0xFFF;
      if (type < 5)
        for (uint32_t i = 0; i < n; i++) set_reg(kBase[type] + index + i, ppc_ld32(mem(), phys((d[0] & 0x3FFFFFFFu) + 4 * i)));
      break;
    }
    case kPm4SetShaderConstants: {  // índice de registrador, valores…
      uint32_t index = r.next() & 0xFFFF;
      for (uint32_t i = 1; i < count; i++) set_reg(index++, r.next());
      used = count;
      break;
    }
    case kPm4DrawIndx: {  // viz query, VGT_DRAW_INITIATOR, [VGT_DMA_BASE, VGT_DMA_SIZE]
      used = take(std::min<uint32_t>(count, 4));
      set_reg(0x21FC, d[1]);
      if (((d[1] >> 6) & 3) == 0 && count >= 4) set_reg(0x21FA, d[2]), set_reg(0x21FB, d[3]);
      render::draw(d[1], d[2], d[3], nullptr, 0);
      break;
    }
    case kPm4DrawIndx2: {  // VGT_DRAW_INITIATOR, índices imediatos…
      uint32_t initiator = r.next();
      std::vector<uint32_t> imm(count - 1);
      for (uint32_t& v : imm) v = r.next();
      used = count;
      set_reg(0x21FC, initiator);
      render::draw(initiator, 0, 0, imm.data(), (uint32_t)imm.size());
      break;
    }
    default: {  // estado e pacotes sem efeito no host; regista as primeiras ocorrências (diagnóstico)
      static int seen[128] = {};
      if (op != kPm4Nop && seen[op]++ < 3) {
        Reader peek = r;
        std::fprintf(stderr, "[gpu] opcode 0x%02X não tratado (n=%u):", op, count);
        for (uint32_t i = 0; i < count && i < 8; i++) std::fprintf(stderr, " %08X", peek.next());
        std::fputc('\n', stderr);
      }
      break;
    }
  }
  if (used < count) r.skip(count - used);
}

void execute(Reader& r, uint32_t end, int depth) {
  while (r.pos != end) {
    g_cur_pos = r.pos;
    g_cur_depth = (uint32_t)depth;
    g_packets.fetch_add(1, std::memory_order_relaxed);
    uint32_t h = r.next();
    switch (h >> 30) {
      case 0: {  // escrita em registradores consecutivos (ou num só)
        uint32_t idx = h & 0x7FFF, n = ((h >> 16) & 0x3FFF) + 1;
        bool one = (h >> 15) & 1;
        for (uint32_t i = 0; i < n; i++) {
          uint32_t ri = one ? idx : idx + i, v = r.next();
          if (g_trace_pm4 && ((ri >= kRegScratch0 && ri < kRegScratch0 + 8) || ri == kRegScratchUmsk ||
                              ri == kRegScratchAddr))
            std::fprintf(stderr, "[pm4] %*sreg 0x%04X = 0x%08X\n", depth * 2, "", ri, v);
          set_reg(ri, v);
        }
        break;
      }
      case 1: {
        uint32_t a = h & 0x7FF, b = (h >> 11) & 0x7FF;
        set_reg(a, r.next());
        set_reg(b, r.next());
        break;
      }
      case 2:
        break;
      case 3:
        execute_type3(r, h, depth);
        break;
    }
    if (!r.ring && r.pos > end) break;  // pacote mal formado num buffer indireto
  }
}

void command_processor_thread() {
  render::init();  // janela + D3D12 nesta thread (todo o trabalho D3D12 corre aqui)
  uint8_t* m = mem();
  uint32_t rptr = 0;
  for (;;) {
    uint32_t wptr = reg(kRegCpRbWptr), dwords = g_rb_dwords.load();
    if (dwords && wptr != rptr && wptr < dwords) {
      Reader r{phys(g_rb_base.load()), dwords, rptr, true};
      g_batch_start = rptr;
      g_batch_end = wptr;
      // GOW_TRACE_RING=1: para lotes grandes, quantos dwords são iguais aos da volta anterior do ring (conteúdo
      // velho reexecutado = o D3D avançou o WPTR sem escrever comandos novos).
      static const bool trace_ring = std::getenv("GOW_TRACE_RING") != nullptr;
      if (trace_ring) {
        static std::vector<uint32_t> last(0x10000);
        static int logged = 0;
        uint32_t n = (wptr + dwords - rptr) % dwords, same = 0;
        for (uint32_t i = 0; i < n; i++) {
          uint32_t p = (rptr + i) % dwords, v = ppc_ld32(m, phys(g_rb_base.load()) + 4 * p);
          same += last[p] == v;
          last[p] = v;
        }
        if (n >= 256 && logged < 40) {
          logged++;
          std::fprintf(stderr, "[ring] lote 0x%04X..0x%04X (%u dwords): %u iguais à volta anterior\n", rptr, wptr, n,
                       same);
        }
      }
      tl("lote 0x%04X..0x%04X", rptr, wptr);
      execute(r, wptr, 0);
      tl("lote 0x%04X..0x%04X: fim", rptr, wptr);
      rptr = wptr;
      ppc_st32(m, gm::kGpuRegBase + 4 * kRegCpRbRptr, rptr);
      if (uint32_t wb = g_rptr_writeback.load()) ppc_st32(m, phys(wb), rptr);
      continue;
    }
    sync_scratch();
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
}

}  // namespace

// Valores fixos que o hardware devolve (como no Xenia): VBlank pendente, viewport 1280×720, contador V.
void init_registers() {
  set_reg(0x0F01, 0x0000200E);  // RB_BC_CONTROL
  set_reg(0x194C, 0x000002D0);  // D1MODE_V_COUNTER
  set_reg(0x1951, 0x00000001);  // D1MODE_VBLANK_STATUS (bit 0: VBlank)
  set_reg(0x1961, 0x050002D0);  // D1MODE_VIEWPORT_SIZE
}

void start_ring_buffer(uint32_t physical_base, uint32_t size_log2) {
  g_rb_base = physical_base;
  g_rb_dwords = (1u << (size_log2 + 3)) / 4;
  if (!g_started.exchange(true)) {
    init_registers();
    std::thread(command_processor_thread).detach();
  }
}

void set_rptr_writeback(uint32_t physical) { g_rptr_writeback = physical; }

void set_interrupt_callback(uint32_t cb, uint32_t data) {
  g_interrupt_data = data;
  bool first = !g_interrupt_cb.exchange(cb) && cb;
  std::fprintf(stderr, "[gpu] callback de interrupção 0x%08X (data 0x%08X)\n", cb, data);
  if (first) {
    HANDLE h = CreateThread(nullptr, 16u << 20, interrupt_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION,
                            nullptr);
    if (h) CloseHandle(h);
    std::thread(vblank_thread).detach();
  }
}

uint32_t swap_count() { return g_swap_count.load(); }

uint32_t register_value(uint32_t index) { return reg(index); }
const uint32_t* constant_registers() { return g_high_regs.data(); }

void dump_state() {
  std::fprintf(stderr, "[gpu] renderer: %s\n", render::phase());
  if (g_history_addr) dump_history(0, true);
  std::fprintf(stderr,
               "[gpu] lote 0x%X..0x%X, pacote em 0x%X (profundidade %u), último opcode 0x%02X, %llu pacotes, "
               "%u swaps\n",
               g_batch_start.load(), g_batch_end.load(), g_cur_pos.load(), g_cur_depth.load(), g_cur_op.load(),
               (unsigned long long)g_packets.load(), g_swap_count.load());
  std::lock_guard<std::mutex> lock(g_wait_stats_mu);
  std::vector<std::pair<uint64_t, WaitKey>> v;
  for (auto& [k, st] : g_wait_stats) v.emplace_back(st.micros, k);
  std::sort(v.rbegin(), v.rend());
  for (size_t i = 0; i < v.size() && i < 8; i++) {
    auto [f, a, ref, mask] = v[i].second;
    std::fprintf(stderr, "[gpu]   WAIT_REG_MEM %s 0x%08X & 0x%08X func %u ref 0x%08X: %llu× %.1f ms\n",
                 (f & 0x10) ? "mem" : "reg", a, mask, f & 7, ref,
                 (unsigned long long)g_wait_stats[v[i].second].count, v[i].first / 1000.0);
  }
}

}  // namespace gpu
