#include "loader.h"

#include <cstdio>
#include <cstring>

#include "guest_memory.h"
#include "import_table.h"
#include "kernel/sync.h"
#include "kernel/time_bundle.h"
#include "kernel/xex.h"
#include "ppc_context.h"

namespace loader {
namespace {

void put_string(uint32_t addr, const char* s) { std::memcpy(gm::base() + addr, s, std::strlen(s) + 1); }

// Objeto que uma variável exportada pelo kernel aponta; conteúdo mínimo plausível.
uint32_t make_variable(const char* name) {
  uint8_t* m = gm::base();
  uint32_t p = gm::alloc_runtime(0x100, 16);
  if (!p) return 0;
  if (!std::strcmp(name, "XboxKrnlVersion")) {  // {major, minor, build, qfe} u16 BE
    ppc_st16(m, p + 0, 2);
    ppc_st16(m, p + 2, 0);
    ppc_st16(m, p + 4, 3529);
    ppc_st16(m, p + 6, 0);
  } else if (!std::strcmp(name, "VdGpuClockInMHz")) {
    ppc_st32(m, p, 500);
  } else if (!std::strcmp(name, "ExLoadedCommandLine")) {
    put_string(p, "\"default.xex\"");
  } else if (!std::strcmp(name, "KeTimeStampBundle")) {
    kernel::start_time_stamp_bundle(p);
  } else if (!std::strcmp(name, "VdHSIOCalibrationLock")) {
    kernel::init_critical_section(m, p);  // RTL_CRITICAL_SECTION exportada: tem de começar livre
  } else if (!std::strcmp(name, "XexExecutableModuleHandle")) {
    ppc_st32(m, p, xex::module_handle());  // *XexExecutableModuleHandle = LDR_DATA_TABLE_ENTRY do título
  }
  // ExThreadObjectType, VdGlobalDevice, Ke*MonitorData: zerados até o
  // kernel HLE os modelar.
  return p;
}

}  // namespace

bool load_image(const std::vector<uint8_t>& image) {
  if (!gm::commit(g_ppc_image_base, g_ppc_image_size)) {
    std::fprintf(stderr, "[loader] falha ao confirmar a região da imagem\n");
    return false;
  }
  const size_t n = image.size();
  if (n != g_ppc_image_size) {
    std::fprintf(stderr, "[loader] imagem base com %zu bytes, esperado 0x%X\n", n, g_ppc_image_size);
    return false;
  }
  std::memcpy(gm::base() + g_ppc_image_base, image.data(), n);
  std::fprintf(stderr, "[loader] imagem: %zu bytes em 0x%08X, entry 0x%08X\n", n, g_ppc_image_base,
               g_ppc_entry_point);
  return true;
}

bool bind_imports() {
  uint8_t* m = gm::base();
  for (size_t i = 0; i < kImportBindingCount; i++) {
    const ImportBinding& b = kImportBindings[i];
    if (!b.slot) continue;
    if (b.variable) {
      uint32_t obj = make_variable(b.name);
      if (!obj) return false;
      ppc_st32(m, b.slot, obj);
    } else if (b.thunk) {
      ppc_st32(m, b.slot, b.thunk);  // permite chamadas via ponteiro do registo de import
    }
  }
  return true;
}

}  // namespace loader
