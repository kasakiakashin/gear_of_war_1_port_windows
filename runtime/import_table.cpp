#include "import_table.h"

const ImportBinding kImportBindings[] = {
#include "import_bindings.inc"
};
const size_t kImportBindingCount = sizeof(kImportBindings) / sizeof(kImportBindings[0]);
