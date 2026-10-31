// import_table.h — imports do default.xex (tabela gerada em import_bindings.inc).
#pragma once

#include <cstddef>
#include <cstdint>

struct ImportBinding {
  uint32_t lib;      // 0 = xam.xex, 1 = xboxkrnl.exe
  uint32_t ordinal;
  const char* name;
  bool variable;
  uint32_t slot;     // endereço do registo de import na imagem
  uint32_t thunk;    // endereço do thunk (só funções)
};

extern const ImportBinding kImportBindings[];
extern const size_t kImportBindingCount;
