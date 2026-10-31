// xam_user.cpp — utilizadores e perfil: sessão local do utilizador 0, XUID offline, definições de perfil.
//
// As definições escritas pelo jogo (XamUserWriteProfileSettings, sobretudo os blocos binários TITLE_SPECIFIC
// 1–3 onde o UE3 guarda as opções) ficam em user_data/profile/settings.bin e voltam em ReadProfileSettings.
//
// ID de uma definição: bits 28–31 = tipo (X_USER_DATA_TYPE), bits 16–27 = tamanho máximo, 0–15 = número.
#include <filesystem>
#include <fstream>
#include <random>

#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "kernel/kernel.h"
#include "kernel/vfs.h"
#include "xam/xam.h"

using namespace kernel;

namespace xam {

namespace {
// XUID offline (prefixo 0xE...), aleatório na 1ª execução e guardado: em rede, dois jogadores têm de ter XUIDs diferentes.
uint64_t offline_xuid() {
  static const uint64_t id = [] {
    const auto path = vfs::user_root() / "profile" / "xuid.bin";
    uint64_t v = 0;
    if (std::ifstream in{path, std::ios::binary}) in.read(reinterpret_cast<char*>(&v), sizeof v);
    if (v >> 60 != 0xE) {
      std::random_device rd;
      v = 0xE000000000000000ull | ((uint64_t)rd() & 0x0FFFFFFF) << 32 | rd();
      std::filesystem::create_directories(path.parent_path());
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(&v), sizeof v);
    }
    return v;
  }();
  return id;
}
}

bool signed_in(uint32_t user_index) { return user_index == 0 || user_index == 0xFF; }
uint64_t xuid(uint32_t user_index) { return signed_in(user_index) ? offline_xuid() : 0; }
const std::string& gamertag() {
  static const std::string tag = [] {
    const char* e = std::getenv("GOW_GAMERTAG");
    if (e && *e) return std::string(e);
    const std::string& n = config::get().player_name;
    return n.empty() ? std::string("Player") : n;
  }();
  return tag;
}

std::string read_utf16(uint8_t* m, uint32_t p, size_t max_chars) {
  std::string out;
  if (!p) return out;
  for (size_t i = 0; i < max_chars; i++) {
    uint32_t c = ppc_ld16(m, p + 2 * (uint32_t)i);
    if (!c) break;
    if (c >= 0xD800 && c < 0xDC00) {  // par substituto
      uint32_t lo = ppc_ld16(m, p + 2 * (uint32_t)++i);
      c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
    }
    if (c < 0x80) {
      out += (char)c;
    } else if (c < 0x800) {
      out += (char)(0xC0 | c >> 6), out += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += (char)(0xE0 | c >> 12), out += (char)(0x80 | (c >> 6 & 0x3F)), out += (char)(0x80 | (c & 0x3F));
    } else {
      out += (char)(0xF0 | c >> 18), out += (char)(0x80 | (c >> 12 & 0x3F));
      out += (char)(0x80 | (c >> 6 & 0x3F)), out += (char)(0x80 | (c & 0x3F));
    }
  }
  return out;
}

void write_utf16(uint8_t* m, uint32_t p, const std::string& s, size_t max_chars) {
  if (!p || !max_chars) return;
  size_t n = 0;
  for (size_t i = 0; i < s.size() && n + 1 < max_chars;) {
    unsigned char b = (unsigned char)s[i];
    uint32_t c = b, extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
    if (extra) c = b & (0x3F >> extra);
    for (uint32_t k = 1; k <= extra && i + k < s.size(); k++) c = c << 6 | ((unsigned char)s[i + k] & 0x3F);
    i += extra + 1;
    if (c >= 0x10000) {
      if (n + 2 >= max_chars) break;
      c -= 0x10000;
      ppc_st16(m, p + 2 * (uint32_t)n++, (uint16_t)(0xD800 + (c >> 10)));
      c = 0xDC00 + (c & 0x3FF);
    }
    ppc_st16(m, p + 2 * (uint32_t)n++, (uint16_t)c);
  }
  ppc_st16(m, p + 2 * (uint32_t)n, 0);
}

}  // namespace xam

using namespace xam;

// ---- Sessão / identidade ----

// XamUserGetSigninState(DWORD UserIndex) -> 0 sem sessão, 1 local, 2 Live
XBOX_IMPORT(XamUserGetSigninState) { ret(ctx, signed_in(arg(ctx, 0)) ? 1 : 0); }

// XamUserGetXUID(DWORD UserIndex, DWORD TypeMask, PXUID Xuid): 1 = offline, 2 = online, 4 = convidado
XBOX_IMPORT(XamUserGetXUID) {
  uint32_t user = arg(ctx, 0), mask = arg(ctx, 1), out = arg(ctx, 2);
  if (out) ppc_st64(mem_base, out, 0);
  if (user >= kUserCount && user != 0xFF) return ret(ctx, kErrorInvalidParameter);
  if (!signed_in(user) || !(mask & 1)) return ret(ctx, kErrorNoSuchUser);
  if (out) ppc_st64(mem_base, out, xuid(user));
  ret(ctx, kErrorSuccess);
}

// XamUserGetName(DWORD UserIndex, PSTR Buffer, DWORD Length)
XBOX_IMPORT(XamUserGetName) {
  uint32_t user = arg(ctx, 0), buf = arg(ctx, 1), len = arg(ctx, 2);
  if (!signed_in(user)) return ret(ctx, kErrorNoSuchUser);
  if (!buf || !len) return ret(ctx, kErrorInvalidParameter);
  const std::string& tag = gamertag();
  uint32_t n = std::min<uint32_t>((uint32_t)tag.size(), len - 1);
  for (uint32_t i = 0; i < n; i++) ppc_st8(mem_base, buf + i, (uint8_t)tag[i]);
  ppc_st8(mem_base, buf + n, 0);
  ret(ctx, kErrorSuccess);
}

// XamUserCheckPrivilege(DWORD UserIndex, DWORD PrivilegeType, PBOOL Result): sem restrições parentais.
XBOX_IMPORT(XamUserCheckPrivilege) {
  if (!signed_in(arg(ctx, 0))) return ret(ctx, kErrorNoSuchUser);
  out32(mem_base, arg(ctx, 2), 1);
  ret(ctx, kErrorSuccess);
}

// XamUserAreUsersFriends(DWORD UserIndex, PXUID Xuids, DWORD Count, PBOOL Result, PXOVERLAPPED): sem amigos.
XBOX_IMPORT(XamUserAreUsersFriends) {
  out32(mem_base, arg(ctx, 3), 0);
  ret(ctx, finish(mem_base, arg(ctx, 4), kErrorSuccess));
}

// ---- Definições de perfil ----
namespace {

enum DataType : uint8_t { kContext = 0, kInt32 = 1, kInt64 = 2, kDouble = 3, kUnicode = 4, kFloat = 5,
                          kBinary = 6, kDateTime = 7 };

uint8_t setting_type(uint32_t id) { return (uint8_t)(id >> 28 & 0xF); }
bool is_variable(uint8_t t) { return t == kUnicode || t == kBinary; }
bool is_title_specific(uint32_t id) { return id == 0x63E83FFF || id == 0x63E83FFE || id == 0x63E83FFD; }

// Valor guardado: escalares = 8 bytes do X_USER_DATA (BE, como na memória convidada); variáveis = conteúdo.
struct Store {
  std::mutex mu;
  std::map<uint32_t, std::vector<uint8_t>> values;
  bool loaded = false;

  std::filesystem::path file() { return vfs::user_root() / "profile" / "settings.bin"; }

  void load() {
    if (loaded) return;
    loaded = true;
    std::ifstream in(file(), std::ios::binary);
    uint32_t hdr[2];
    while (in.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) {
      std::vector<uint8_t> v(hdr[1]);
      if (!in.read(reinterpret_cast<char*>(v.data()), v.size())) break;
      values[hdr[0]] = std::move(v);
    }
    // Predefinições do dashboard (as mesmas do Xenia) para o que ainda não foi escrito.
    auto i32 = [&](uint32_t id, uint32_t v) {
      if (!values.count(id)) values[id] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v,
                                           0, 0, 0, 0};
    };
    i32(0x10040002, 0);            // GAMER_YAXIS_INVERSION
    i32(0x10040003, 3);            // OPTION_CONTROLLER_VIBRATION
    i32(0x10040015, 0);            // GAMER_DIFFICULTY
    i32(0x10040018, 0);            // GAMER_CONTROL_SENSITIVITY
    i32(0x1004001D, 0xFFFF0000u);  // GAMER_PREFERRED_COLOR_FIRST
    i32(0x1004001E, 0xFF00FF00u);  // GAMER_PREFERRED_COLOR_SECOND
    i32(0x10040022, 1);            // GAMER_ACTION_AUTO_AIM
    i32(0x10040023, 0);            // GAMER_ACTION_AUTO_CENTER
    i32(0x10040024, 1);            // GAMER_ACTION_MOVEMENT_CONTROL
    i32(0x10040038, 1);            // GAMERCARD_TITLES_PLAYED
  }

  void save() {
    std::error_code ec;
    std::filesystem::create_directories(file().parent_path(), ec);
    std::ofstream out(file(), std::ios::binary | std::ios::trunc);
    for (auto& [id, v] : values) {
      uint32_t hdr[2] = {id, (uint32_t)v.size()};
      out.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
      out.write(reinterpret_cast<const char*>(v.data()), v.size());
    }
  }
};

Store& store() {
  static Store s;
  return s;
}

}  // namespace

// XamUserReadProfileSettings(DWORD TitleId, DWORD UserIndex, DWORD XuidCount, PXUID Xuids, DWORD SettingCount,
//                            PDWORD SettingIds, PDWORD BufferSize, PVOID Buffer, PXOVERLAPPED [pilha])
// Buffer: {DWORD Count; X_USER_PROFILE_SETTING* Settings} + Count × 0x28 + dados variáveis.
// X_USER_PROFILE_SETTING: +0 from (0 nenhum, 1 perfil, 2 título), +8 user_index|xuid, +0x10 id,
//                         +0x18 X_USER_DATA {u8 type; ...; +8 valor | {u32 size; u32 ptr}}.
XBOX_IMPORT(XamUserReadProfileSettings) {
  uint32_t title = arg(ctx, 0), user = arg(ctx, 1), xuid_count = arg(ctx, 2), xuids = arg(ctx, 3);
  uint32_t count = arg(ctx, 4), ids = arg(ctx, 5), size_ptr = arg(ctx, 6), buf = arg(ctx, 7);
  uint32_t ov = stack_arg(ctx, mem_base, 8);
  if (!size_ptr || (count && !ids)) return ret(ctx, kErrorInvalidParameter);
  Store& s = store();
  std::lock_guard<std::mutex> lock(s.mu);
  s.load();
  uint32_t needed = 8 + count * 0x28;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t id = ppc_ld32(mem_base, ids + 4 * i);
    auto it = s.values.find(id);
    if (it != s.values.end() && is_variable(setting_type(id))) needed += ((uint32_t)it->second.size() + 3) & ~3u;
  }
  uint32_t have = ppc_ld32(mem_base, size_ptr);
  static int logged = 0;
  if (logged < 6) {
    logged++;
    std::fprintf(stderr,
                 "[xam] XamUserReadProfileSettings(título 0x%08X, utilizador %u, xuids %u@0x%08X, %u definições "
                 "@0x%08X, tamanho %u/%u, buffer 0x%08X, overlapped 0x%08X)\n",
                 title, user, xuid_count, xuids, count, ids, have, needed, buf, ov);
    for (uint32_t i = 0; i < count && i < 8; i++)
      std::fprintf(stderr, "[xam]   definição 0x%08X\n", ppc_ld32(mem_base, ids + 4 * i));
  }
  if (!buf || have < needed) {
    ppc_st32(mem_base, size_ptr, needed);
    return ret(ctx, finish(mem_base, ov, kErrorInsufficientBuffer));
  }
  ppc_st32(mem_base, buf, count);
  ppc_st32(mem_base, buf + 4, buf + 8);
  uint32_t data = buf + 8 + count * 0x28;
  for (uint32_t i = 0; i < count; i++) {
    uint32_t e = buf + 8 + i * 0x28, id = ppc_ld32(mem_base, ids + 4 * i);
    uint8_t type = setting_type(id);
    for (uint32_t o = 0; o < 0x28; o += 4) ppc_st32(mem_base, e + o, 0);
    if (xuid_count && xuids)
      ppc_st64(mem_base, e + 8, ppc_ld64(mem_base, xuids));
    else
      ppc_st32(mem_base, e + 8, user);
    ppc_st32(mem_base, e + 0x10, id);
    ppc_st8(mem_base, e + 0x18, type);
    auto it = s.values.find(id);
    if (it == s.values.end()) continue;  // from = 0: o jogo usa a predefinição dele
    const std::vector<uint8_t>& v = it->second;
    ppc_st32(mem_base, e, is_title_specific(id) ? 2 : 1);
    if (is_variable(type)) {
      for (size_t k = 0; k < v.size(); k++) ppc_st8(mem_base, data + (uint32_t)k, v[k]);
      ppc_st32(mem_base, e + 0x20, (uint32_t)v.size());
      ppc_st32(mem_base, e + 0x24, data);
      data += ((uint32_t)v.size() + 3) & ~3u;
    } else {
      for (size_t k = 0; k < 8 && k < v.size(); k++) ppc_st8(mem_base, e + 0x20 + (uint32_t)k, v[k]);
    }
  }
  ret(ctx, finish(mem_base, ov, kErrorSuccess));
}

// XamUserWriteProfileSettings(DWORD TitleId, DWORD UserIndex, DWORD SettingCount,
//                             X_USER_PROFILE_SETTING* Settings, PXOVERLAPPED)
XBOX_IMPORT(XamUserWriteProfileSettings) {
  uint32_t count = arg(ctx, 2), settings = arg(ctx, 3), ov = arg(ctx, 4);
  Store& s = store();
  {
    std::lock_guard<std::mutex> lock(s.mu);
    s.load();
    for (uint32_t i = 0; i < count; i++) {
      uint32_t e = settings + i * 0x28, id = ppc_ld32(mem_base, e + 0x10);
      uint8_t type = ppc_ld8(mem_base, e + 0x18);
      std::vector<uint8_t> v;
      if (is_variable(type)) {
        uint32_t size = ppc_ld32(mem_base, e + 0x20), ptr = ppc_ld32(mem_base, e + 0x24);
        if (size > 0x10000 || (size && !ptr)) continue;
        for (uint32_t k = 0; k < size; k++) v.push_back(ppc_ld8(mem_base, ptr + k));
      } else {
        for (uint32_t k = 0; k < 8; k++) v.push_back(ppc_ld8(mem_base, e + 0x20 + k));
      }
      s.values[id] = std::move(v);
    }
    s.save();
  }
  std::fprintf(stderr, "[xam] XamUserWriteProfileSettings: %u definições gravadas\n", count);
  broadcast(kXnSysProfileSettingChanged, 1);
  ret(ctx, finish(mem_base, ov, kErrorSuccess));
}
