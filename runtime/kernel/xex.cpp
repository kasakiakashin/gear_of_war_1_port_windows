// xex.cpp — XexGetModuleHandle / XexGetModuleSection / XexExecutableModuleHandle.
//
// O cabeçalho do default.xex (até ao início dos dados PE) é copiado para a área do runtime, como o loader do
// kernel faz; LDR_DATA_TABLE_ENTRY.XexHeaderBase (+0x58) aponta para ele. As "secções" de XexGetModuleSection
// são os recursos do cabeçalho opcional RESOURCE_INFO (0x000002FF): {char name[8]; u32 address; u32 size}.
#include "kernel/xex.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "guest_memory.h"
#include "kernel/kernel.h"
#include "kernel/vfs.h"

using namespace kernel;

namespace xex {
namespace {

struct Resource {
  std::string name;
  uint32_t address, size;
};

struct Module {
  uint32_t ldr = 0, header = 0;
  std::vector<Resource> resources;
};

uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
  return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3]
                           : 0;
}

// UNICODE_STRING {u16 Length, u16 MaximumLength, u32 Buffer} com texto UTF-16 BE logo a seguir.
uint32_t put_unicode(uint8_t* m, uint32_t us, uint32_t buf, const char* s) {
  uint16_t n = (uint16_t)std::strlen(s);
  for (uint16_t i = 0; i < n; i++) ppc_st16(m, buf + 2 * i, (uint8_t)s[i]);
  ppc_st16(m, buf + 2 * n, 0);
  ppc_st16(m, us + 0, n * 2);
  ppc_st16(m, us + 2, n * 2 + 2);
  ppc_st32(m, us + 4, buf);
  return buf + 2 * n + 2;
}

Module build() {
  Module mod;
  std::vector<uint8_t> d;
  if (!vfs::read_game_file("default.xex", &d, 0x10000) || d.size() < 0x18 || std::memcmp(d.data(), "XEX2", 4) != 0) {
    std::fprintf(stderr, "[xex] não foi possível ler o cabeçalho do default.xex\n");
    return mod;
  }
  uint32_t pe_offset = be32(d, 8), count = be32(d, 0x14);
  uint32_t header_size = std::min<uint32_t>(pe_offset, (uint32_t)d.size());
  uint8_t* m = gm::base();
  mod.header = gm::alloc_runtime((header_size + 0xFFF) & ~0xFFFu, 0x1000);
  std::memcpy(m + mod.header, d.data(), header_size);  // bytes brutos: já estão em BE
  for (uint32_t i = 0; i < count; i++) {
    uint32_t key = be32(d, 0x18 + 8 * i), val = be32(d, 0x1C + 8 * i);
    if (key != 0x000002FF) continue;
    uint32_t size = be32(d, val);
    for (uint32_t o = val + 4; o + 16 <= val + size; o += 16) {
      std::string name(reinterpret_cast<const char*>(&d[o]), strnlen(reinterpret_cast<const char*>(&d[o]), 8));
      mod.resources.push_back({name, be32(d, o + 8), be32(d, o + 12)});
    }
  }

  // X_LDR_DATA_TABLE_ENTRY (layout do Xenia): listas +0/+8/+0x10, DllBase +0x18, ImageBase +0x1C,
  // SizeOfImage +0x20, FullDllName +0x24, BaseDllName +0x2C, Flags +0x34, FullImageSize +0x38,
  // EntryPoint +0x3C, LoadCount +0x40, ModuleIndex +0x42, DllBaseOriginal +0x44, CheckSum +0x48,
  // LoadFlags +0x4C, TimeDateStamp +0x50, LoadedImports +0x54, XexHeaderBase +0x58.
  uint32_t ldr = mod.ldr = gm::alloc_runtime(0x200, 16);
  for (uint32_t l = 0; l < 0x18; l += 8) {  // listas vazias (apontam para si próprias)
    ppc_st32(m, ldr + l, ldr + l);
    ppc_st32(m, ldr + l + 4, ldr + l);
  }
  ppc_st32(m, ldr + 0x18, g_ppc_image_base);
  ppc_st32(m, ldr + 0x1C, g_ppc_image_base);
  ppc_st32(m, ldr + 0x20, g_ppc_image_size);
  uint32_t strings = put_unicode(m, ldr + 0x24, ldr + 0x100, "game:\\default.xex");
  put_unicode(m, ldr + 0x2C, strings, "default.xex");
  ppc_st32(m, ldr + 0x34, 0);
  ppc_st32(m, ldr + 0x38, g_ppc_image_size);
  ppc_st32(m, ldr + 0x3C, g_ppc_entry_point);
  ppc_st16(m, ldr + 0x40, 1);
  ppc_st32(m, ldr + 0x44, g_ppc_image_base);
  ppc_st32(m, ldr + 0x58, mod.header);
  std::fprintf(stderr, "[xex] módulo do título: LDR 0x%08X, cabeçalho 0x%08X (0x%X bytes), %zu recursos\n", ldr,
               mod.header, header_size, mod.resources.size());
  for (auto& r : mod.resources)
    std::fprintf(stderr, "[xex]   recurso %-8s 0x%08X +0x%X\n", r.name.c_str(), r.address, r.size);
  return mod;
}

const Module& module() {
  static std::once_flag once;
  static Module mod;
  std::call_once(once, [] { mod = build(); });
  return mod;
}

bool is_title_name(const std::string& name) {
  std::string l;
  for (char c : name) l += (char)std::tolower((unsigned char)c);
  size_t slash = l.find_last_of("\\/:");
  if (slash != std::string::npos) l = l.substr(slash + 1);
  return l == "default.xex";
}

}  // namespace

uint32_t module_handle() { return module().ldr; }

}  // namespace xex

// XexGetModuleHandle(PSZ ModuleName, PHANDLE hModule): nome nulo = módulo do título.
XBOX_IMPORT(XexGetModuleHandle) {
  uint32_t name_ptr = arg(ctx, 0), out = arg(ctx, 1);
  std::string name = name_ptr ? reinterpret_cast<const char*>(PPC_HOST(mem_base, name_ptr)) : "";
  if (name_ptr && !xex::is_title_name(name)) {
    std::fprintf(stderr, "[xex] XexGetModuleHandle(\"%s\"): módulo não modelado\n", name.c_str());
    out32(mem_base, out, 0);
    return ret_status(ctx, kStatusDllNotFound);
  }
  uint32_t h = xex::module_handle();
  out32(mem_base, out, h);
  ret_status(ctx, h ? kStatusSuccess : kStatusDllNotFound);
}

// XexGetModuleSection(HANDLE hModule, PSZ Name, PVOID* Data, PULONG Size)
XBOX_IMPORT(XexGetModuleSection) {
  uint32_t name_ptr = arg(ctx, 1);
  std::string name = name_ptr ? reinterpret_cast<const char*>(PPC_HOST(mem_base, name_ptr)) : "";
  for (const auto& r : xex::module().resources) {
    if (r.name != name) continue;
    out32(mem_base, arg(ctx, 2), r.address);
    out32(mem_base, arg(ctx, 3), r.size);
    return ret_status(ctx, kStatusSuccess);
  }
  std::fprintf(stderr, "[xex] XexGetModuleSection(0x%08X, \"%s\"): não existe\n", arg(ctx, 0), name.c_str());
  ret_status(ctx, kStatusNotFound);
}

// Cabeçalho opcional `key` de um cabeçalho XEX na memória convidada (tabela {u32 key; u32 value} em +0x18).
// (key & 0xFF) == 0: o valor é o próprio dado; == 1: dado de 4 bytes no campo value; senão value = deslocamento.
static uint32_t xex_opt_header(uint8_t* m, uint32_t header, uint32_t key) {
  if (!header) return 0;
  uint32_t count = ppc_ld32(m, header + 0x14);
  for (uint32_t i = 0; i < count && i < 0x100; i++) {
    uint32_t e = header + 0x18 + 8 * i;
    if (ppc_ld32(m, e) != key) continue;
    uint32_t value = ppc_ld32(m, e + 4);
    switch (key & 0xFF) {
      case 0x00: return value;
      case 0x01: return e + 4;
      default: return header + value;
    }
  }
  return 0;
}

// RtlImageXexHeaderField(PVOID XexHeaderBase, DWORD Key) -> valor/ponteiro (0 se não existir)
XBOX_IMPORT(RtlImageXexHeaderField) { ret(ctx, xex_opt_header(mem_base, arg(ctx, 0), arg(ctx, 1))); }

// XamGetExecutionId(XEX_EXECUTION_ID** Info): cabeçalho opcional EXECUTION_INFO (0x00040006) do título.
XBOX_IMPORT(XamGetExecutionId) {
  uint32_t ldr = xex::module_handle();
  uint32_t info = ldr ? xex_opt_header(mem_base, ppc_ld32(mem_base, ldr + 0x58), 0x00040006) : 0;
  out32(mem_base, arg(ctx, 0), info);
  ret(ctx, info ? 0u : 0x490u);  // ERROR_NOT_FOUND
}
