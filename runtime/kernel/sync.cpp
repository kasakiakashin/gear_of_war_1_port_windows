// sync.cpp — núcleo de dispatcher + imports de eventos, semáforos, mutantes, esperas, spinlocks e IRQL.
#include "kernel/sync.h"
#include "kernel/vfs.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"
#include "kernel/apc.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"

namespace kernel {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mu;
std::condition_variable g_cv;
std::unordered_map<uint32_t, uint32_t> g_mutant_owner;  // objeto -> KTHREAD dono
std::unordered_map<uint32_t, uint32_t> g_handles;       // handle -> objeto
std::unordered_map<std::string, uint32_t> g_names;      // nome -> objeto
std::unordered_map<uint32_t, std::vector<uint32_t>> g_free_objects;  // tamanho -> objetos livres
uint32_t g_next_handle = 0xF8000004u;

uint8_t* mem() { return gm::base(); }
uint8_t type_of(uint32_t obj) { return ppc_ld8(mem(), obj); }
int32_t signal_of(uint32_t obj) { return (int32_t)ppc_ld32(mem(), obj + 4); }
void set_signal(uint32_t obj, int32_t v) { ppc_st32(mem(), obj + 4, (uint32_t)v); }
uint32_t current_kthread() {
  GuestThread* t = current_thread();
  return t ? t->kthread : 0;
}

// Chamadas com g_mu adquirido.
bool is_signaled(uint32_t obj) {
  if (type_of(obj) == kMutant) {
    if (signal_of(obj) > 0) return true;
    auto it = g_mutant_owner.find(obj);
    return it != g_mutant_owner.end() && it->second == current_kthread();
  }
  return signal_of(obj) > 0;
}

void satisfy(uint32_t obj) {
  switch (type_of(obj)) {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
      set_signal(obj, 0);
      break;
    case kSemaphore:
      set_signal(obj, signal_of(obj) - 1);
      break;
    case kMutant:
      set_signal(obj, signal_of(obj) - 1);
      g_mutant_owner[obj] = current_kthread();
      break;
    default:  // eventos/timers de notificação, threads, processos: continuam sinalizados
      break;
  }
}

}  // namespace

uint64_t decode_timeout(uint8_t* m, uint32_t timeout_ptr) {
  if (!timeout_ptr) return kInfinite;
  int64_t v = (int64_t)ppc_ld64(m, timeout_ptr);
  if (v <= 0) return (uint64_t)-v;  // relativo (negativo) ou 0 = consulta
  uint64_t now = system_time();     // absoluto (FILETIME)
  return (uint64_t)v > now ? (uint64_t)v - now : 0;
}

uint32_t wait(const uint32_t* objects, uint32_t count, bool wait_all, uint64_t timeout_100ns, bool alertable) {
  GuestThread* self = alertable ? current_thread() : nullptr;
  std::unique_lock<std::mutex> lock(g_mu);
  auto deadline = Clock::now();
  if (timeout_100ns != kInfinite) deadline += std::chrono::nanoseconds(timeout_100ns * 100);
  for (;;) {
    if (self && user_apc_pending(self)) return kStatusUserApc;
    if (!count) {
      // só tempo: continua até ao prazo
    } else if (wait_all) {
      bool all = true;
      for (uint32_t i = 0; i < count && all; i++) all = is_signaled(objects[i]);
      if (all) {
        for (uint32_t i = 0; i < count; i++) satisfy(objects[i]);
        return kStatusSuccess;
      }
    } else {
      for (uint32_t i = 0; i < count; i++)
        if (is_signaled(objects[i])) {
          satisfy(objects[i]);
          return kStatusSuccess + i;
        }
    }
    if (timeout_100ns == 0) return kStatusTimeout;
    if (timeout_100ns == kInfinite) {
      g_cv.wait(lock);
    } else if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout) {
      timeout_100ns = 0;  // última verificação antes de devolver STATUS_TIMEOUT
    }
  }
}

void notify_waiters() {
  std::lock_guard<std::mutex> lock(g_mu);
  g_cv.notify_all();
}

int32_t set_event(uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mu);
  int32_t prev = signal_of(obj);
  set_signal(obj, 1);
  g_cv.notify_all();
  return prev;
}

int32_t reset_event(uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mu);
  int32_t prev = signal_of(obj);
  set_signal(obj, 0);
  return prev;
}

// Aproximação: sinaliza, acorda quem espera e dá-lhes uma fatia de tempo antes de voltar a zerar.
int32_t pulse_event(uint32_t obj) {
  int32_t prev = set_event(obj);
  std::this_thread::yield();
  reset_event(obj);
  return prev;
}

int32_t release_semaphore(uint32_t obj, int32_t adjustment, uint32_t* status) {
  std::lock_guard<std::mutex> lock(g_mu);
  int32_t prev = signal_of(obj), limit = (int32_t)ppc_ld32(mem(), obj + 0x10);
  if (prev + adjustment > limit) {
    if (status) *status = kStatusSemaphoreLimitExceeded;
    return prev;
  }
  set_signal(obj, prev + adjustment);
  g_cv.notify_all();
  if (status) *status = kStatusSuccess;
  return prev;
}

uint32_t release_mutant(uint32_t obj, int32_t* previous) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_mutant_owner.find(obj);
  if (it == g_mutant_owner.end() || it->second != current_kthread()) return kStatusMutantNotOwned;
  int32_t prev = signal_of(obj);
  if (previous) *previous = prev;
  set_signal(obj, prev + 1);
  if (prev + 1 > 0) g_mutant_owner.erase(it);
  g_cv.notify_all();
  return kStatusSuccess;
}

void init_header(uint32_t obj, uint8_t type, int32_t signal, uint8_t size_words) {
  uint8_t* m = mem();
  ppc_st8(m, obj + 0, type);
  ppc_st8(m, obj + 1, 0);
  ppc_st8(m, obj + 2, size_words);
  ppc_st8(m, obj + 3, 0);
  ppc_st32(m, obj + 4, (uint32_t)signal);
  ppc_st32(m, obj + 8, obj + 8);
  ppc_st32(m, obj + 12, obj + 8);
}

uint32_t create_handle(uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mu);
  uint32_t h = g_next_handle;
  g_next_handle += 4;
  g_handles[h] = obj;
  return h;
}

uint32_t object_of(uint32_t handle) {
  if (handle == kCurrentThreadHandle) return current_kthread();
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_handles.find(handle);
  return it == g_handles.end() ? 0 : it->second;
}

bool close_handle(uint32_t handle) {
  if (handle == kCurrentThreadHandle || handle == kCurrentProcessHandle) return true;
  std::lock_guard<std::mutex> lock(g_mu);
  return g_handles.erase(handle) != 0;  // objetos ficam vivos (podem ter nome / outros handles)
}

bool has_handles(uint32_t obj) {
  std::lock_guard<std::mutex> lock(g_mu);
  for (auto& [h, o] : g_handles)
    if (o == obj) return true;
  return false;
}

uint32_t alloc_object(uint32_t size) {
  size = (size + 15) & ~15u;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto& fl = g_free_objects[size];
    if (!fl.empty()) {
      uint32_t o = fl.back();
      fl.pop_back();
      return o;
    }
  }
  return gm::alloc_runtime(size, 16);
}

uint32_t create_event_handle(bool manual_reset, bool signaled, uint32_t* object) {
  uint32_t obj = alloc_object(0x10);
  init_header(obj, manual_reset ? kNotificationEvent : kSynchronizationEvent, signaled ? 1 : 0, 0x10 / 4);
  if (object) *object = obj;
  return create_handle(obj);
}

uint32_t named_object(uint32_t object_attributes, uint32_t create_obj, bool* existed) {
  if (existed) *existed = false;
  if (!object_attributes) return create_obj;
  uint8_t* m = mem();
  uint32_t name_ptr = ppc_ld32(m, object_attributes + 4);  // OBJECT_ATTRIBUTES.ObjectName (ANSI_STRING*)
  if (!name_ptr) return create_obj;
  uint16_t len = ppc_ld16(m, name_ptr);
  uint32_t buf = ppc_ld32(m, name_ptr + 4);
  if (!len || !buf) return create_obj;
  std::string name(reinterpret_cast<const char*>(PPC_HOST(m, buf)), len);
  std::lock_guard<std::mutex> lock(g_mu);
  auto [it, inserted] = g_names.emplace(name, create_obj);
  if (!inserted && existed) *existed = true;
  return it->second;
}

}  // namespace kernel

// =============================================================================================
// Imports
// =============================================================================================
using namespace kernel;

namespace {

// Cria um objeto de dispatcher (opcionalmente nomeado) e devolve o handle em *handle_ptr.
uint32_t create_object(uint8_t* m, uint32_t handle_ptr, uint32_t attrs, uint8_t type,
                       int32_t signal, uint32_t size) {
  uint32_t obj = alloc_object(size);
  if (!obj) return kStatusNoMemory;
  init_header(obj, type, signal, (uint8_t)(size / 4));
  bool existed = false;
  uint32_t actual = named_object(attrs, obj, &existed);
  ppc_st32(m, handle_ptr, create_handle(actual));
  return existed ? kStatusObjectNameExists : kStatusSuccess;
}

uint32_t wait_handles(uint8_t* m, uint32_t count, uint32_t handles_ptr, bool wait_all, uint32_t timeout_ptr,
                      bool alertable) {
  uint32_t objs[64];
  if (!count || count > 64) return kStatusInvalidParameter;
  for (uint32_t i = 0; i < count; i++)
    if (!(objs[i] = object_of(ppc_ld32(m, handles_ptr + 4 * i)))) return kStatusInvalidHandle;
  return wait(objs, count, wait_all, decode_timeout(m, timeout_ptr), alertable);
}

}  // namespace

// ---- Eventos ----
XBOX_IMPORT(NtCreateEvent) {  // (PHANDLE, POBJECT_ATTRIBUTES, EVENT_TYPE, BOOLEAN InitialState)
  ret_status(ctx, create_object(mem_base, arg(ctx, 0), arg(ctx, 1), (uint8_t)arg(ctx, 2), arg(ctx, 3) ? 1 : 0, 0x10));
}
XBOX_IMPORT(NtSetEvent) {  // (HANDLE, PLONG PreviousState)
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  out32(mem_base, arg(ctx, 1), (uint32_t)set_event(obj));
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(NtClearEvent) {
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  reset_event(obj);
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(NtPulseEvent) {
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  out32(mem_base, arg(ctx, 1), (uint32_t)pulse_event(obj));
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(KeSetEvent) { ret_status(ctx, (uint32_t)set_event(arg(ctx, 0))); }    // (PKEVENT, Increment, Wait)
XBOX_IMPORT(KeResetEvent) { ret_status(ctx, (uint32_t)reset_event(arg(ctx, 0))); }

// ---- Semáforos ----
XBOX_IMPORT(KeInitializeSemaphore) {  // (PKSEMAPHORE, LONG Count, LONG Limit)
  init_header(arg(ctx, 0), kSemaphore, (int32_t)arg(ctx, 1), 0x14 / 4);
  ppc_st32(mem_base, arg(ctx, 0) + 0x10, arg(ctx, 2));
}
XBOX_IMPORT(KeReleaseSemaphore) {  // (PKSEMAPHORE, Increment, LONG Adjustment, Wait) -> estado anterior
  uint32_t status;
  ret_status(ctx, (uint32_t)release_semaphore(arg(ctx, 0), (int32_t)arg(ctx, 2), &status));
  if (status) std::fprintf(stderr, "[sync] KeReleaseSemaphore: limite excedido em 0x%08X\n", arg(ctx, 0));
}

// ---- Mutantes ----
XBOX_IMPORT(NtCreateMutant) {  // (PHANDLE, POBJECT_ATTRIBUTES, BOOLEAN InitialOwner)
  uint32_t st = create_object(mem_base, arg(ctx, 0), arg(ctx, 1), kMutant, 1, 0x20);
  if (st == kStatusSuccess && arg(ctx, 2)) {
    uint32_t obj = object_of(ppc_ld32(mem_base, arg(ctx, 0)));
    wait(&obj, 1, false, 0);  // adquire de imediato
  }
  ret_status(ctx, st);
}
XBOX_IMPORT(NtReleaseMutant) {  // (HANDLE, PLONG PreviousCount)
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  int32_t prev = 0;
  uint32_t st = release_mutant(obj, &prev);
  if (st == kStatusSuccess) out32(mem_base, arg(ctx, 1), (uint32_t)prev);
  ret_status(ctx, st);
}

// ---- Handles / objetos ----
XBOX_IMPORT(NtClose) {
  uint32_t obj = object_of(arg(ctx, 0));
  if (!close_handle(arg(ctx, 0))) return ret_status(ctx, kStatusInvalidHandle);
  if (obj) vfs::on_handle_closed(obj);
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(NtDuplicateObject) {  // (HANDLE Source, PHANDLE Target, ULONG Options)
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  out32(mem_base, arg(ctx, 1), create_handle(obj));
  if (arg(ctx, 2) & 1) close_handle(arg(ctx, 0));  // DUPLICATE_CLOSE_SOURCE
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(ObReferenceObjectByHandle) {  // (HANDLE, POBJECT_TYPE, PVOID* Object)
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  out32(mem_base, arg(ctx, 2), obj);
  ret_status(ctx, kStatusSuccess);
}
XBOX_IMPORT(ObDereferenceObject) {}

// ---- Esperas ----
// Esperas alertáveis entregam as APCs de utilizador pendentes e devolvem STATUS_USER_APC.
namespace {
uint32_t after_wait(PPCContext& ctx, uint32_t status) {
  if (status == kStatusUserApc) deliver_user_apcs(ctx);
  return status;
}
}  // namespace

XBOX_IMPORT(KeWaitForSingleObject) {  // (PVOID Object, Reason, Mode, Alertable, PLARGE_INTEGER Timeout)
  uint32_t obj = arg(ctx, 0);
  bool alertable = arg(ctx, 3) & 0xFF;
  ret_status(ctx, after_wait(ctx, wait(&obj, 1, false, decode_timeout(mem_base, arg(ctx, 4)), alertable)));
}
XBOX_IMPORT(KeWaitForMultipleObjects) {  // (Count, PVOID Object[], WaitType, Reason, Mode, Alertable, Timeout, WaitBlocks)
  uint32_t count = arg(ctx, 0), objs[64];
  if (!count || count > 64) return ret_status(ctx, kStatusInvalidParameter);
  for (uint32_t i = 0; i < count; i++) objs[i] = ppc_ld32(mem_base, arg(ctx, 1) + 4 * i);
  bool alertable = arg(ctx, 5) & 0xFF;
  ret_status(ctx, after_wait(ctx, wait(objs, count, arg(ctx, 2) == 0, decode_timeout(mem_base, arg(ctx, 6)),
                                       alertable)));  // WaitAll = 0
}
XBOX_IMPORT(NtWaitForSingleObjectEx) {  // (HANDLE, Mode, Alertable, PLARGE_INTEGER Timeout)
  uint32_t obj = object_of(arg(ctx, 0));
  if (!obj) return ret_status(ctx, kStatusInvalidHandle);
  bool alertable = arg(ctx, 2) & 0xFF;
  ret_status(ctx, after_wait(ctx, wait(&obj, 1, false, decode_timeout(mem_base, arg(ctx, 3)), alertable)));
}
XBOX_IMPORT(NtWaitForMultipleObjectsEx) {  // (Count, PHANDLE, WaitType, Mode, Alertable, Timeout)
  bool alertable = arg(ctx, 4) & 0xFF;
  ret_status(ctx, after_wait(ctx, wait_handles(mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2) == 0, arg(ctx, 5),
                                               alertable)));
}
XBOX_IMPORT(KeDelayExecutionThread) {  // (Mode, Alertable, PLARGE_INTEGER Interval)
  uint64_t t = decode_timeout(mem_base, arg(ctx, 2));
  if (arg(ctx, 1) & 0xFF) {  // alertável: a espera pelo tempo pode ser interrompida por uma APC
    uint32_t st = wait(nullptr, 0, false, t == kInfinite ? 0 : t, true);
    if (st == kStatusUserApc) return ret_status(ctx, after_wait(ctx, st));
    if (t == 0) std::this_thread::yield();
    return ret_status(ctx, kStatusSuccess);
  }
  if (t == 0 || t == kInfinite) std::this_thread::yield();
  else std::this_thread::sleep_for(std::chrono::nanoseconds(t * 100));
  ret_status(ctx, kStatusSuccess);
}

// ---- Spinlocks / IRQL (o "lock" é um u32 BE convidado: 0 livre, senão o KTHREAD dono) ----
namespace {
void spin_acquire(uint8_t* m, uint32_t lock) {
  uint32_t me = current_kthread();
  while (!atomic_cas32(m, lock, 0, me ? me : 1)) std::this_thread::yield();
}
void spin_release(uint8_t* m, uint32_t lock) { ppc_st32(m, lock, 0); }  // ppc_st32 + barreira abaixo
uint8_t raise_irql(uint8_t to) {
  GuestThread* t = current_thread();
  uint8_t old = t ? t->irql : 0;
  if (t) t->irql = to;
  return old;
}
}  // namespace

XBOX_IMPORT(KfAcquireSpinLock) {
  spin_acquire(mem_base, arg(ctx, 0));
  ret(ctx, raise_irql(2));  // DISPATCH_LEVEL
}
XBOX_IMPORT(KfReleaseSpinLock) {  // (PKSPIN_LOCK, KIRQL OldIrql)
  std::atomic_thread_fence(std::memory_order_seq_cst);
  spin_release(mem_base, arg(ctx, 0));
  raise_irql((uint8_t)arg(ctx, 1));
}
XBOX_IMPORT(KeAcquireSpinLockAtRaisedIrql) { spin_acquire(mem_base, arg(ctx, 0)); }
XBOX_IMPORT(KeReleaseSpinLockFromRaisedIrql) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
  spin_release(mem_base, arg(ctx, 0));
}
XBOX_IMPORT(KeRaiseIrqlToDpcLevel) { ret(ctx, raise_irql(2)); }
XBOX_IMPORT(KfLowerIrql) { raise_irql((uint8_t)arg(ctx, 0)); }
XBOX_IMPORT(KeEnterCriticalRegion) {}
XBOX_IMPORT(KeLeaveCriticalRegion) {}
