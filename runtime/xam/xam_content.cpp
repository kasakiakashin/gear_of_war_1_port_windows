// xam_content.cpp — pacotes de conteúdo (saves), dispositivo de armazenamento e enumeradores (XamEnumerate).
//
// Há um único dispositivo: o disco rígido (id 1). Um pacote é uma pasta do host:
//   user_data/content/<ContentType %08X>/<FileName>/       conteúdo (montado como "<root>:" no vfs)
//   user_data/content/<ContentType %08X>/<FileName>.name   DisplayName em UTF-8
// XCONTENT_DATA (0x134 bytes BE): +0 DeviceId, +4 ContentType, +8 WCHAR DisplayName[128], +0x108 CHAR FileName[42].
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "kernel/vfs.h"
#include "xam/xam.h"

using namespace kernel;
using namespace xam;

namespace {

constexpr uint32_t kDeviceHdd = 1, kContentDataSize = 0x134;
constexpr uint64_t kHddBytes = 20ull << 30, kHddFree = 16ull << 30;

namespace fs = std::filesystem;

struct ContentData {
  uint32_t device = kDeviceHdd, type = 0;
  std::string display, file;
};

ContentData read_content_data(uint8_t* m, uint32_t p) {
  ContentData d;
  d.device = ppc_ld32(m, p);
  d.type = ppc_ld32(m, p + 4);
  d.display = read_utf16(m, p + 8, 128);
  const char* f = reinterpret_cast<const char*>(PPC_HOST(m, p + 0x108));
  d.file.assign(f, strnlen(f, 42));
  return d;
}

void write_content_data(uint8_t* m, uint32_t p, const ContentData& d) {
  for (uint32_t o = 0; o < kContentDataSize; o += 4) ppc_st32(m, p + o, 0);
  ppc_st32(m, p, d.device);
  ppc_st32(m, p + 4, d.type);
  write_utf16(m, p + 8, d.display, 128);
  for (size_t i = 0; i < d.file.size() && i < 41; i++) ppc_st8(m, p + 0x108 + (uint32_t)i, (uint8_t)d.file[i]);
}

fs::path type_dir(uint32_t type) {
  char t[16];
  std::snprintf(t, sizeof(t), "%08X", type);
  return vfs::user_root() / "content" / t;
}
fs::path package_dir(const ContentData& d) { return type_dir(d.type) / d.file; }
fs::path name_file(const ContentData& d) { return type_dir(d.type) / (d.file + ".name"); }

std::string guest_cstr(uint8_t* m, uint32_t p) {
  return p ? std::string(reinterpret_cast<const char*>(PPC_HOST(m, p))) : std::string();
}

// ---- Enumeradores (handle de evento; os itens são blocos de bytes já no formato convidado) ----
struct Enumerator {
  uint32_t item_size, per_call;
  std::vector<std::vector<uint8_t>> items;
  size_t next = 0;
};
std::mutex g_mu;
std::map<uint32_t, Enumerator> g_enums;

}  // namespace

// XamContentGetDeviceData(DWORD DeviceId, X_CONTENT_DEVICE_DATA* Data)
// X_CONTENT_DEVICE_DATA (0x50): DeviceId, DeviceType, u64 TotalBytes, u64 FreeBytes, WCHAR Name[28].
XBOX_IMPORT(XamContentGetDeviceData) {
  uint32_t dev = arg(ctx, 0), p = arg(ctx, 1);
  if (dev != kDeviceHdd) return ret(ctx, kErrorDeviceNotConnected);
  for (uint32_t o = 0; o < 0x50; o += 4) ppc_st32(mem_base, p + o, 0);
  ppc_st32(mem_base, p + 0x00, kDeviceHdd);
  ppc_st32(mem_base, p + 0x04, 1);  // XCONTENTDEVICETYPE_HDD
  ppc_st64(mem_base, p + 0x08, kHddBytes);
  ppc_st64(mem_base, p + 0x10, kHddFree);
  write_utf16(mem_base, p + 0x18, "Hard Drive", 28);
  ret(ctx, kErrorSuccess);
}

// XamContentGetDeviceState(DWORD DeviceId, PXOVERLAPPED)
XBOX_IMPORT(XamContentGetDeviceState) {
  uint32_t r = arg(ctx, 0) == kDeviceHdd ? kErrorSuccess : kErrorDeviceNotConnected;
  ret(ctx, finish(mem_base, arg(ctx, 1), r));
}

// XamShowDeviceSelectorUI(DWORD UserIndex, DWORD ContentType, DWORD ContentFlags, QWORD BytesRequested,
//                         PDWORD DeviceId, PXOVERLAPPED): escolhe sempre o disco rígido.
XBOX_IMPORT(XamShowDeviceSelectorUI) {
  out32(mem_base, arg(ctx, 4), kDeviceHdd);
  std::fprintf(stderr, "[xam] XamShowDeviceSelectorUI -> disco rígido\n");
  system_ui_shown();
  ret(ctx, finish(mem_base, arg(ctx, 5), kErrorSuccess));
}

// XamContentCreateEx(DWORD UserIndex, PSZ RootName, XCONTENT_DATA*, DWORD Flags, PDWORD Disposition,
//                    PDWORD LicenseMask, DWORD CacheSize, QWORD ContentSize, PXOVERLAPPED [pilha])
// Flags & 0xF: 1 CREATE_NEW, 2 CREATE_ALWAYS, 3 OPEN_EXISTING, 4 OPEN_ALWAYS, 5 TRUNCATE_EXISTING.
XBOX_IMPORT(XamContentCreateEx) {
  std::string root = guest_cstr(mem_base, arg(ctx, 1));
  ContentData d = read_content_data(mem_base, arg(ctx, 2));
  uint32_t flags = arg(ctx, 3), disp_ptr = arg(ctx, 4), license_ptr = arg(ctx, 5);
  uint32_t ov = stack_arg(ctx, mem_base, 8);
  fs::path dir = package_dir(d);
  std::error_code ec;
  bool exists = fs::is_directory(dir, ec);
  uint32_t result = kErrorSuccess, disposition = 0;
  switch (flags & 0xF) {
    case 1:  // CREATE_NEW
      if (exists) result = kErrorAlreadyExists;
      else disposition = 1;
      break;
    case 2:  // CREATE_ALWAYS
      if (exists) fs::remove_all(dir, ec);
      disposition = 1;
      break;
    case 3:  // OPEN_EXISTING
      if (!exists) result = kErrorPathNotFound;
      else disposition = 2;
      break;
    case 4:  // OPEN_ALWAYS
      disposition = exists ? 2 : 1;
      break;
    case 5:  // TRUNCATE_EXISTING
      if (!exists) result = kErrorPathNotFound;
      else fs::remove_all(dir, ec), disposition = 1;
      break;
    default:
      result = kErrorInvalidParameter;
  }
  if (result == kErrorSuccess) {
    fs::create_directories(dir, ec);
    if (disposition == 1 || !fs::exists(name_file(d), ec)) std::ofstream(name_file(d), std::ios::trunc) << d.display;
    vfs::mount(root, dir);
  }
  out32(mem_base, disp_ptr, disposition);
  out32(mem_base, license_ptr, 0);
  std::fprintf(stderr, "[xam] XamContentCreateEx(\"%s\", tipo 0x%08X, \"%s\" \"%s\", flags 0x%X) -> %u (disp %u)\n",
               root.c_str(), d.type, d.file.c_str(), d.display.c_str(), flags, result, disposition);
  ret(ctx, finish(mem_base, ov, result));
}

// XamContentClose(PSZ RootName, PXOVERLAPPED)
XBOX_IMPORT(XamContentClose) {
  std::string root = guest_cstr(mem_base, arg(ctx, 0));
  bool ok = vfs::unmount(root);
  ret(ctx, finish(mem_base, arg(ctx, 1), ok ? kErrorSuccess : kErrorNotFound));
}

// XamContentDelete(DWORD UserIndex, XCONTENT_DATA*, PXOVERLAPPED)
XBOX_IMPORT(XamContentDelete) {
  ContentData d = read_content_data(mem_base, arg(ctx, 1));
  std::error_code ec;
  bool ok = fs::remove_all(package_dir(d), ec) > 0;
  fs::remove(name_file(d), ec);
  std::fprintf(stderr, "[xam] XamContentDelete(tipo 0x%08X, \"%s\")\n", d.type, d.file.c_str());
  ret(ctx, finish(mem_base, arg(ctx, 2), ok ? kErrorSuccess : kErrorFileNotFound));
}

// XamContentGetCreator(DWORD UserIndex, XCONTENT_DATA*, PBOOL IsCreator, PXUID Creator, PXOVERLAPPED)
XBOX_IMPORT(XamContentGetCreator) {
  out32(mem_base, arg(ctx, 2), 1);
  if (arg(ctx, 3)) ppc_st64(mem_base, arg(ctx, 3), xuid(arg(ctx, 0)));
  ret(ctx, finish(mem_base, arg(ctx, 4), kErrorSuccess));
}

// XamContentCreateEnumerator(DWORD UserIndex, DWORD DeviceId, DWORD ContentType, DWORD ContentFlags,
//                            DWORD ItemsPerEnumerate, PDWORD BufferSize, PHANDLE Enum)
XBOX_IMPORT(XamContentCreateEnumerator) {
  uint32_t dev = arg(ctx, 1), type = arg(ctx, 2), per = arg(ctx, 4), size_ptr = arg(ctx, 5), handle_ptr = arg(ctx, 6);
  if (!handle_ptr || !per) return ret(ctx, kErrorInvalidParameter);
  Enumerator e{kContentDataSize, per, {}, 0};
  std::error_code ec;
  if (dev == 0 || dev == kDeviceHdd) {
    for (auto& entry : fs::directory_iterator(type_dir(type), ec)) {
      if (!entry.is_directory(ec)) continue;
      ContentData d;
      d.type = type;
      d.file = entry.path().filename().string();
      std::ifstream in(name_file(d));
      std::getline(in, d.display);
      std::vector<uint8_t> bytes(kContentDataSize);
      // Monta o XCONTENT_DATA num buffer local BE (mesmo formato que write_content_data).
      auto st32 = [&](size_t o, uint32_t v) {
        for (int k = 0; k < 4; k++) bytes[o + k] = (uint8_t)(v >> (24 - 8 * k));
      };
      st32(0, kDeviceHdd);
      st32(4, type);
      for (size_t i = 0; i < d.display.size() && i < 127; i++) bytes[9 + 2 * i] = (uint8_t)d.display[i];
      for (size_t i = 0; i < d.file.size() && i < 41; i++) bytes[0x108 + i] = (uint8_t)d.file[i];
      e.items.push_back(std::move(bytes));
    }
  }
  out32(mem_base, size_ptr, per * kContentDataSize);
  uint32_t handle = create_event_handle(true, true);
  std::fprintf(stderr, "[xam] XamContentCreateEnumerator(dispositivo %u, tipo 0x%08X) -> 0x%08X, %zu pacotes\n", dev,
               type, handle, e.items.size());
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_enums[handle] = std::move(e);
  }
  ppc_st32(mem_base, handle_ptr, handle);
  ret(ctx, kErrorSuccess);
}

uint32_t xam::create_empty_enumerator(uint32_t item_size, uint32_t per_call) {
  uint32_t handle = create_event_handle(true, true);
  std::lock_guard<std::mutex> lock(g_mu);
  g_enums[handle] = Enumerator{item_size, per_call ? per_call : 1, {}, 0};
  return handle;
}

// XamEnumerate(HANDLE Enum, DWORD Flags, PVOID Buffer, DWORD BufferSize, PDWORD ItemsReturned, PXOVERLAPPED)
XBOX_IMPORT(XamEnumerate) {
  uint32_t handle = arg(ctx, 0), buf = arg(ctx, 2), size = arg(ctx, 3), count_ptr = arg(ctx, 4), ov = arg(ctx, 5);
  out32(mem_base, count_ptr, 0);
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_enums.find(handle);
  if (it == g_enums.end()) {
    std::fprintf(stderr, "[xam] XamEnumerate(0x%08X): enumerador desconhecido\n", handle);
    return ret(ctx, finish(mem_base, ov, kErrorInvalidHandle));
  }
  Enumerator& e = it->second;
  uint32_t n = 0;
  while (e.next < e.items.size() && n < e.per_call && (n + 1) * e.item_size <= size) {
    const auto& item = e.items[e.next++];
    for (size_t k = 0; k < item.size(); k++) ppc_st8(mem_base, buf + n * e.item_size + (uint32_t)k, item[k]);
    n++;
  }
  uint32_t result = n ? kErrorSuccess : kErrorNoMoreFiles;
  if (!ov) out32(mem_base, count_ptr, n);
  ret(ctx, finish(mem_base, ov, result, n));
}
