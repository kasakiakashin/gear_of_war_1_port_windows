// diag.h — diagnóstico do bring-up: último import chamado, relatório de crash.
#pragma once

#include <cstdint>

namespace diag {

// Nome do último import chamado pela thread atual (para relatórios de falha).
void set_last_import(const char* name);
const char* last_import();
// Imprime contexto (último import, estatísticas de imports) antes de o processo morrer.
void crash_report();
void set_silent();  // o log normal está desligado: crash_report passa a escrever em gow_crash.log
// Backtrace convidado: segue a cadeia de frames (r1 -> [r1]) e o LR salvo em [frame_anterior - 8].
void guest_backtrace(const char* why, uint64_t sp, uint64_t lr);
// Última conversão de float para texto da CRT do jogo nesta thread (_cfltcvt chamado pelo printf, 0x828DCEC0): argumentos e
// cadeia de chamadas, impressos no relatório de crash. O printf já rebentou a pilha a escrever zeros sem fim (a pilha toda acima
// do buffer fica destruída, e com ela o backtrace normal).
void note_float_format(const uint8_t* mem, uint32_t value_ptr, uint32_t buf, uint32_t size, uint32_t fmt, uint32_t precision,
                       uint64_t sp, uint64_t lr);

}  // namespace diag
