// rtl.cpp — critical sections do RTL e listas SLIST interligadas.
#include <atomic>
#include <thread>

#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "kernel/thread.h"

using namespace kernel;

// RTL_CRITICAL_SECTION (Xbox 360, 0x1C bytes BE):
//   +0x00 DISPATCHER_HEADER (evento de sincronização)  +0x10 LONG LockCount (-1 = livre)
//   +0x14 LONG RecursionCount  +0x18 PKTHREAD OwningThread
namespace {

constexpr uint32_t kLockCount = 0x10, kRecursion = 0x14, kOwner = 0x18;

uint32_t me() {
  GuestThread* t = current_thread();
  return t ? t->kthread : 1;
}

}  // namespace

namespace kernel {
void init_critical_section(uint8_t* m, uint32_t cs) {
  init_header(cs, kSynchronizationEvent, 0, 4);
  ppc_st32(m, cs + kLockCount, 0xFFFFFFFFu);
  ppc_st32(m, cs + kRecursion, 0);
  ppc_st32(m, cs + kOwner, 0);
}
}  // namespace kernel

XBOX_IMPORT(RtlInitializeCriticalSection) { init_critical_section(mem_base, arg(ctx, 0)); }
// Não importados por este jogo; mantidos por completude.
XBOX_IMPORT(RtlInitializeCriticalSectionAndSpinCount) {
  init_critical_section(mem_base, arg(ctx, 0));
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(RtlDeleteCriticalSection) {}

XBOX_IMPORT(RtlEnterCriticalSection) {
  uint32_t cs = arg(ctx, 0), cur = me();
  if (atomic_add32(mem_base, cs + kLockCount, 1) == 0) {  // estava livre (-1)
    ppc_st32(mem_base, cs + kOwner, cur);
    ppc_st32(mem_base, cs + kRecursion, 1);
    return;
  }
  if (ppc_ld32(mem_base, cs + kOwner) == cur) {
    ppc_st32(mem_base, cs + kRecursion, ppc_ld32(mem_base, cs + kRecursion) + 1);
    return;
  }
  wait(&cs, 1, false, kInfinite);  // o dono sinaliza o evento embutido ao sair
  ppc_st32(mem_base, cs + kOwner, cur);
  ppc_st32(mem_base, cs + kRecursion, 1);
}

XBOX_IMPORT(RtlLeaveCriticalSection) {
  uint32_t cs = arg(ctx, 0);
  uint32_t rec = ppc_ld32(mem_base, cs + kRecursion) - 1;
  ppc_st32(mem_base, cs + kRecursion, rec);
  if (rec) {
    atomic_add32(mem_base, cs + kLockCount, -1);
    return;
  }
  ppc_st32(mem_base, cs + kOwner, 0);
  if (atomic_add32(mem_base, cs + kLockCount, -1) >= 0) set_event(cs);  // há threads à espera
}

XBOX_IMPORT(RtlTryEnterCriticalSection) {
  uint32_t cs = arg(ctx, 0), cur = me();
  if (atomic_cas32(mem_base, cs + kLockCount, 0xFFFFFFFFu, 0)) {
    ppc_st32(mem_base, cs + kOwner, cur);
    ppc_st32(mem_base, cs + kRecursion, 1);
    return ret(ctx, 1);
  }
  if (ppc_ld32(mem_base, cs + kOwner) == cur) {
    atomic_add32(mem_base, cs + kLockCount, 1);
    ppc_st32(mem_base, cs + kRecursion, ppc_ld32(mem_base, cs + kRecursion) + 1);
    return ret(ctx, 1);
  }
  ret(ctx, 0);
}

// SLIST_HEADER (8 bytes BE): +0 Next (SINGLE_LIST_ENTRY*), +4 u16 Depth, +6 u16 Sequence. O jogo faz o
// push inline com ldarx/stdcx., que no código traduzido é um CAS sobre os 64 bits brutos: usar o mesmo aqui.
namespace {
std::atomic_ref<uint64_t> slist(uint8_t* m, uint32_t head) {
  return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(PPC_HOST(m, head)));
}
}  // namespace

XBOX_IMPORT(InterlockedPopEntrySList) {
  auto ref = slist(mem_base, arg(ctx, 0));
  uint64_t raw = ref.load();
  for (;;) {
    uint64_t v = ppc_bswap(raw);
    uint32_t first = (uint32_t)(v >> 32);
    if (!first) return ret(ctx, 0);
    uint32_t next = ppc_ld32(mem_base, first);
    uint16_t depth = (uint16_t)(v >> 16), seq = (uint16_t)v;
    uint64_t nv = (uint64_t)next << 32 | (uint64_t)(uint16_t)(depth - 1) << 16 | (uint16_t)(seq + 1);
    if (ref.compare_exchange_weak(raw, ppc_bswap(nv))) return ret(ctx, first);
  }
}

XBOX_IMPORT(InterlockedFlushSList) {
  auto ref = slist(mem_base, arg(ctx, 0));
  uint64_t raw = ref.load();
  for (;;) {
    uint64_t v = ppc_bswap(raw);
    uint64_t nv = (uint16_t)(v + 1);  // Next = 0, Depth = 0, Sequence + 1
    if (ref.compare_exchange_weak(raw, ppc_bswap(nv))) return ret(ctx, (uint32_t)(v >> 32));
  }
}
