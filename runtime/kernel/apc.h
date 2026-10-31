// apc.h — APCs de modo utilizador: rotinas convidadas enfileiradas numa thread e executadas por ela quando entra
// numa espera alertável (Ke/NtWaitFor*(Alertable = TRUE), KeDelayExecutionThread(Alertable = TRUE)).
//
// Origens no runtime: ApcRoutine de NtReadFile/NtWriteFile/NtQueryDirectoryFile (ApcContext, IoStatusBlock, 0),
// rotina de NtSetTimerEx (ApcContext, FILETIME baixo, alto) e pCompletionRoutine do XOVERLAPPED
// (dwErrorCode, dwNumberOfBytes, pOverlapped). A espera que as entrega devolve STATUS_USER_APC.
#pragma once

#include <cstdint>

#include "ppc_context.h"

namespace kernel {

struct GuestThread;

// Enfileira routine(a0, a1, a2) em `t` (nullptr = thread atual) e acorda-a se estiver numa espera alertável.
void queue_user_apc(GuestThread* t, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2);
bool user_apc_pending(GuestThread* t);
// Executa (na thread atual, sobre `ctx`) todas as APCs pendentes; devolve quantas correram. Os registradores do
// convidado são preservados e cada rotina corre numa pilha abaixo de r1.
uint32_t deliver_user_apcs(PPCContext& ctx);

}  // namespace kernel
