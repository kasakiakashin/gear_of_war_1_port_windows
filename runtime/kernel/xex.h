// xex.h — módulo do título (default.xex) visto pelo convidado: LDR_DATA_TABLE_ENTRY + cabeçalho XEX copiado.
#pragma once

#include <cstdint>

namespace xex {

// Endereço convidado do X_LDR_DATA_TABLE_ENTRY do título (criado na 1ª chamada; 0 se o XEX não abrir).
uint32_t module_handle();

}  // namespace xex
