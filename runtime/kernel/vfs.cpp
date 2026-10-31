#include "kernel/vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>

#include "ppc_context.h"

namespace vfs {
namespace {

std::mutex g_mounts_mu;
std::map<std::string, std::filesystem::path> g_mounts;  // dispositivo (minúsculas, sem ':') -> pasta do host

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

// "a\b\..\c" -> "a/c", sem raiz e sem escapar acima do dispositivo.
std::filesystem::path relative_part(std::string rest) {
  std::replace(rest.begin(), rest.end(), '\\', '/');
  std::filesystem::path out;
  size_t i = 0;
  while (i <= rest.size()) {
    size_t j = rest.find('/', i);
    if (j == std::string::npos) j = rest.size();
    std::string part = rest.substr(i, j - i);
    if (part == "..") {
      if (out.has_relative_path()) out = out.parent_path();
    } else if (!part.empty() && part != ".") {
      out /= part;
    }
    i = j + 1;
  }
  return out;
}

}  // namespace

std::filesystem::path user_root() {
  static const std::filesystem::path root = [] {
    const char* e = std::getenv("GOW_USER_DIR");
    return std::filesystem::path(e && *e ? e : "user_data");
  }();
  return root;
}

void mount(const std::string& device, const std::filesystem::path& host) {
  std::lock_guard<std::mutex> lock(g_mounts_mu);
  g_mounts[lower(device)] = host;
}

bool unmount(const std::string& device) {
  std::lock_guard<std::mutex> lock(g_mounts_mu);
  return g_mounts.erase(lower(device)) != 0;
}

std::filesystem::path game_root() {
  static const std::filesystem::path root = [] {
    const char* e = std::getenv("GOW_GAME_DIR");
    return std::filesystem::path(e && *e ? e : "extracted");
  }();
  return root;
}

std::optional<Resolved> resolve(const std::string& guest_path) {
  static const char* const kGameDevices[] = {"game:", "d:", "\\device\\cdrom0", "\\??\\game:", "\\??\\d:"};
  std::string l = lower(guest_path);
  for (const char* dev : kGameDevices) {
    size_t n = std::char_traits<char>::length(dev);
    if (l.compare(0, n, dev) != 0 || (l.size() > n && l[n] != '\\' && l[n] != '/')) continue;
    const std::filesystem::path rel = relative_part(guest_path.substr(n));
    Resolved out{game_root() / rel, false};
    std::error_code ec;
    const auto st = std::filesystem::status(out.host, ec);
    const bool loose = !ec && std::filesystem::exists(st);
    if (!loose || std::filesystem::is_directory(st)) out.iso = iso::find(rel.generic_string());
    if (!loose) out.iso_parent = iso::find(rel.parent_path().generic_string()) != nullptr;
    return out;
  }
  std::string p = l.compare(0, 4, "\\??\\") == 0 ? guest_path.substr(4) : guest_path;
  size_t colon = p.find(':');
  if (colon != std::string::npos && colon > 0) {
    std::lock_guard<std::mutex> lock(g_mounts_mu);
    auto it = g_mounts.find(lower(p.substr(0, colon)));
    if (it != g_mounts.end()) return Resolved{it->second / relative_part(p.substr(colon + 1)), true};
  }
  if (colon != std::string::npos && colon > 0 && p.find_first_of("\\/") > colon)
    return Resolved{user_root() / lower(p.substr(0, colon)) / relative_part(p.substr(colon + 1)), true};
  if (lower(p).compare(0, 8, "\\device\\") == 0) return Resolved{user_root() / relative_part(lower(p)), true};
  return std::nullopt;
}

bool read_game_file(const std::string& rel, std::vector<uint8_t>* out, size_t max) {
  if (std::ifstream in{game_root() / rel, std::ios::binary | std::ios::ate}) {
    const size_t size = std::min<size_t>((size_t)in.tellg(), max);
    out->resize(size);
    in.seekg(0);
    return (bool)in.read(reinterpret_cast<char*>(out->data()), (std::streamsize)size);
  }
  const iso::Node* n = iso::find(rel);
  if (!n || n->dir) return false;
  out->resize((size_t)std::min<uint64_t>(n->size, max));
  uint32_t got = 0;
  return iso::read(*n, 0, out->data(), (uint32_t)out->size(), &got) && got == out->size();
}

std::string object_name(uint8_t* m, uint32_t object_attributes) {
  if (!object_attributes) return {};
  uint32_t name = ppc_ld32(m, object_attributes + 4);
  if (!name) return {};
  uint16_t len = ppc_ld16(m, name);
  uint32_t buf = ppc_ld32(m, name + 4);
  return buf ? std::string(reinterpret_cast<const char*>(PPC_HOST(m, buf)), len) : std::string();
}

}  // namespace vfs
