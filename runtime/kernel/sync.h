// sync.h — objetos de dispatcher do kernel (eventos, semáforos, mutantes, threads) e tabela de handles.
//
// Os objetos vivem na memória convidada com o DISPATCHER_HEADER do NT (big-endian):
//   +0x0 u8 Type  +0x1 u8 Absolute  +0x2 u8 Size  +0x3 u8 Inserted  +0x4 s32 SignalState  +0x8 LIST_ENTRY
// O estado é a própria SignalState na memória convidada (o jogo inicializa eventos inline, sem import).
// Todas as alterações e esperas passam por um mutex global + condition_variable (simples e correto para o
// bring-up; trocar por filas de espera por objeto se virar gargalo).
#pragma once

#include <cstdint>

namespace kernel {

enum ObjectType : uint8_t {
  kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kProcess = 3, kQueue = 4, kSemaphore = 5,
  kThread = 6, kNotificationTimer = 8, kSynchronizationTimer = 9,
};

constexpr uint32_t kCurrentProcessHandle = 0xFFFFFFFFu, kCurrentThreadHandle = 0xFFFFFFFEu;
constexpr uint64_t kInfinite = ~0ull;

// Timeout do NT (ponteiro para LARGE_INTEGER BE; 0 = infinito) -> intervalo em 100 ns (kInfinite).
uint64_t decode_timeout(uint8_t* m, uint32_t timeout_ptr);

// Espera: devolve STATUS_WAIT_0 + i, STATUS_TIMEOUT ou STATUS_ABANDONED_WAIT_0.
// Alertable: devolve STATUS_USER_APC se a thread atual tiver (ou receber durante a espera) APCs de utilizador;
// quem chamou executa-as com deliver_user_apcs. count = 0 é uma espera só pelo tempo (KeDelayExecutionThread).
uint32_t wait(const uint32_t* objects, uint32_t count, bool wait_all, uint64_t timeout_100ns,
              bool alertable = false);
// Acorda todas as esperas para reavaliarem as condições (usado ao enfileirar APCs).
void notify_waiters();
int32_t set_event(uint32_t obj);    // devolve o estado anterior
int32_t reset_event(uint32_t obj);
int32_t pulse_event(uint32_t obj);
int32_t release_semaphore(uint32_t obj, int32_t adjustment, uint32_t* status);
uint32_t release_mutant(uint32_t obj, int32_t* previous);
void init_header(uint32_t obj, uint8_t type, int32_t signal, uint8_t size_words);

// Handles (0xF8000000+). Objetos criados por Nt* são alocados na área do runtime.
uint32_t create_handle(uint32_t obj);
uint32_t object_of(uint32_t handle);  // 0 se inválido
bool close_handle(uint32_t handle);
bool has_handles(uint32_t obj);       // ainda existe algum handle para o objeto?
uint32_t alloc_object(uint32_t size);
// Evento anónimo (0x10 bytes na área do runtime) + handle; usado pelo XAM (listeners, WSACreateEvent).
uint32_t create_event_handle(bool manual_reset, bool signaled, uint32_t* object = nullptr);
// RTL_CRITICAL_SECTION livre (LockCount = -1); usado por RtlInitializeCriticalSection e pelo loader.
void init_critical_section(uint8_t* m, uint32_t cs);
// Procura/regista um objeto nomeado (OBJECT_ATTRIBUTES convidado); devolve 0 se não houver nome/objeto.
uint32_t named_object(uint32_t object_attributes, uint32_t create_obj, bool* existed);

}  // namespace kernel
