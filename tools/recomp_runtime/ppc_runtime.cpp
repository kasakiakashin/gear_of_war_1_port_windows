// ppc_runtime.cpp — implementação padrão (mínima) dos ganchos usados pelo código traduzido.
// Serve para ligar e testar a fase 2; a fase 3 substitui este ficheiro pelo runtime real
// (kernel/XAM HLE, threads, memória virtual).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_set>

#include "ppc_context.h"

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

static bool report_once(uint64_t key) {
  static std::mutex mu;
  static std::unordered_set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(mu);
  return seen.insert(key).second;
}

void ppc_call_indirect(PPCContext& ctx, uint8_t* mem_base, uint32_t target) {
  if (PPCFunc* f = ppc_lookup(target)) {
    f(ctx, mem_base);
    return;
  }
  std::fprintf(stderr, "[ppc] salto indireto para endereço sem rotina: 0x%08X (lr=0x%08llX)\n", target,
               (unsigned long long)ctx.lr);
  std::abort();
}

void ppc_import_call(PPCContext& ctx, uint8_t*, uint32_t lib, uint32_t ordinal) {
  if (report_once(((uint64_t)lib << 32) | ordinal))
    std::fprintf(stderr, "[ppc] import não implementado: %s ordinal 0x%04X\n", lib == 0 ? "xam.xex" : "xboxkrnl.exe",
                 ordinal);
  ctx.r[3] = 0;
}

void ppc_syscall(PPCContext&, uint8_t*, uint32_t addr) {
  std::fprintf(stderr, "[ppc] sc em 0x%08X\n", addr);
}

void ppc_trap(PPCContext&, uint8_t*, uint32_t addr, uint32_t insn) {
  if (report_once(0x100000000ull | addr)) std::fprintf(stderr, "[ppc] trap em 0x%08X (0x%08X)\n", addr, insn);
}

void ppc_unimplemented(PPCContext&, uint8_t*, uint32_t addr, uint32_t insn) {
  if (report_once(0x200000000ull | addr))
    std::fprintf(stderr, "[ppc] instrução não implementada em 0x%08X: 0x%08X\n", addr, insn);
}

void ppc_unreachable(PPCContext&, uint8_t*, uint32_t addr) {
  std::fprintf(stderr, "[ppc] fluxo caiu para fora da rotina em 0x%08X\n", addr);
  std::abort();
}

uint64_t ppc_mftb() {
  // Timebase do Xenon: 49.875 MHz
  using namespace std::chrono;
  auto ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
  return (uint64_t)((unsigned __int128)ns * 49875000 / 1000000000);
}
