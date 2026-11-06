// ppc_hooks.cpp — ganchos do código traduzido (ppc_context.h) implementados pelo runtime real.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <string>
#include <vector>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "guest_memory.h"
#include "kernel/diag.h"
#include "kernel/time_bundle.h"
#include "ppc_context.h"
#include "apu/apu.h"

namespace {

bool report_once(uint64_t key) {
  static std::mutex mu;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(mu);
  return seen.insert(key).second;
}

}  // namespace

PPCFunc* ppc_lookup(uint32_t guest) {
  uint32_t lo = 0, hi = g_ppc_func_count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    uint32_t a = g_ppc_func_table[mid].guest;
    if (a == guest) return g_ppc_func_table[mid].host;
    if (a < guest) lo = mid + 1; else hi = mid;
  }
  return nullptr;
}

// GOW_TRACE_VCALL=lr1,lr2,...: regista as chamadas indiretas feitas a partir desses endereços de retorno (alvo,
// r3..r7, f1) e as GOW_TRACE_VCALL_DEPTH (padrão 40) chamadas indiretas seguintes da mesma thread.
namespace {
struct VcallTrace {
  std::vector<uint32_t> lrs;
  int depth = 40;
  bool on = false;
  VcallTrace() {
    const char* e = std::getenv("GOW_TRACE_VCALL");
    if (!e) return;
    for (const char* p = e; *p;) {
      char* end;
      uint32_t v = (uint32_t)std::strtoul(p, &end, 16);
      if (end == p) break;
      lrs.push_back(v);
      p = *end ? end + 1 : end;
    }
    if (const char* d = std::getenv("GOW_TRACE_VCALL_DEPTH")) depth = std::atoi(d);
    on = !lrs.empty();
  }
};
const VcallTrace& vcall_trace() {
  static VcallTrace t;
  return t;
}
thread_local int t_vcall_follow = 0;

// Nomes do UE3 (FNames): GNames = TArray<FNameEntry*> em 0x82C0C8CC, nome UTF-16BE em entrada+0x10.
// UObject: +0x28 Outer, +0x2C Name (índice), +0x34 Class.
bool guest_ptr_ok(uint32_t a) { return a >= 0x40000000u && a < 0x50000000u && !IsBadReadPtr(PPC_HOST(gm::base(), a), 4); }
std::string ue3_name(uint32_t index) {
  uint8_t* m = gm::base();
  uint32_t data = ppc_ld32(m, 0x82C0C8CCu), num = ppc_ld32(m, 0x82C0C8D0u);
  if (!guest_ptr_ok(data) || index >= num) return "?";
  uint32_t e = ppc_ld32(m, data + 4 * index);
  if (!guest_ptr_ok(e)) return "?";
  std::string s;
  for (uint32_t i = 0; i < 64; i++) {
    uint16_t c = ppc_ld16(m, e + 0x10 + 2 * i);
    if (!c) break;
    s += c < 128 ? (char)c : '?';
  }
  return s;
}
std::string ue3_object(uint32_t o) {
  if (!guest_ptr_ok(o) || !guest_ptr_ok(o + 0x34)) return "";
  uint8_t* m = gm::base();
  uint32_t outer = ppc_ld32(m, o + 0x28), cls = ppc_ld32(m, o + 0x34);
  std::string s = ue3_name(ppc_ld32(m, o + 0x2C));
  if (guest_ptr_ok(outer)) s = ue3_name(ppc_ld32(m, outer + 0x2C)) + "." + s;
  if (guest_ptr_ok(cls)) s += "(" + ue3_name(ppc_ld32(m, cls + 0x2C)) + ")";
  return s;
}
// GOW_TRACE_UFUNC=N: chamadas indiretas com r4 = UFunction (ProcessEvent e afins): as N primeiras de cada par
// (classe de r3, função), com o tempo desde o arranque — cronologia dos eventos UnrealScript.
int g_ufunc_limit = [] {
  const char* e = std::getenv("GOW_TRACE_UFUNC");
  return e ? std::max(1, std::atoi(e)) : 0;
}();
std::atomic<uint32_t> g_function_class{0};
std::mutex g_ufunc_mu;
std::unordered_map<uint64_t, int> g_ufunc_seen;
const auto g_start_time = std::chrono::steady_clock::now();

void trace_ufunc(const PPCContext& ctx) {
  uint32_t f = (uint32_t)ctx.r[4];
  if (f < 0x40000000u || f >= 0x50000000u || !guest_ptr_ok(f) || !guest_ptr_ok(f + 0x34)) return;
  uint8_t* m = gm::base();
  uint32_t cls = ppc_ld32(m, f + 0x34);
  uint32_t fc = g_function_class.load(std::memory_order_relaxed);
  if (fc ? cls != fc : true) {
    if (fc || !guest_ptr_ok(cls) || ue3_name(ppc_ld32(m, cls + 0x2C)) != "Function") return;
    g_function_class = cls;
  }
  uint32_t o = (uint32_t)ctx.r[3];
  uint32_t ocls = guest_ptr_ok(o + 0x34) ? ppc_ld32(m, o + 0x34) : 0;
  uint64_t key = (uint64_t)o << 32 | f;  // por objeto
  {
    std::lock_guard<std::mutex> lock(g_ufunc_mu);
    if (++g_ufunc_seen[key] > g_ufunc_limit) return;
  }
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start_time).count();
  std::fprintf(stderr, "[ufunc] %8.3f %s -> %s lr=0x%08X\n", t, ue3_object(o).c_str(), ue3_object(f).c_str(),
               (uint32_t)ctx.lr);
  // GOW_TRACE_UFUNC_BT=Nome: backtrace convidado quando a função (nome sem a classe) é chamada.
  static const char* bt = std::getenv("GOW_TRACE_UFUNC_BT");
  if (bt && ue3_name(ppc_ld32(m, f + 0x2C)) == bt) diag::guest_backtrace(bt, ctx.r[1], ctx.lr);
}
}  // namespace

void ppc_call_indirect(PPCContext& ctx, uint8_t* mem_base, uint32_t target) {
  if (g_ufunc_limit) trace_ufunc(ctx);
  // _cfltcvt(double*, buf, size, fmt, precision, caps) chamado pelo _woutput da CRT do jogo: guardado para o relatório de crash.
  if ((uint32_t)ctx.lr == 0x828DCEC4u)
    diag::note_float_format(mem_base, (uint32_t)ctx.r[3], (uint32_t)ctx.r[4], (uint32_t)ctx.r[5], (uint32_t)ctx.r[6],
                            (uint32_t)ctx.r[7], ctx.r[1], ctx.lr);
  if (const VcallTrace& vt = vcall_trace(); vt.on) {
    uint32_t lr = (uint32_t)ctx.lr;
    bool root = std::find(vt.lrs.begin(), vt.lrs.end(), lr) != vt.lrs.end();
    if (root) t_vcall_follow = vt.depth;
    if (root || t_vcall_follow > 0) {
      if (!root) t_vcall_follow--;
      std::fprintf(stderr, "[vcall] %s lr=0x%08X -> 0x%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X f1=%g\n",
                   root ? "*" : " ", lr, target, (uint32_t)ctx.r[3], (uint32_t)ctx.r[4], (uint32_t)ctx.r[5],
                   (uint32_t)ctx.r[6], (uint32_t)ctx.r[7], ctx.f[1]);
      std::string a = ue3_object((uint32_t)ctx.r[3]), b = ue3_object((uint32_t)ctx.r[4]);
      if (!a.empty() || !b.empty()) std::fprintf(stderr, "[vcall]     r3=%s r4=%s\n", a.c_str(), b.c_str());
      // GOW_TRACE_VCALL_PEEK=0xA0,0x144: nas chamadas raiz, u32 em r3+offset (campos do objeto).
      if (const char* pk = std::getenv("GOW_TRACE_VCALL_PEEK"); root && pk) {
        for (const char* p = pk; *p;) {
          char* end;
          uint32_t off = (uint32_t)std::strtoul(p, &end, 0);
          if (end == p) break;
          uint32_t ea = (uint32_t)ctx.r[3] + off;
          if (guest_ptr_ok(ea)) std::fprintf(stderr, "[vcall]     [r3+0x%X] = 0x%08X\n", off, ppc_ld32(mem_base, ea));
          p = *end ? end + 1 : end;
        }
      }
    }
  }
  if (PPCFunc* f = ppc_lookup(target)) {
    f(ctx, mem_base);
    return;
  }
  std::fprintf(stderr, "[ppc] salto indireto para endereço sem rotina: 0x%08X (lr=0x%08llX)\n", target,
               (unsigned long long)ctx.lr);
  diag::crash_report();
  std::abort();
}

void ppc_syscall(PPCContext&, uint8_t*, uint32_t addr) {
  if (report_once(0x300000000ull | addr)) std::fprintf(stderr, "[ppc] sc em 0x%08X (ignorado)\n", addr);
}

void ppc_trap(PPCContext& ctx, uint8_t* mem_base, uint32_t addr, uint32_t insn) {
  // twi 31,r0,20 = serviço de debug print do kernel (r3 = texto, r4 = comprimento).
  if (insn == 0x0FE00014u) {
    uint32_t len = std::min<uint32_t>((uint32_t)ctx.r[4], 2048);
    std::fprintf(stderr, "[debug] %.*s", (int)len, reinterpret_cast<const char*>(PPC_HOST(mem_base, (uint32_t)ctx.r[3])));
    if (len && ppc_ld8(mem_base, (uint32_t)ctx.r[3] + len - 1) != '\n') std::fputc('\n', stderr);
    return;
  }
  // Restantes twi 31,r0,N: __debugbreak/assert do SDK; segue em frente.
  if (report_once(0x100000000ull | addr)) std::fprintf(stderr, "[ppc] trap em 0x%08X (0x%08X)\n", addr, insn);
}

void ppc_unimplemented(PPCContext&, uint8_t*, uint32_t addr, uint32_t insn) {
  if (report_once(0x200000000ull | addr))
    std::fprintf(stderr, "[ppc] instrução não implementada em 0x%08X: 0x%08X\n", addr, insn);
}

void ppc_unreachable(PPCContext&, uint8_t*, uint32_t addr) {
  std::fprintf(stderr, "[ppc] fluxo caiu para fora da rotina em 0x%08X\n", addr);
  diag::crash_report();
  std::abort();
}

void ppc_mmio_st32r(uint8_t*, uint32_t ea, uint32_t v) { apu::xma_mmio_store(ea, v); }

uint64_t ppc_mftb() {
  // Mesma frequência que KeQueryPerformanceFrequency devolve ao convidado.
  using namespace std::chrono;
  auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  return (uint64_t)((unsigned __int128)ns * kernel::kTimebaseFrequency / 1000000000);
}
