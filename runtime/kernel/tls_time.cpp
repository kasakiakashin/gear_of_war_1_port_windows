// tls_time.cpp — TLS dinâmico (KeTls*) e relógios (KeQuery*, KeTimeStampBundle).
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "guest_memory.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"
#include "kernel/time_bundle.h"

using namespace kernel;

namespace kernel {

uint64_t system_time() {
  // FILETIME: 100 ns desde 1601-01-01; system_clock usa a época Unix (1970).
  using namespace std::chrono;
  auto unix_100ns = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
  return (uint64_t)unix_100ns + 116444736000000000ull;
}

uint64_t interrupt_time() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return (uint64_t)duration_cast<nanoseconds>(steady_clock::now() - start).count() / 100;
}

// KeTimeStampBundle (exportado): +0x00 u64 InterruptTime, +0x08 u64 SystemTime, +0x10 u32 TickCount (ms).
// GetTickCount do XAPI lê TickCount diretamente, por isso é atualizado a cada 1 ms por uma thread host.
void start_time_stamp_bundle(uint32_t bundle) {
  std::thread([bundle] {
    uint8_t* m = gm::base();
    for (;;) {
      uint64_t it = interrupt_time();
      ppc_st64(m, bundle + 0x00, it);
      ppc_st64(m, bundle + 0x08, system_time());
      ppc_st32(m, bundle + 0x10, (uint32_t)(it / 10000));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }).detach();
}

}  // namespace kernel

// ---- TLS: os slots ficam na memória convidada (KPCR+0 -> u32[64]), como o jogo pode ler inline ----
namespace {
std::mutex g_tls_mu;
uint64_t g_tls_used = 0;  // bitmap de 64 slots
}  // namespace

XBOX_IMPORT(KeTlsAlloc) {
  std::lock_guard<std::mutex> lock(g_tls_mu);
  for (uint32_t i = 0; i < kTlsSlotCount; i++)
    if (!(g_tls_used >> i & 1)) {
      g_tls_used |= 1ull << i;
      return ret(ctx, i);
    }
  ret(ctx, 0xFFFFFFFFu);
}
XBOX_IMPORT(KeTlsFree) {
  uint32_t i = arg(ctx, 0);
  if (i >= kTlsSlotCount) return ret(ctx, 0);
  std::lock_guard<std::mutex> lock(g_tls_mu);
  g_tls_used &= ~(1ull << i);
  ret(ctx, 1);
}
XBOX_IMPORT(KeTlsGetValue) {
  uint32_t i = arg(ctx, 0);
  GuestThread* t = current_thread();
  ret(ctx, t && i < kTlsSlotCount ? ppc_ld32(mem_base, t->tls + 4 * i) : 0);
}
XBOX_IMPORT(KeTlsSetValue) {
  uint32_t i = arg(ctx, 0);
  GuestThread* t = current_thread();
  if (!t || i >= kTlsSlotCount) return ret(ctx, 0);
  ppc_st32(mem_base, t->tls + 4 * i, arg(ctx, 1));
  ret(ctx, 1);
}

// ---- Tempo ----
// Mesma frequência do timebase (mftb) usado por QueryPerformanceCounter no convidado.
XBOX_IMPORT(KeQueryPerformanceFrequency) { ret(ctx, kTimebaseFrequency); }
XBOX_IMPORT(KeQuerySystemTime) { ppc_st64(mem_base, arg(ctx, 0), system_time()); }  // (PLARGE_INTEGER)
