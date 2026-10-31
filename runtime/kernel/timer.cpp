// timer.cpp — timers do kernel (NtCreateTimer / NtSetTimerEx / NtCancelTimer).
//
// O KTIMER (0x28 bytes) vive na memória convidada com DISPATCHER_HEADER de tipo 8 (notificação) ou 9
// (sincronização: a espera satisfeita volta a zerá-lo, ver sync.cpp). Uma thread do host guarda os prazos e
// sinaliza o objeto quando expira; timers periódicos são reagendados. A rotina opcional do NtSetTimerEx vira uma
// APC de utilizador da thread que armou o timer (entregue na sua próxima espera alertável).
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>

#include "kernel/apc.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "kernel/thread.h"

using namespace kernel;

namespace {

using Clock = std::chrono::steady_clock;

struct Timer {
  Clock::time_point due;
  std::chrono::milliseconds period{0};
  GuestThread* thread = nullptr;  // quem chamou NtSetTimerEx: recebe a APC
  uint32_t routine = 0, apc_context = 0;
};

std::mutex g_mu;
std::condition_variable g_cv;
std::map<uint32_t, Timer> g_active;  // objeto -> prazo

void service() {
  std::unique_lock<std::mutex> lock(g_mu);
  for (;;) {
    auto next = Clock::time_point::max();
    for (auto& [obj, t] : g_active) next = std::min(next, t.due);
    if (next == Clock::time_point::max()) {
      g_cv.wait(lock);
      continue;
    }
    if (g_cv.wait_until(lock, next) != std::cv_status::timeout && Clock::now() < next) continue;
    auto now = Clock::now();
    for (auto it = g_active.begin(); it != g_active.end();) {
      if (it->second.due > now) {
        ++it;
        continue;
      }
      set_event(it->first);  // SignalState = 1 + acorda quem espera (vale para qualquer objeto de dispatcher)
      if (it->second.routine) {  // PTIMERAPCROUTINE(Context, TimerLowValue, TimerHighValue): FILETIME do disparo
        uint64_t ft = system_time();
        queue_user_apc(it->second.thread, it->second.routine, it->second.apc_context, (uint32_t)ft,
                       (uint32_t)(ft >> 32));
      }
      if (it->second.period.count()) {
        it->second.due += it->second.period;
        if (it->second.due < now) it->second.due = now + it->second.period;
        ++it;
      } else {
        it = g_active.erase(it);
      }
    }
  }
}

void ensure_service() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(service).detach(); });
}

}  // namespace

// NtCreateTimer(PHANDLE, POBJECT_ATTRIBUTES, TIMER_TYPE): 0 = NotificationTimer, 1 = SynchronizationTimer.
XBOX_IMPORT(NtCreateTimer) {
  uint32_t handle_ptr = arg(ctx, 0), attrs = arg(ctx, 1), type = arg(ctx, 2);
  if (!handle_ptr || type > 1) return ret_status(ctx, kStatusInvalidParameter);
  uint32_t obj = alloc_object(0x28);
  if (!obj) return ret_status(ctx, kStatusNoMemory);
  init_header(obj, type ? kSynchronizationTimer : kNotificationTimer, 0, 0x28 / 4);
  for (uint32_t o = 0x10; o < 0x28; o += 4) ppc_st32(mem_base, obj + o, 0);
  bool existed = false;
  uint32_t actual = named_object(attrs, obj, &existed);
  ppc_st32(mem_base, handle_ptr, create_handle(actual));
  ret_status(ctx, existed ? kStatusObjectNameExists : kStatusSuccess);
}

// NtSetTimerEx(HANDLE, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE, KPROCESSOR_MODE, PVOID ApcContext,
//              BOOLEAN Resume, LONG PeriodMs, PBOOLEAN PreviousState)
// DueTime: negativo = relativo (100 ns), positivo = absoluto (FILETIME).
XBOX_IMPORT(NtSetTimerEx) {
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  uint32_t due_ptr = arg(ctx, 1), routine = arg(ctx, 2), period = arg(ctx, 6), prev_ptr = arg(ctx, 7);
  if (!due_ptr) return ret_status(ctx, kStatusInvalidParameter);
  uint64_t delay = decode_timeout(mem_base, due_ptr);
  ensure_service();
  int32_t prev = reset_event(obj);
  if (prev_ptr) ppc_st8(mem_base, prev_ptr, prev > 0 ? 1 : 0);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_active[obj] = Timer{Clock::now() + std::chrono::nanoseconds(delay * 100), std::chrono::milliseconds(period),
                          current_thread(), routine, arg(ctx, 4)};
  }
  g_cv.notify_all();
  ret_status(ctx, kStatusSuccess);
}

// NtCancelTimer(HANDLE, PBOOLEAN CurrentState)
XBOX_IMPORT(NtCancelTimer) {
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_active.erase(obj);
  }
  if (arg(ctx, 1)) ppc_st8(mem_base, arg(ctx, 1), (int32_t)ppc_ld32(mem_base, obj + 4) > 0 ? 1 : 0);
  ret_status(ctx, kStatusSuccess);
}
