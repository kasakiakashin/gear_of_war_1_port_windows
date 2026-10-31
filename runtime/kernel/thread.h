// thread.h — threads convidadas: cada uma é uma thread host com PPCContext, pilha, KPCR, KTHREAD e TLS próprios.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include "ppc_context.h"

namespace kernel {

constexpr uint32_t kTlsSlotCount = 64;          // XEX TLS_INFO: 64 slots, sem dados estáticos
constexpr uint32_t kDefaultStackSize = 0x40000;  // XEX DEFAULT_STACK_SIZE

struct UserApc {
  uint32_t routine = 0, a0 = 0, a1 = 0, a2 = 0;
};

struct GuestThread {
  uint32_t thread_id = 0;
  uint32_t pcr = 0;          // r13
  uint32_t kthread = 0;      // objeto KTHREAD (DISPATCHER_HEADER tipo 6; sinalizado ao terminar)
  uint32_t tls = 0;          // slots de KeTlsAlloc (u32 BE), apontado por KPCR+0x000
  uint32_t stack_base = 0;   // topo (endereço alto)
  uint32_t stack_limit = 0;  // base (endereço baixo)
  uint8_t irql = 0;
  int32_t priority = 0;
  uint32_t affinity = 0;
  uint32_t exit_code = 0;
  bool finished = false;
  // Diagnóstico (lidos por outras threads sem sincronização: só para relatórios).
  PPCContext* ctx = nullptr;
  const char* last_import = nullptr;
  // Suspensão (CREATE_SUSPENDED / NtSuspendThread da própria thread).
  std::mutex mu;
  std::condition_variable cv;
  int32_t suspend_count = 0;
  // APCs de utilizador pendentes (kernel/apc.h); protegidas por `mu`, contador lido sem lock pelas esperas.
  std::deque<UserApc> apcs;
  std::atomic<int32_t> apc_count{0};
};

// Lançada por ExTerminateThread e apanhada na base da thread host.
struct ThreadExit {
  uint32_t code;
};

// Aloca pilha + KPCR + KTHREAD + TLS (pode ser chamada noutra thread, antes de a nova arrancar).
GuestThread* alloc_guest_thread(uint32_t stack_size);
// Liga `t` à thread host atual e prepara `ctx` (r1, r13).
void bind_guest_thread(GuestThread* t, PPCContext& ctx);
GuestThread* create_guest_thread(PPCContext& ctx, uint32_t stack_size);
GuestThread* current_thread();
GuestThread* thread_by_kthread(uint32_t kthread);

// Executa uma rotina convidada até ao fim nesta thread host (apanha ThreadExit); devolve o código de saída.
uint32_t run_guest(GuestThread* t, PPCContext& ctx, uint32_t entry);
// Marca o fim da thread: guarda o código e sinaliza o KTHREAD (acorda quem espera pelo handle).
void finish_thread(GuestThread* t, uint32_t code);
// Imprime o estado de todas as threads convidadas (watchdog / crash).
void dump_threads();
// GPRs/LR/CTR da thread convidada atual (relatório de falha de memória).
void dump_current_registers();
// Amostrador (GOW_SAMPLE=1): a cada 5 ms regista lr + 5 frames de cada thread viva; print_samples() mostra as
// pilhas mais frequentes por thread (perfil grosseiro para descobrir onde uma thread gira).
void start_sampler();
void print_samples();
// Bloqueia enquanto suspend_count > 0.
void wait_while_suspended(GuestThread* t);

}  // namespace kernel
