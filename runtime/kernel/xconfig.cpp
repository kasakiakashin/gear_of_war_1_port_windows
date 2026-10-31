// xconfig.cpp — privilégios do XEX e configuração do sistema (ExGetXConfigSetting).
#include <cstdio>

#include "kernel/kernel.h"
#include "language.h"

using namespace kernel;

namespace {
constexpr uint32_t kXexSystemFlags = 0x00000600;  // optional header SYSTEM_FLAGS do default.xex
constexpr uint32_t kStatusInvalidParameter2 = 0xC00000F0;
}  // namespace

// XexCheckExecutablePrivilege(ULONG Privilege) -> bit `Privilege` das flags de sistema do XEX.
XBOX_IMPORT(XexCheckExecutablePrivilege) {
  uint32_t p = arg(ctx, 0);
  ret(ctx, p < 32 ? (kXexSystemFlags >> p) & 1 : 0);
}

// ExGetXConfigSetting(USHORT Category, USHORT Setting, PVOID Buffer, USHORT BufferSize, PUSHORT RequiredSize)
// Valores de uma consola retail NTSC-U, 720p widescreen; idioma e país vêm de Language= no gow_config.ini.
XBOX_IMPORT(ExGetXConfigSetting) {
  uint32_t cat = arg(ctx, 0) & 0xFFFF, setting = arg(ctx, 1) & 0xFFFF, buf = arg(ctx, 2);
  uint32_t buf_size = arg(ctx, 3) & 0xFFFF, req_ptr = arg(ctx, 4);
  uint32_t value = 0, size = 4;
  bool known = true;
  if (cat == 0x0003) {  // XCONFIG_USER_CATEGORY
    switch (setting) {
      case 0x0001: value = 0; break;           // TIME_ZONE_BIAS
      case 0x0009: value = language::current().xbox; break;  // LANGUAGE
      case 0x000A: value = 0x00040000; break;  // VIDEO_FLAGS: widescreen
      case 0x000B: value = 0; break;           // AUDIO_FLAGS: estéreo
      case 0x000C: value = 0x40; break;        // RETAIL_FLAGS
      case 0x000E: value = language::current().country; size = 1; break;  // COUNTRY
      default: known = false;
    }
  } else if (cat == 0x0002 && setting == 0x0002) {  // XCONFIG_SECURED: AV_REGION = NTSC-U
    value = 0x00001000;
  } else {
    known = false;
  }
  if (!known) {
    std::fprintf(stderr, "[xconfig] configuração desconhecida: categoria 0x%X item 0x%X\n", cat, setting);
    return ret_status(ctx, kStatusInvalidParameter2);
  }
  if (req_ptr) ppc_st16(mem_base, req_ptr, (uint16_t)size);
  if (buf) {
    if (buf_size < size) return ret_status(ctx, kStatusBufferTooSmall);
    if (size == 1) ppc_st8(mem_base, buf, (uint8_t)value);
    else ppc_st32(mem_base, buf, value);
  }
  ret_status(ctx, kStatusSuccess);
}
