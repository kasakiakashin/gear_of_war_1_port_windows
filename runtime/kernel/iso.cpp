// iso.cpp — leitor XDVDFS (mesmo formato de tools/xdvdfs.py).
//
// A partição do jogo começa num deslocamento fixo que depende da geração do disco. No setor 32 da partição está o
// descritor {"MICROSOFT*XBOX*MEDIA", u32 setor da raiz, u32 tamanho da raiz, u64 FILETIME}, com a assinatura repetida em
// +0x7EC. Cada diretório é uma árvore binária em setores de 2048 bytes:
//   +0 u16 esquerda (unidades de 4 bytes) +2 u16 direita +4 u32 setor +8 u32 tamanho +0xC u8 atributos +0xD u8 nome +0xE nome
// Tudo em little-endian; setores relativos à partição.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "kernel/iso.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace iso {
namespace {

constexpr uint64_t kSector = 2048;
constexpr char kMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMagicSize = 20;
// XGD2 (retail), XGD3, XGD1, ISO pura e as variantes reconstruídas que o Xenia também aceita.
constexpr uint64_t kPartitions[] = {0x0FD90000, 0x02080000, 0x18300000, 0x00000000, 0x0000FB20, 0x00020600};
constexpr uint8_t kAttrDirectory = 0x10;

HANDLE g_file = INVALID_HANDLE_VALUE;
uint64_t g_base = 0, g_size = 0, g_time = 0;
Node g_root;

bool read_at(uint64_t off, void* dst, uint32_t len, uint32_t* got) {
  OVERLAPPED ov{};
  ov.Offset = (DWORD)off;
  ov.OffsetHigh = (DWORD)(off >> 32);
  // Sem FILE_FLAG_OVERLAPPED: síncrono, e o deslocamento vem do OVERLAPPED, por isso várias threads podem ler.
  DWORD n = 0;
  BOOL ok = ReadFile(g_file, dst, len, &n, &ov);
  *got = n;
  return ok || GetLastError() == ERROR_HANDLE_EOF;
}

bool read_exact(uint64_t off, void* dst, uint32_t len) {
  uint32_t got = 0;
  return read_at(off, dst, len, &got) && got == len;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

bool read_dir(Node& dir, int depth) {
  if (depth > 32) return false;  // imagem corrompida com um ciclo
  if (!dir.size) return true;
  std::vector<uint8_t> data((size_t)((dir.size + kSector - 1) / kSector * kSector));
  if (g_base + dir.sector * kSector + data.size() > g_size ||
      !read_exact(g_base + dir.sector * kSector, data.data(), (uint32_t)data.size()))
    return false;
  std::vector<uint32_t> stack{0}, seen;
  while (!stack.empty()) {
    uint32_t off = stack.back();
    stack.pop_back();
    if (off + 14 > data.size() || std::find(seen.begin(), seen.end(), off) != seen.end()) continue;
    seen.push_back(off);
    const uint8_t* e = &data[off];
    uint16_t left = le16(e), right = le16(e + 2);
    if (left == 0xFFFF && right == 0xFFFF) continue;  // enchimento / diretório vazio
    uint8_t name_len = e[0xD];
    if (name_len && off + 14 + name_len <= data.size()) {
      Node n;
      n.name.assign(reinterpret_cast<const char*>(e + 14), name_len);
      n.sector = le32(e + 4);
      n.size = le32(e + 8);
      n.dir = (e[0xC] & kAttrDirectory) != 0;
      if (n.dir && !read_dir(n, depth + 1)) return false;
      dir.children.push_back(std::move(n));
    }
    if (left) stack.push_back(left * 4u);
    if (right) stack.push_back(right * 4u);
  }
  std::sort(dir.children.begin(), dir.children.end(),
            [](const Node& a, const Node& b) { return lower(a.name) < lower(b.name); });
  return true;
}

void count_files(const Node& n, uint32_t* files, uint64_t* bytes) {
  for (const Node& c : n.children) {
    if (c.dir) {
      count_files(c, files, bytes);
    } else {
      ++*files;
      *bytes += c.size;
    }
  }
}

int verify_node(const Node& n, const std::filesystem::path& dir, uint32_t* files, uint64_t* bytes) {
  int bad = 0;
  std::vector<char> a(1 << 20), b(1 << 20);
  for (const Node& c : n.children) {
    const auto p = dir / c.name;
    if (c.dir) {
      bad += verify_node(c, p, files, bytes);
      continue;
    }
    std::ifstream in(p, std::ios::binary);
    bool same = (bool)in;
    for (uint64_t off = 0; same && off < c.size;) {
      uint32_t got = 0;
      if (!read(c, off, a.data(), (uint32_t)a.size(), &got) || !got) same = false;
      else if (!in.read(b.data(), got) || std::memcmp(a.data(), b.data(), got) != 0) same = false;
      off += got;
    }
    if (same && in.peek() != std::ifstream::traits_type::eof()) same = false;  // cópia solta maior do que a imagem
    if (!same) {
      std::fprintf(stderr, "[iso] DIFERENTE: %s\n", p.string().c_str());
      bad++;
    }
    ++*files;
    *bytes += c.size;
  }
  return bad;
}

}  // namespace

bool mounted() { return g_file != INVALID_HANDLE_VALUE; }

void unmount() {
  if (mounted()) CloseHandle(g_file);
  g_file = INVALID_HANDLE_VALUE;
  g_root = Node{};
}

bool mount(const std::filesystem::path& image, std::string* error) {
  unmount();
  auto fail = [&](const char* why) {
    if (error) *error = why;
    unmount();
    return false;
  };
  g_file = CreateFileW(image.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!mounted()) return fail("não foi possível abrir o ficheiro");
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(g_file, &size)) return fail("não foi possível ler o tamanho do ficheiro");
  g_size = (uint64_t)size.QuadPart;
  uint8_t vd[kSector];
  bool found = false;
  for (uint64_t base : kPartitions) {
    if (base + 33 * kSector > g_size || !read_exact(base + 32 * kSector, vd, sizeof vd)) continue;
    if (!std::memcmp(vd, kMagic, kMagicSize) && !std::memcmp(vd + 0x7EC, kMagic, kMagicSize)) {
      g_base = base;
      found = true;
      break;
    }
  }
  if (!found) return fail("não é uma imagem de disco Xbox (XDVDFS não encontrado)");
  g_root = Node{};
  g_root.dir = true;
  g_root.sector = le32(vd + 0x14);
  g_root.size = le32(vd + 0x18);
  g_time = (uint64_t)le32(vd + 0x1C) | (uint64_t)le32(vd + 0x20) << 32;
  if (!read_dir(g_root, 0)) return fail("diretórios da imagem corrompidos");
  uint32_t files = 0;
  uint64_t bytes = 0;
  count_files(g_root, &files, &bytes);
  std::fprintf(stderr, "[iso] %s: partição 0x%llX, %u ficheiros, %.2f GiB\n", image.string().c_str(),
               (unsigned long long)g_base, files, bytes / (1024.0 * 1024.0 * 1024.0));
  return true;
}

const Node* find(const std::string& rel) {
  if (!mounted()) return nullptr;
  const Node* n = &g_root;
  size_t i = 0;
  while (i < rel.size()) {
    size_t j = rel.find_first_of("/\\", i);
    if (j == std::string::npos) j = rel.size();
    if (j > i) {
      const std::string part = lower(rel.substr(i, j - i));
      auto it = std::lower_bound(n->children.begin(), n->children.end(), part,
                                 [](const Node& c, const std::string& v) { return lower(c.name) < v; });
      if (it == n->children.end() || lower(it->name) != part) return nullptr;
      n = &*it;
    }
    i = j + 1;
  }
  return n;
}

bool read(const Node& node, uint64_t offset, void* dst, uint32_t len, uint32_t* got) {
  *got = 0;
  if (offset >= node.size || !len) return true;
  const uint32_t n = (uint32_t)std::min<uint64_t>(len, node.size - offset);
  return read_at(g_base + node.sector * kSector + offset, dst, n, got);
}

bool read_all(const Node& node, std::vector<uint8_t>* out) {
  out->resize((size_t)node.size);
  uint32_t got = 0;
  return !node.size || (read(node, 0, out->data(), (uint32_t)out->size(), &got) && got == out->size());
}

uint64_t volume_time() { return g_time; }

int verify_against(const std::filesystem::path& dir) {
  uint32_t files = 0;
  uint64_t bytes = 0;
  int bad = verify_node(g_root, dir, &files, &bytes);
  std::fprintf(stderr, "[iso] verificação: %u ficheiros, %llu bytes, %d diferentes\n", files, (unsigned long long)bytes, bad);
  return bad;
}

}  // namespace iso
