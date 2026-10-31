// diag.h — diagnóstico do bring-up: último import chamado, relatório de crash.
#pragma once

#include <cstdint>

namespace diag {

// Nome do último import chamado pela thread atual (para relatórios de falha).
void set_last_import(const char* name);
const char* last_import();
// Imprime contexto (último import, estatísticas de imports) antes de o processo morrer.
void crash_report();
// Backtrace convidado: segue a cadeia de frames (r1 -> [r1]) e o LR salvo em [frame_anterior - 8].
void guest_backtrace(const char* why, uint64_t sp, uint64_t lr);

}  // namespace diag
