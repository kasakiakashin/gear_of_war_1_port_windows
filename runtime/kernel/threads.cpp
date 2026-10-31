// threads.cpp — imports de criação/controlo de threads (xboxkrnl).
#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "guest_memory.h"
#include "kernel/thread.h"

using namespace kernel;

namespace {

constexpr uint32_t kCreateSuspended = 0x1;
constexpr SIZE_T kHostStackSize = 64ull << 20;  // recursão das rotinas traduzidas

struct StartInfo {
  GuestThread* thread;
  uint32_t xapi_startup, start_address, start_context;
};

DWORD WINAPI host_thread_main(LPVOID p) {
  auto* s = static_cast<StartInfo*>(p);
  GuestThread* t = s->thread;
  auto* ctx = new PPCContext;
  bind_guest_thread(t, *ctx);
  wait_while_suspended(t);
  // Com rotina XAPI: XapiThreadStartup(StartAddress, StartContext); senão StartAddress(StartContext).
  uint32_t entry = s->xapi_startup ? s->xapi_startup : s->start_address;
  if (s->xapi_startup) {
    ctx->r[3] = s->start_address;
    ctx->r[4] = s->start_context;
  } else {
    ctx->r[3] = s->start_context;
  }
  delete s;
  uint32_t code = run_guest(t, *ctx, entry);
  std::fprintf(stderr, "[thread] %u terminou (código 0x%08X)\n", t->thread_id, code);
  finish_thread(t, code);
  return code;
}

// KPCR+0x10C = CPU atual: a 1ª CPU da máscara de afinidade (KeGetCurrentProcessorNumber inline no jogo).
void apply_affinity(GuestThread* t, uint32_t mask) {
  t->affinity = mask;
  if (mask & 0x3F) ppc_st8(gm::base(), t->pcr + 0x10C, (uint8_t)__builtin_ctz(mask & 0x3F));
}

uint32_t resume(GuestThread* t) {
  std::lock_guard<std::mutex> lock(t->mu);
  int32_t prev = t->suspend_count;
  if (prev > 0 && --t->suspend_count == 0) t->cv.notify_all();
  return (uint32_t)prev;
}

GuestThread* thread_of_handle(uint32_t handle) { return thread_by_kthread(object_of(handle)); }

}  // namespace

// ExCreateThread(PHANDLE Handle, SIZE_T StackSize, PDWORD ThreadId, PVOID XapiThreadStartup,
//                PVOID StartAddress, PVOID StartContext, DWORD CreationFlags)
XBOX_IMPORT(ExCreateThread) {
  uint32_t handle_ptr = arg(ctx, 0), id_ptr = arg(ctx, 2), flags = arg(ctx, 6);
  GuestThread* t = alloc_guest_thread(arg(ctx, 1));
  if (!t) return ret_status(ctx, kStatusNoMemory);
  apply_affinity(t, flags >> 24);
  t->suspend_count = (flags & kCreateSuspended) ? 1 : 0;
  auto* s = new StartInfo{t, arg(ctx, 3), arg(ctx, 4), arg(ctx, 5)};
  HANDLE h = CreateThread(nullptr, kHostStackSize, host_thread_main, s, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!h) {
    delete s;
    return ret_status(ctx, kStatusNoMemory);
  }
  CloseHandle(h);
  out32(mem_base, handle_ptr, create_handle(t->kthread));
  out32(mem_base, id_ptr, t->thread_id);
  std::fprintf(stderr, "[thread] %u criada: start=0x%08X ctx=0x%08X xapi=0x%08X pilha=0x%X flags=0x%X\n",
               t->thread_id, arg(ctx, 4), arg(ctx, 5), arg(ctx, 3), t->stack_base - t->stack_limit, flags);
  ret_status(ctx, kStatusSuccess);
}

XBOX_IMPORT(ExTerminateThread) { throw ThreadExit{arg(ctx, 0)}; }  // (DWORD ExitStatus)

// NtResumeThread(HANDLE, PULONG PreviousSuspendCount) / KeResumeThread(PKTHREAD) -> contagem anterior
XBOX_IMPORT(NtResumeThread) {
  GuestThread* t = thread_of_handle(arg(ctx, 0));
  if (!t) return ret_status(ctx, kStatusInvalidHandle);
  out32(mem_base, arg(ctx, 1), resume(t));
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(KeResumeThread) {
  GuestThread* t = thread_by_kthread(arg(ctx, 0));
  ret(ctx, t ? resume(t) : 0);
}

// NtSuspendThread(HANDLE, PULONG PreviousSuspendCount): só tem efeito imediato na própria thread; noutra
// thread conta para o próximo ponto de suspensão (arranque).
XBOX_IMPORT(NtSuspendThread) {
  GuestThread* t = thread_of_handle(arg(ctx, 0));
  if (!t) return ret_status(ctx, kStatusInvalidHandle);
  int32_t prev;
  {
    std::lock_guard<std::mutex> lock(t->mu);
    prev = t->suspend_count++;
  }
  out32(mem_base, arg(ctx, 1), (uint32_t)prev);
  if (t == current_thread()) wait_while_suspended(t);
  else std::fprintf(stderr, "[thread] NtSuspendThread de outra thread (%u) ignorado em execução\n", t->thread_id);
  ret_status(ctx, kStatusSuccess);
}

// KeSetAffinityThread(PKTHREAD, KAFFINITY) / KeSetBasePriorityThread(PKTHREAD, LONG) -> valor anterior.
// Guardados apenas (o escalonador do host decide).
XBOX_IMPORT(KeSetAffinityThread) {
  GuestThread* t = thread_by_kthread(arg(ctx, 0));
  if (!t) return ret(ctx, 0);
  uint32_t prev = t->affinity;
  apply_affinity(t, arg(ctx, 1));
  ret(ctx, prev);
}
XBOX_IMPORT(KeSetBasePriorityThread) {
  GuestThread* t = thread_by_kthread(arg(ctx, 0));
  if (!t) return ret(ctx, 0);
  int32_t prev = t->priority;
  t->priority = (int32_t)arg(ctx, 1);
  ret_status(ctx, (uint32_t)prev);
}

XBOX_IMPORT(KeGetCurrentProcessType) { ret(ctx, 1); }  // PROCTYPE_USER (título)
