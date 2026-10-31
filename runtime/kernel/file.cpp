// file.cpp — sistema de ficheiros do convidado (NtCreateFile/NtOpenFile/NtReadFile/NtWriteFile/NtQuery*).
//
// Cada ficheiro aberto é um objeto convidado (cabeçalho de evento de notificação sinalizado, para que esperas no
// handle do ficheiro retornem de imediato) + estado no host (HANDLE Win32, caminho, posição, enumeração).
// Todo o I/O é síncrono: NtReadFile/NtWriteFile completam antes de voltar, preenchem o IO_STATUS_BLOCK
// {u32 Status, u32 Information} (BE) e sinalizam o evento opcional.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"
#include "kernel/diag.h"
#include "kernel/apc.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "kernel/thread.h"
#include "kernel/vfs.h"

using namespace kernel;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFileAttributeReadOnly = 0x01, kFileAttributeDirectory = 0x10, kFileAttributeNormal = 0x80;
// CreateDisposition
// (FILE_OPEN_IF = 3 é o caminho por omissão: abre se existir, senão cria)
constexpr uint32_t kFileSupersede = 0, kFileOpen = 1, kFileCreate = 2, kFileOverwrite = 4, kFileOverwriteIf = 5;
// IO_STATUS_BLOCK.Information de NtCreateFile
constexpr uint32_t kFileSuperseded = 0, kFileOpened = 1, kFileCreated = 2, kFileOverwritten = 3;
// CreateOptions
constexpr uint32_t kFileDirectoryFile = 0x1, kFileNonDirectoryFile = 0x40;
// ACCESS_MASK
constexpr uint32_t kGenericWrite = 0x40000000, kGenericAll = 0x10000000, kFileWriteData = 0x2,
                   kFileAppendData = 0x4, kDelete = 0x10000;

struct DirEntry {
  std::string name;
  bool dir = false;
  uint64_t size = 0, time = 0;
};

struct File {
  std::string guest_path;
  fs::path host;
  const iso::Node* iso = nullptr;  // ficheiro só na imagem de disco (sem HANDLE), ou a parte da imagem de um diretório
  bool dir = false, writable = false;
  HANDLE h = INVALID_HANDLE_VALUE;
  std::mutex mu;
  uint64_t position = 0;
  bool delete_on_close = false;
  // Enumeração (NtQueryDirectoryFile)
  bool enum_started = false;
  std::vector<DirEntry> entries;
  size_t enum_index = 0;
  std::string mask;
  ~File() {
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (delete_on_close) {
      std::error_code ec;
      fs::remove(host, ec);
    }
  }
};

std::mutex g_files_mu;
std::unordered_map<uint32_t, std::shared_ptr<File>> g_files;  // objeto convidado -> ficheiro

bool env_flag(const char* name) {
  const char* e = std::getenv(name);
  return e && *e == '1';
}
const bool g_trace = env_flag("GOW_TRACE_FILES");

// Handle de ficheiro inválido: regista (com backtrace) a 1ª ocorrência, que costuma apontar a causa.
std::shared_ptr<File> bad_handle(uint32_t handle) {
  static std::atomic<bool> once{false};
  if (!once.exchange(true)) {
    std::fprintf(stderr, "[file] handle inválido 0x%08X (último import: %s)\n", handle, diag::last_import());
    if (GuestThread* t = current_thread(); t && t->ctx) diag::guest_backtrace("handle", t->ctx->r[1], t->ctx->lr);
  }
  return nullptr;
}

std::shared_ptr<File> file_of(uint32_t handle) {
  uint32_t obj = object_of(handle);
  if (!obj) return bad_handle(handle);
  std::lock_guard<std::mutex> lock(g_files_mu);
  auto it = g_files.find(obj);
  return it == g_files.end() ? bad_handle(handle) : it->second;
}

uint64_t to_filetime(fs::file_time_type t) {
  auto sys = std::chrono::file_clock::to_sys(t);
  auto unix_100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count() / 100;
  return (uint64_t)unix_100ns + 116444736000000000ull;  // época Unix -> FILETIME (1601)
}

struct Stat {
  bool exists = false, dir = false;
  uint64_t size = 0, time = 0;
};
Stat stat_path(const fs::path& p) {
  Stat s;
  std::error_code ec;
  auto st = fs::status(p, ec);
  if (ec || !fs::exists(st)) return s;
  s.exists = true;
  s.dir = fs::is_directory(st);
  s.size = s.dir ? 0 : fs::file_size(p, ec);
  s.time = to_filetime(fs::last_write_time(p, ec));
  return s;
}

// O ficheiro solto tem prioridade; o que só existe na imagem de disco vem do nó.
Stat stat_any(const fs::path& host, const iso::Node* node) {
  Stat s = stat_path(host);
  if (!s.exists && node) {
    s.exists = true;
    s.dir = node->dir;
    s.size = node->dir ? 0 : node->size;
    s.time = iso::volume_time();
  }
  return s;
}

uint32_t attributes(const Stat& s, bool writable) {
  if (s.dir) return kFileAttributeDirectory;
  return writable ? kFileAttributeNormal : kFileAttributeReadOnly;
}

void write_iosb(uint8_t* m, uint32_t iosb, uint32_t status, uint32_t info) {
  if (!iosb) return;
  ppc_st32(m, iosb + 0, status);
  ppc_st32(m, iosb + 4, info);
}

void complete(uint8_t* m, uint32_t event_handle, uint32_t iosb, uint32_t status, uint32_t info) {
  write_iosb(m, iosb, status, info);
  if (event_handle)
    if (uint32_t ev = object_of(event_handle)) set_event(ev);
}

// UTF-16 BE (as structs FS do NT usam WCHAR).
void put_wide(uint8_t* m, uint32_t addr, const char* s) {
  for (uint32_t i = 0; s[i]; i++) ppc_st16(m, addr + 2 * i, (uint8_t)s[i]);
}

bool wildcard_match(const char* pat, const char* str) {
  for (; *pat; pat++, str++) {
    if (*pat == '*') {
      while (pat[1] == '*') pat++;
      if (!pat[1]) return true;
      for (; *str; str++)
        if (wildcard_match(pat + 1, str)) return true;
      return false;
    }
    if (!*str) return false;
    if (*pat != '?' && std::tolower((unsigned char)*pat) != std::tolower((unsigned char)*str)) return false;
  }
  return !*str;
}

// Caminho completo do convidado, juntando OBJECT_ATTRIBUTES.RootDirectory (handle de diretório aberto) se houver.
std::string full_guest_path(uint8_t* m, uint32_t attrs, fs::path* root_host, bool* root_writable) {
  std::string name = vfs::object_name(m, attrs);
  uint32_t root = attrs ? ppc_ld32(m, attrs + 0) : 0;
  if (root && root != 0xFFFFFFFDu) {  // 0xFFFFFFFD = raiz de objetos (\??\)
    if (auto f = file_of(root)) {
      *root_host = f->host;
      *root_writable = f->writable;
      return f->guest_path + "\\" + name;
    }
  }
  return name;
}

// Abertura comum a NtCreateFile/NtOpenFile.
uint32_t open_file(PPCContext& ctx, uint8_t* m, uint32_t handle_ptr, uint32_t access, uint32_t attrs,
                   uint32_t iosb, uint32_t disposition, uint32_t options, const char* who) {
  fs::path root_host;
  bool root_writable = false;
  std::string guest = full_guest_path(m, attrs, &root_host, &root_writable);
  // GOW_BT_FILE=texto: backtrace convidado quando o caminho contém esse texto (quem montou o nome?).
  if (const char* bt = std::getenv("GOW_BT_FILE"); bt && guest.find(bt) != std::string::npos)
    diag::guest_backtrace(who, ctx.r[1], ctx.lr);
  std::optional<vfs::Resolved> r;
  if (!root_host.empty() && root_writable) {
    std::string rel = vfs::object_name(m, attrs);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    r = vfs::Resolved{rel.empty() ? root_host : root_host / rel, root_writable};
  } else {
    r = vfs::resolve(guest);
  }
  auto fail = [&](uint32_t st) {
    if (g_trace || st != kStatusObjectNameNotFound)
      std::fprintf(stderr, "[file] %s falhou 0x%08X: %s\n", who, st, guest.c_str());
    else
      std::fprintf(stderr, "[vfs] não encontrado: %s\n", guest.c_str());
    if (g_trace) diag::guest_backtrace(who, ctx.r[1], ctx.lr);
    write_iosb(m, iosb, st, 0);
    return st;
  };
  if (!r) return fail(kStatusObjectPathNotFound);

  bool wants_write = access & (kGenericWrite | kGenericAll | kFileWriteData | kFileAppendData | kDelete);
  Stat st = stat_any(r->host, r->iso);
  uint32_t info = kFileOpened;
  if (st.exists) {
    if (disposition == kFileCreate) return fail(kStatusObjectNameCollision);
    if (st.dir && (options & kFileNonDirectoryFile)) return fail(kStatusFileIsADirectory);
    if (!st.dir && (options & kFileDirectoryFile)) return fail(kStatusNotADirectory);
    if (disposition == kFileSupersede || disposition == kFileOverwrite || disposition == kFileOverwriteIf) {
      if (!r->writable) return fail(kStatusMediaWriteProtected);
      info = disposition == kFileSupersede ? kFileSuperseded : kFileOverwritten;
    }
  } else {
    if (disposition == kFileOpen || disposition == kFileOverwrite) {
      std::error_code ec;
      bool parent = r->iso_parent || fs::is_directory(r->host.parent_path(), ec);
      return fail(parent ? kStatusObjectNameNotFound : kStatusObjectPathNotFound);
    }
    if (!r->writable) return fail(kStatusMediaWriteProtected);
    std::error_code ec;
    fs::create_directories(options & kFileDirectoryFile ? r->host : r->host.parent_path(), ec);
    info = kFileCreated;
  }

  auto f = std::make_shared<File>();
  f->guest_path = guest;
  f->host = r->host;
  f->writable = r->writable;
  f->dir = st.exists ? st.dir : (options & kFileDirectoryFile) != 0;
  f->iso = r->iso;
  if (!f->dir && !f->iso) {
    DWORD acc = GENERIC_READ | (r->writable && (wants_write || info != kFileOpened) ? GENERIC_WRITE : 0);
    DWORD disp = info == kFileCreated ? CREATE_NEW : info == kFileOpened ? OPEN_EXISTING : TRUNCATE_EXISTING;
    f->h = CreateFileW(r->host.wstring().c_str(), acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f->h == INVALID_HANDLE_VALUE) {
      std::fprintf(stderr, "[file] CreateFileW erro %lu: %s\n", GetLastError(), r->host.string().c_str());
      return fail(kStatusAccessDenied);
    }
  }
  uint32_t obj = alloc_object(0x10);
  init_header(obj, kNotificationEvent, 1, 0x10 / 4);
  {
    std::lock_guard<std::mutex> lock(g_files_mu);
    g_files[obj] = f;
  }
  uint32_t h = create_handle(obj);
  ppc_st32(m, handle_ptr, h);
  write_iosb(m, iosb, kStatusSuccess, info);
  if (g_trace)
    std::fprintf(stderr, "[file] %s %s -> 0x%08X (%s%s, info %u)\n", who, guest.c_str(), h, f->dir ? "dir" : "file",
                 f->iso ? ", iso" : "", info);
  (void)ctx;
  return kStatusSuccess;
}

uint64_t file_size(File& f) {
  if (f.iso && !f.dir) return f.iso->size;
  LARGE_INTEGER sz{};
  if (f.h != INVALID_HANDLE_VALUE && GetFileSizeEx(f.h, &sz)) return (uint64_t)sz.QuadPart;
  return 0;
}

// ByteOffset (PLARGE_INTEGER, opcional): nulo ou FILE_USE_FILE_POINTER_POSITION (-2) = posição atual.
bool explicit_offset(uint8_t* m, uint32_t ptr, uint64_t* off) {
  if (!ptr) return false;
  uint64_t v = ppc_ld64(m, ptr);
  if (v == 0xFFFFFFFFFFFFFFFEull) return false;
  *off = v;
  return true;
}

// I/O síncrono: a ApcRoutine (ApcContext, IoStatusBlock, 0) fica enfileirada para a próxima espera alertável.
void queue_io_apc(const PPCContext& ctx) {
  queue_user_apc(nullptr, arg(ctx, 2), arg(ctx, 3), arg(ctx, 4), 0);
}

}  // namespace

void vfs::on_handle_closed(uint32_t object) {
  std::shared_ptr<File> f;
  {
    std::lock_guard<std::mutex> lock(g_files_mu);
    auto it = g_files.find(object);
    if (it == g_files.end() || has_handles(object)) return;
    f = std::move(it->second);
    g_files.erase(it);
  }
  if (g_trace) std::fprintf(stderr, "[file] fechado: %s\n", f->guest_path.c_str());
}

// NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER AllocationSize,
//              FileAttributes, ShareAccess, CreateDisposition, CreateOptions [pilha])
XBOX_IMPORT(NtCreateFile) {
  ret_status(ctx, open_file(ctx, mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 7),
                     stack_arg(ctx, mem_base, 8), "NtCreateFile"));
}

// NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ShareAccess, OpenOptions)
XBOX_IMPORT(NtOpenFile) {
  ret_status(ctx, open_file(ctx, mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), kFileOpen, arg(ctx, 5),
                     "NtOpenFile"));
}

// NtReadFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext, PIO_STATUS_BLOCK, PVOID Buffer,
//            ULONG Length, PLARGE_INTEGER ByteOffset)
XBOX_IMPORT(NtReadFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  if (f->dir) return ret_status(ctx, kStatusInvalidParameter);
  queue_io_apc(ctx);
  uint32_t buf = arg(ctx, 5), len = arg(ctx, 6);
  std::lock_guard<std::mutex> lock(f->mu);
  uint64_t off = f->position;
  explicit_offset(mem_base, arg(ctx, 7), &off);
  OVERLAPPED ov{};
  ov.Offset = (DWORD)off;
  ov.OffsetHigh = (DWORD)(off >> 32);
  DWORD got = 0;
  BOOL ok = TRUE;
  if (f->iso) {
    uint32_t n = 0;
    ok = iso::read(*f->iso, off, PPC_HOST(mem_base, buf), len, &n);
    got = n;
  } else if (len) {
    ok = ReadFile(f->h, PPC_HOST(mem_base, buf), len, &got, &ov);
  }
  if (!ok && GetLastError() != ERROR_HANDLE_EOF) {
    std::fprintf(stderr, "[file] ReadFile erro %lu: %s\n", GetLastError(), f->guest_path.c_str());
    complete(mem_base, arg(ctx, 1), arg(ctx, 4), kStatusAccessDenied, 0);
    return ret_status(ctx, kStatusAccessDenied);
  }
  f->position = off + got;
  uint32_t st = (len && !got) ? kStatusEndOfFile : kStatusSuccess;
  if (g_trace)
    std::fprintf(stderr, "[file] read %s @0x%llX len 0x%X -> 0x%lX\n", f->guest_path.c_str(),
                 (unsigned long long)off, len, got);
  complete(mem_base, arg(ctx, 1), arg(ctx, 4), st, got);
  ret_status(ctx, st);
}

// NtWriteFile: mesma assinatura que NtReadFile.
XBOX_IMPORT(NtWriteFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  if (f->dir) return ret_status(ctx, kStatusInvalidParameter);
  if (!f->writable) {
    complete(mem_base, arg(ctx, 1), arg(ctx, 4), kStatusMediaWriteProtected, 0);
    return ret_status(ctx, kStatusMediaWriteProtected);
  }
  queue_io_apc(ctx);
  uint32_t buf = arg(ctx, 5), len = arg(ctx, 6);
  std::lock_guard<std::mutex> lock(f->mu);
  uint64_t off = f->position;
  explicit_offset(mem_base, arg(ctx, 7), &off);
  OVERLAPPED ov{};
  ov.Offset = (DWORD)off;
  ov.OffsetHigh = (DWORD)(off >> 32);
  DWORD put = 0;
  if (len && !WriteFile(f->h, PPC_HOST(mem_base, buf), len, &put, &ov)) {
    std::fprintf(stderr, "[file] WriteFile erro %lu: %s\n", GetLastError(), f->guest_path.c_str());
    complete(mem_base, arg(ctx, 1), arg(ctx, 4), kStatusAccessDenied, 0);
    return ret_status(ctx, kStatusAccessDenied);
  }
  f->position = off + put;
  complete(mem_base, arg(ctx, 1), arg(ctx, 4), kStatusSuccess, put);
  ret_status(ctx, kStatusSuccess);
}

XBOX_IMPORT(NtFlushBuffersFile) {  // (HANDLE, PIO_STATUS_BLOCK)
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  if (f->h != INVALID_HANDLE_VALUE && f->writable) FlushFileBuffers(f->h);
  write_iosb(mem_base, arg(ctx, 1), kStatusSuccess, 0);
  ret_status(ctx, kStatusSuccess);
}

// NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info, ULONG Length, FILE_INFORMATION_CLASS)
XBOX_IMPORT(NtQueryInformationFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  uint32_t out = arg(ctx, 2), len = arg(ctx, 3), cls = arg(ctx, 4), iosb = arg(ctx, 1);
  std::lock_guard<std::mutex> lock(f->mu);
  Stat s = stat_any(f->host, f->iso);
  uint64_t size = f->dir ? 0 : file_size(*f);
  uint64_t alloc = (size + 0xFFF) & ~0xFFFull;
  uint32_t need = 0;
  auto check = [&](uint32_t n) {
    need = n;
    return len >= n;
  };
  switch (cls) {
    case 4:  // FileBasicInformation (0x28): 4× tempos, Attributes
      if (!check(0x28)) break;
      for (int i = 0; i < 4; i++) ppc_st64(mem_base, out + 8 * i, s.time);
      ppc_st32(mem_base, out + 0x20, attributes(s, f->writable));
      ppc_st32(mem_base, out + 0x24, 0);
      break;
    case 5:  // FileStandardInformation (0x18): AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory
      if (!check(0x18)) break;
      ppc_st64(mem_base, out + 0, alloc);
      ppc_st64(mem_base, out + 8, size);
      ppc_st32(mem_base, out + 0x10, 1);
      ppc_st8(mem_base, out + 0x14, f->delete_on_close);
      ppc_st8(mem_base, out + 0x15, f->dir);
      ppc_st16(mem_base, out + 0x16, 0);
      break;
    case 6:  // FileInternalInformation (8): IndexNumber
      if (!check(8)) break;
      ppc_st64(mem_base, out, std::hash<std::string>{}(f->host.string()));
      break;
    case 14:  // FilePositionInformation (8)
      if (!check(8)) break;
      ppc_st64(mem_base, out, f->position);
      break;
    case 20:  // FileEndOfFileInformation (8)
      if (!check(8)) break;
      ppc_st64(mem_base, out, size);
      break;
    case 34:  // FileNetworkOpenInformation (0x38): 4× tempos, AllocationSize, EndOfFile, Attributes
      if (!check(0x38)) break;
      for (int i = 0; i < 4; i++) ppc_st64(mem_base, out + 8 * i, s.time);
      ppc_st64(mem_base, out + 0x20, alloc);
      ppc_st64(mem_base, out + 0x28, size);
      ppc_st32(mem_base, out + 0x30, attributes(s, f->writable));
      ppc_st32(mem_base, out + 0x34, 0);
      break;
    default:
      std::fprintf(stderr, "[file] NtQueryInformationFile: classe %u não suportada (%s)\n", cls,
                   f->guest_path.c_str());
      write_iosb(mem_base, iosb, kStatusInvalidInfoClass, 0);
      return ret_status(ctx, kStatusInvalidInfoClass);
  }
  if (len < need) {
    write_iosb(mem_base, iosb, kStatusInfoLengthMismatch, 0);
    return ret_status(ctx, kStatusInfoLengthMismatch);
  }
  write_iosb(mem_base, iosb, kStatusSuccess, need);
  ret_status(ctx, kStatusSuccess);
}

// NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info, ULONG Length, FILE_INFORMATION_CLASS)
XBOX_IMPORT(NtSetInformationFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  uint32_t in = arg(ctx, 2), cls = arg(ctx, 4), iosb = arg(ctx, 1);
  std::lock_guard<std::mutex> lock(f->mu);
  uint32_t st = kStatusSuccess;
  switch (cls) {
    case 4:  // FileBasicInformation: tempos/atributos — ignorado
      break;
    case 13:  // FileDispositionInformation: BOOLEAN DeleteFile
      if (!f->writable) st = kStatusMediaWriteProtected;
      else f->delete_on_close = ppc_ld8(mem_base, in) != 0;
      break;
    case 14:  // FilePositionInformation
      f->position = ppc_ld64(mem_base, in);
      break;
    case 19:  // FileAllocationInformation
    case 20: {  // FileEndOfFileInformation
      if (!f->writable || f->h == INVALID_HANDLE_VALUE) {
        st = kStatusMediaWriteProtected;
        break;
      }
      LARGE_INTEGER li;
      li.QuadPart = (LONGLONG)ppc_ld64(mem_base, in);
      if (cls == 19 && (uint64_t)li.QuadPart >= file_size(*f)) break;  // só encolhe
      if (!SetFilePointerEx(f->h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(f->h)) st = kStatusAccessDenied;
      break;
    }
    default:
      std::fprintf(stderr, "[file] NtSetInformationFile: classe %u não suportada (%s)\n", cls,
                   f->guest_path.c_str());
      st = kStatusInvalidInfoClass;
  }
  write_iosb(mem_base, iosb, st, 0);
  ret_status(ctx, st);
}

// NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID Info, ULONG Length, FS_INFORMATION_CLASS)
XBOX_IMPORT(NtQueryVolumeInformationFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  uint32_t out = arg(ctx, 2), len = arg(ctx, 3), cls = arg(ctx, 4), iosb = arg(ctx, 1);
  const char* fs_name = f->writable ? "FATX" : "GDFX";
  const char* label = f->writable ? "HDD" : "GEARS";
  uint32_t need = 0;
  // Volume fictício: 4 GiB, 512 bytes/setor, 32 setores por unidade (16 KiB); o DVD não tem espaço livre.
  constexpr uint64_t kTotalUnits = (4ull << 30) / 0x4000;
  switch (cls) {
    case 1: {  // FileFsVolumeInformation: CreationTime u64, Serial u32, LabelLength u32, SupportsObjects u8, Label[]
      uint32_t ll = (uint32_t)std::strlen(label) * 2;
      need = 0x12 + ll;
      if (len < need) break;
      ppc_st64(mem_base, out + 0, 0);
      ppc_st32(mem_base, out + 8, 0x47454152);
      ppc_st32(mem_base, out + 0xC, ll);
      ppc_st8(mem_base, out + 0x10, 0);
      ppc_st8(mem_base, out + 0x11, 0);
      put_wide(mem_base, out + 0x12, label);
      break;
    }
    case 3:  // FileFsSizeInformation (0x18)
    case 7:  // FileFsFullSizeInformation (0x20): Total, CallerAvailable, ActualAvailable, SectorsPerUnit, BytesPerSector
      need = cls == 3 ? 0x18 : 0x20;
      if (len < need) break;
      ppc_st64(mem_base, out + 0, kTotalUnits);
      ppc_st64(mem_base, out + 8, f->writable ? kTotalUnits / 2 : 0);
      if (cls == 7) ppc_st64(mem_base, out + 0x10, f->writable ? kTotalUnits / 2 : 0);
      ppc_st32(mem_base, out + need - 8, 32);
      ppc_st32(mem_base, out + need - 4, 512);
      break;
    case 5: {  // FileFsAttributeInformation: Attributes u32, MaxComponentNameLength s32, NameLength u32, Name[]
      uint32_t nl = (uint32_t)std::strlen(fs_name) * 2;
      need = 0xC + nl;
      if (len < need) break;
      ppc_st32(mem_base, out + 0, 0);
      ppc_st32(mem_base, out + 4, f->writable ? 42 : 255);
      ppc_st32(mem_base, out + 8, nl);
      put_wide(mem_base, out + 0xC, fs_name);
      break;
    }
    default:
      std::fprintf(stderr, "[file] NtQueryVolumeInformationFile: classe %u não suportada\n", cls);
      write_iosb(mem_base, iosb, kStatusInvalidInfoClass, 0);
      return ret_status(ctx, kStatusInvalidInfoClass);
  }
  if (len < need) {
    write_iosb(mem_base, iosb, kStatusBufferTooSmall, 0);
    return ret_status(ctx, kStatusBufferTooSmall);
  }
  write_iosb(mem_base, iosb, kStatusSuccess, need);
  ret_status(ctx, kStatusSuccess);
}

// NtQueryDirectoryFile(HANDLE, HANDLE Event, PIO_APC_ROUTINE, PVOID ApcContext, PIO_STATUS_BLOCK,
//                      PVOID FileInformation, ULONG Length, PANSI_STRING FileMask, BOOLEAN RestartScan [pilha])
// No 360 só existe FILE_DIRECTORY_INFORMATION com nome ANSI:
//   +0 NextEntryOffset +4 FileIndex +8 Creation +0x10 LastAccess +0x18 LastWrite +0x20 Change
//   +0x28 EndOfFile +0x30 AllocationSize +0x38 Attributes +0x3C FileNameLength +0x40 FileName[]
// Devolve uma entrada por chamada (sempre válido no NT; o FindNextFile do XAPI pede uma de cada vez).
XBOX_IMPORT(NtQueryDirectoryFile) {
  auto f = file_of(arg(ctx, 0));
  if (!f) return ret_status(ctx, kStatusInvalidHandle);
  if (!f->dir) return ret_status(ctx, kStatusInvalidParameter);
  uint32_t event = arg(ctx, 1), iosb = arg(ctx, 4), out = arg(ctx, 5), len = arg(ctx, 6), mask_ptr = arg(ctx, 7);
  bool restart = (stack_arg(ctx, mem_base, 8) & 0xFF) != 0;
  queue_io_apc(ctx);
  std::lock_guard<std::mutex> lock(f->mu);
  bool first = !f->enum_started || restart;
  if (first) {
    f->enum_started = true;
    f->enum_index = 0;
    f->entries.clear();
    f->mask = "*";
    if (mask_ptr && ppc_ld16(mem_base, mask_ptr)) {
      uint32_t b = ppc_ld32(mem_base, mask_ptr + 4);
      f->mask.assign(reinterpret_cast<const char*>(PPC_HOST(mem_base, b)), ppc_ld16(mem_base, mask_ptr));
    }
    std::error_code ec;
    for (auto& e : fs::directory_iterator(f->host, ec)) {
      DirEntry d;
      d.name = e.path().filename().string();
      if (!wildcard_match(f->mask.c_str(), d.name.c_str())) continue;
      Stat s = stat_path(e.path());
      d.dir = s.dir;
      d.size = s.size;
      d.time = s.time;
      f->entries.push_back(std::move(d));
    }
    if (f->iso && f->iso->dir) {  // o que está solto substitui a mesma entrada da imagem de disco
      const size_t loose = f->entries.size();
      for (const iso::Node& c : f->iso->children) {
        if (!wildcard_match(f->mask.c_str(), c.name.c_str())) continue;
        const bool hidden = std::any_of(f->entries.begin(), f->entries.begin() + loose, [&](const DirEntry& e) {
          return _stricmp(e.name.c_str(), c.name.c_str()) == 0;
        });
        if (hidden) continue;
        DirEntry d;
        d.name = c.name;
        d.dir = c.dir;
        d.size = c.dir ? 0 : c.size;
        d.time = iso::volume_time();
        f->entries.push_back(std::move(d));
      }
    }
    if (g_trace)
      std::fprintf(stderr, "[file] dir %s mask '%s': %zu entradas\n", f->guest_path.c_str(), f->mask.c_str(),
                   f->entries.size());
  }
  if (f->enum_index >= f->entries.size()) {
    uint32_t st = first ? kStatusNoSuchFile : kStatusNoMoreFiles;
    complete(mem_base, event, iosb, st, 0);
    return ret_status(ctx, st);
  }
  const DirEntry& d = f->entries[f->enum_index];
  uint32_t need = 0x40 + (uint32_t)d.name.size();
  if (len < need) {
    complete(mem_base, event, iosb, kStatusBufferTooSmall, 0);
    return ret_status(ctx, kStatusBufferTooSmall);
  }
  f->enum_index++;
  ppc_st32(mem_base, out + 0, 0);
  ppc_st32(mem_base, out + 4, (uint32_t)f->enum_index);
  for (int i = 0; i < 4; i++) ppc_st64(mem_base, out + 8 + 8 * i, d.time);
  ppc_st64(mem_base, out + 0x28, d.size);
  ppc_st64(mem_base, out + 0x30, (d.size + 0xFFF) & ~0xFFFull);
  ppc_st32(mem_base, out + 0x38, d.dir ? kFileAttributeDirectory
                                       : (f->writable ? kFileAttributeNormal : kFileAttributeReadOnly));
  ppc_st32(mem_base, out + 0x3C, (uint32_t)d.name.size());
  std::memcpy(PPC_HOST(mem_base, out + 0x40), d.name.data(), d.name.size());
  complete(mem_base, event, iosb, kStatusSuccess, need);
  ret_status(ctx, kStatusSuccess);
}

// NtQueryFullAttributesFile(POBJECT_ATTRIBUTES, PFILE_NETWORK_OPEN_INFORMATION)
XBOX_IMPORT(NtQueryFullAttributesFile) {
  fs::path root_host;
  bool root_writable = false;
  std::string name = full_guest_path(mem_base, arg(ctx, 0), &root_host, &root_writable);
  auto r = vfs::resolve(name);
  Stat s = r ? stat_any(r->host, r->iso) : Stat{};
  if (!s.exists) {
    std::fprintf(stderr, "[vfs] não encontrado: %s\n", name.c_str());
    return ret_status(ctx, r ? kStatusObjectNameNotFound : kStatusObjectPathNotFound);
  }
  uint32_t out = arg(ctx, 1);
  for (int i = 0; i < 4; i++) ppc_st64(mem_base, out + 8 * i, s.time);
  ppc_st64(mem_base, out + 0x20, (s.size + 0xFFF) & ~0xFFFull);
  ppc_st64(mem_base, out + 0x28, s.size);
  ppc_st32(mem_base, out + 0x30, attributes(s, r->writable));
  ppc_st32(mem_base, out + 0x34, 0);
  ret_status(ctx, kStatusSuccess);
}
