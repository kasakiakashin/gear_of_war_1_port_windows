#include "import_table.h"

const ImportBinding kImportBindings[] = {
#if GOW_TU5
#include "import_bindings_tu5.inc"  // title update 5: mesmos ordinais, outras entradas da IAT e thunks
#else
#include "import_bindings.inc"
#endif
};
const size_t kImportBindingCount = sizeof(kImportBindings) / sizeof(kImportBindings[0]);
