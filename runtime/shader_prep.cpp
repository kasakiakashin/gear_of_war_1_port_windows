#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcommon.h>

#include "shader_prep.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "gpu/shader_compile.h"
#include "gpu/texture.h"
#include "gpu/xenos_shader.h"
#include "kernel/iso.h"
#include "kernel/vfs.h"

namespace fs = std::filesystem;

namespace shader_prep {
namespace {

constexpr uint32_t kPackageTag = 0x9E2A83C1u;
constexpr uint32_t kLzoFlag = 2;
constexpr char kPackMagic[8] = {'G', 'O', 'W', 'S', 'P', 'K', '0', '1'};

std::mutex g_mu;
Progress g_progress;
std::atomic<bool> g_running{false}, g_stop{false};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

void set(const std::function<void(Progress&)>& f) {
  std::lock_guard<std::mutex> lock(g_mu);
  f(g_progress);
}

// ---- LZO1X (algoritmo de referência lzo1x_decompress_safe) ----
bool lzo_decompress(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t out_len) {
  const size_t start = out.size();
  size_t ip = 0;
  auto ext = [&](uint32_t base, uint32_t& t) {
    while (ip < n && src[ip] == 0) t += 255, ip++;
    if (ip >= n) return false;
    t += base + src[ip++];
    return true;
  };
  auto lit = [&](size_t cnt) {
    if (ip + cnt > n) return false;
    out.insert(out.end(), src + ip, src + ip + cnt);
    ip += cnt;
    return true;
  };
  auto match = [&](size_t dist, size_t len) {
    if (dist == 0 || dist > out.size() - start) return false;
    size_t p = out.size() - dist;
    for (size_t i = 0; i < len; i++) out.push_back(out[p + i]);
    return true;
  };
  if (!n) return false;
  uint32_t state = 0;
  if (src[0] > 17) {
    const uint32_t t = src[0] - 17;
    ip = 1;
    if (!lit(t)) return false;
    state = t < 4 ? t : 4;
  }
  for (;;) {
    if (ip >= n || out.size() - start > out_len) return false;
    uint32_t t = src[ip++];
    if (t < 16) {
      if (state == 0) {
        uint32_t len = t;
        if (!len && !ext(15, len)) return false;
        if (!lit(len + 3)) return false;
        state = 4;
        continue;
      }
      if (ip >= n) return false;
      if (state < 4) {
        if (!match(1 + (t >> 2) + (src[ip++] << 2), 2)) return false;
      } else {
        if (!match(0x801 + (t >> 2) + (src[ip++] << 2), 3)) return false;
      }
      state = t & 3;
    } else if (t >= 64) {
      if (ip >= n || !match(1 + ((t >> 2) & 7) + (src[ip++] << 3), (t >> 5) + 1)) return false;
      state = t & 3;
    } else if (t >= 32) {
      uint32_t len = t & 31;
      if (!len && !ext(31, len)) return false;
      if (ip + 2 > n) return false;
      const uint32_t ds = src[ip] | src[ip + 1] << 8;
      ip += 2;
      if (!match(1 + (ds >> 2), len + 2)) return false;
      state = ds & 3;
    } else {
      uint32_t len = t & 7;
      if (!len && !ext(7, len)) return false;
      if (ip + 2 > n) return false;
      const uint32_t ds = src[ip] | src[ip + 1] << 8;
      ip += 2;
      const size_t dist = ((t & 8) << 11) + (ds >> 2);
      if (!dist) break;  // fim do fluxo
      if (!match(dist + 0x4000, len + 2)) return false;
      state = ds & 3;
    }
    if (state && !lit(state)) return false;
  }
  return out.size() - start == out_len;
}

// Bloco comprimido do UE3: {tag, tamanho do bloco | tag}, {comprimido, descomprimido}, um par por bloco, dados.
bool decompress_chunk(const uint8_t* p, size_t avail, bool little, std::vector<uint8_t>& out) {
  auto rd = [&](size_t o) { return little ? le32(p + o) : be32(p + o); };
  if (avail < 16 || rd(0) != kPackageTag) return false;
  const uint32_t t2 = rd(4), total = rd(12);
  const uint32_t bs = t2 == kPackageTag ? 0x20000 : t2;
  if (!bs || total > (256u << 20)) return false;
  const uint32_t blocks = (total + bs - 1) / bs;
  size_t o = 16, data = 16 + (size_t)blocks * 8;
  if (data > avail) return false;
  for (uint32_t b = 0; b < blocks; b++, o += 8) {
    const uint32_t c = rd(o), u = rd(o + 4);
    if (data + c > avail || !lzo_decompress(p + data, c, out, u)) return false;
    data += c;
  }
  return true;
}

// Conteúdo descomprimido de um pacote (o cabeçalho fica de fora: não tem shaders).
bool package_body(const std::vector<uint8_t>& f, std::vector<uint8_t>& out) {
  if (f.size() < 64) return false;
  if (le32(f.data()) == kPackageTag) return decompress_chunk(f.data(), f.size(), true, out);  // pacote todo comprimido
  if (be32(f.data()) != kPackageTag) return false;
  size_t o = 12;
  const int32_t folder = (int32_t)be32(&f[o]);
  o += 4 + (folder < 0 ? (size_t)(-folder) * 2 : (size_t)folder);
  o += 28 + 16;
  if (o + 4 > f.size()) return false;
  const uint32_t generations = be32(&f[o]);
  o += 4 + (size_t)generations * 12;
  if (o + 16 > f.size()) return false;
  const uint32_t flags = be32(&f[o + 8]), count = be32(&f[o + 12]);
  o += 16;
  if (!count) {
    out = f;  // sem compressão
    return true;
  }
  if (flags != kLzoFlag || o + (size_t)count * 16 > f.size()) return false;
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t co = be32(&f[o + 16 * i + 8]), cs = be32(&f[o + 16 * i + 12]);
    if ((size_t)co + cs > f.size() || !decompress_chunk(&f[co], cs, false, out)) return false;
  }
  return true;
}

struct Found {
  std::vector<uint32_t> ucode;  // ordem do host
  bool pixel;
};

// Contentores 0x102A110x: o código está em [+deslocamento, +tamanho); às vezes há 64 bytes de constantes antes do microcódigo.
void find_shaders(const std::vector<uint8_t>& d, std::vector<Found>& out) {
  for (size_t i = 0; i + 16 <= d.size(); i++) {
    const uint8_t* p = (const uint8_t*)std::memchr(&d[i], 0x10, d.size() - 16 - i + 1);
    if (!p) break;
    i = (size_t)(p - d.data());
    if (p[1] != 0x2A || p[2] != 0x11 || p[3] > 1) continue;
    const uint32_t off = be32(p + 4), size = be32(p + 8);
    if (off < 16 || size < 12 || size > 0x40000 || i + off + size > d.size()) continue;
    std::vector<uint32_t> region(size / 4);
    std::memcpy(region.data(), &d[i + off], region.size() * 4);
    // O jogo carrega o resto inteiro do contentor (o tamanho calculado fica até 12 bytes aquém): o 1º início que acaba aí é o
    // microcódigo, copiado até ao fim, para dar o mesmo hash e o mesmo HLSL que o desenho vai pedir.
    uint32_t best = 0, best_at = 0;
    for (uint32_t s = 0; s < region.size() && s < 64; s++) {  // até 256 bytes antes do microcódigo
      bool vertex;
      const uint32_t n = gpu::microcode_size(region.data() + s, region.size() - s, &vertex);
      if (!n || s + n > region.size() || region.size() - (s + n) > 6) continue;
      best = (uint32_t)region.size() - s, best_at = s;
      break;
    }
    if (!best) continue;
    Found f;
    f.pixel = p[3] == 0;
    f.ucode.resize(best);
    for (uint32_t k = 0; k < best; k++) f.ucode[k] = _byteswap_ulong(region[best_at + k]);
    out.push_back(std::move(f));
  }
}

std::vector<std::string> list_packages() {
  std::set<std::string> names;
  auto add = [&](const std::string& n) {
    std::string l = n;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    if (l.size() > 4 && l.compare(l.size() - 4, 4, ".xxx") == 0) names.insert(n);
  };
  if (const iso::Node* dir = iso::find("WarGame/CookedXenon"))
    for (const iso::Node& c : dir->children) add(c.name);
  std::error_code ec;
  for (auto& e : fs::directory_iterator(vfs::game_root() / "WarGame" / "CookedXenon", ec)) add(e.path().filename().string());
  if (const char* only = std::getenv("GOW_PREP_ONLY")) {  // diagnóstico: só os pacotes com um destes textos (a,b,…) no nome
    const std::string list = std::string(",") + only + ",";
    for (auto it = names.begin(); it != names.end();) {
      bool keep = false;
      for (size_t a = 0, b; (b = list.find(',', a + 1)) != std::string::npos; a = b)
        if (b > a + 1 && it->find(list.substr(a + 1, b - a - 1)) != std::string::npos) keep = true;
      it = keep ? std::next(it) : names.erase(it);
    }
  }
  return {names.begin(), names.end()};
}

fs::path exe_path() {
  wchar_t exe[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof exe / sizeof exe[0]));
  return fs::path(std::wstring(exe, n));
}

// Hash do conteúdo do gow.exe (não a data: o zip não a guarda com exatidão). Outro exe pode traduzir os shaders de outra forma.
std::string signature() {
  static const std::string sig = [] {
    std::ifstream in(exe_path(), std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    char b[40];
    std::snprintf(b, sizeof b, "%016llX-%zu", (unsigned long long)gpu::hash_bytes(data.data(), data.size()), data.size());
    return std::string(b);
  }();
  return sig;
}

fs::path marker() { return vfs::user_root() / "shader_cache" / "prepared.txt"; }

void write_marker() {
  std::error_code ec;
  fs::create_directories(marker().parent_path(), ec);
  std::ofstream(marker()) << signature();
}

// prebuilt_shaders.bin ao lado do exe: índice em memória; os DXBC são lidos do ficheiro quando um desenho os pede.
struct Pack {
  bool ok = false, same_exe = false;
  std::unordered_map<uint64_t, std::pair<uint64_t, uint32_t>> index;  // chave -> (deslocamento, tamanho)
  std::ifstream in;
  std::mutex mu;
};

Pack& pack() {
  static Pack p;
  static std::once_flag once;
  std::call_once(once, [] {
    std::ifstream in(exe_path().parent_path() / "prebuilt_shaders.bin", std::ios::binary);
    char magic[8] = {};
    uint32_t sl = 0, n = 0;
    if (!in.read(magic, 8) || std::memcmp(magic, kPackMagic, 8) || !in.read(reinterpret_cast<char*>(&sl), 4) || sl > 256) return;
    std::string sig(sl, '\0');
    in.read(sig.data(), sl);
    if (!in.read(reinterpret_cast<char*>(&n), 4) || n > 1000000) return;
    uint64_t off = 8 + 4 + (uint64_t)sl + 4 + (uint64_t)n * 12;
    p.index.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
      uint64_t key = 0;
      uint32_t size = 0;
      in.read(reinterpret_cast<char*>(&key), 8);
      in.read(reinterpret_cast<char*>(&size), 4);
      p.index[key] = {off, size};
      off += size;
    }
    if (!in) return;
    p.same_exe = sig == signature();
    p.in = std::move(in);
    p.ok = true;
    std::fprintf(stderr, "[shaders] prebuilt_shaders.bin: %u shaders%s\n", n, p.same_exe ? "" : " (feito com outro gow.exe)");
  });
  return p;
}

bool in_pack(uint64_t key) {
  Pack& p = pack();
  return p.ok && p.index.count(key);
}

struct Job {
  std::string hlsl, name, target;
  fs::path file;
};

void run() {
  const unsigned threads = std::max(2u, std::thread::hardware_concurrency());
  if (up_to_date()) {  // prebuilt_shaders.bin deste exe: nada a fazer
    set([](Progress& p) { p = Progress{}; p.stage = Progress::kDone; });
    g_running = false;
    return;
  }
  set([](Progress& p) { p = Progress{}; p.stage = Progress::kScanning; });
  if (!gpu::compiler_available()) {
    set([](Progress& p) { p.stage = Progress::kFailed; p.message = "d3dcompiler_47.dll não encontrado."; });
    g_running = false;
    return;
  }
  const std::vector<std::string> packages = list_packages();
  set([&](Progress& p) { p.packages_total = (uint32_t)packages.size(); });
  std::atomic<size_t> next{0};
  std::mutex found_mu;
  std::unordered_set<uint64_t> seen;
  std::vector<Job> jobs;
  std::atomic<uint32_t> bad{0};
  // GOW_PREP_FIND=pasta (diagnóstico): procura o microcódigo dos .bin de GOW_TRACE_DEMAND nos pacotes descomprimidos.
  struct Needle {
    std::string name, all, head, tail;
  };
  std::vector<Needle> needles;
  if (const char* dir = std::getenv("GOW_PREP_FIND")) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec)) {
      std::ifstream in(e.path(), std::ios::binary);
      std::string d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (d.size() >= 96) needles.push_back({e.path().filename().string(), d, d.substr(0, 48), d.substr(d.size() - 48)});
    }
  }
  auto scan_worker = [&] {
    std::vector<uint8_t> file, body;
    std::vector<Found> found;
    for (size_t i; !g_stop && (i = next.fetch_add(1)) < packages.size();) {
      file.clear(), body.clear(), found.clear();
      if (vfs::read_game_file("WarGame/CookedXenon/" + packages[i], &file, ~size_t(0)) && package_body(file, body))
        find_shaders(body, found);
      else
        bad++;
      if (!needles.empty() && !body.empty()) {
        const std::string_view b(reinterpret_cast<const char*>(body.data()), body.size());
        for (const Needle& n : needles) {
          const size_t a = b.find(n.all), h = b.find(n.head), t = b.find(n.tail);
          if (a != b.npos || h != b.npos || t != b.npos)
            std::fprintf(stderr, "[find] %s em %s: inteiro %zd, início %zd, fim %zd\n", n.name.c_str(), packages[i].c_str(),
                         a == b.npos ? (ptrdiff_t)-1 : (ptrdiff_t)a, h == b.npos ? (ptrdiff_t)-1 : (ptrdiff_t)h,
                         t == b.npos ? (ptrdiff_t)-1 : (ptrdiff_t)t);
          if (a == b.npos && h != b.npos && h + n.all.size() <= body.size()) {
            std::string diff;
            uint32_t ndiff = 0;
            for (size_t k = 0; k < n.all.size() / 4; k++)
              if (std::memcmp(&body[h + 4 * k], &n.all[4 * k], 4)) {
                if (++ndiff <= 40) {
                  char t[64];
                  std::snprintf(t, sizeof t, " [%zu] %08X->%08X", k, be32(&body[h + 4 * k]), be32((const uint8_t*)&n.all[4 * k]));
                  diff += t;
                }
              }
            std::fprintf(stderr, "[find]   %u dwords diferentes de %zu:%s\n", ndiff, n.all.size() / 4, diff.c_str());
            // O HLSL da versão do disco é igual ao da versão da memória? (senão o tradutor ainda lê um campo que o D3D muda)
            std::vector<uint32_t> ud(n.all.size() / 4), um(n.all.size() / 4);
            for (size_t k = 0; k < ud.size(); k++) ud[k] = be32(&body[h + 4 * k]), um[k] = be32((const uint8_t*)&n.all[4 * k]);
            const gpu::TranslatedShader td = gpu::translate_shader(ud, n.name[0] == 'p'), tm = gpu::translate_shader(um, n.name[0] == 'p');
            if (td.hlsl != tm.hlsl) {
              size_t at = 0;
              while (at < td.hlsl.size() && at < tm.hlsl.size() && td.hlsl[at] == tm.hlsl[at]) at++;
              const size_t ls = td.hlsl.rfind('\n', at) + 1;
              std::fprintf(stderr, "[find]   HLSL difere: disco «%s» / memória «%s»\n", td.hlsl.substr(ls, 120).c_str(),
                           tm.hlsl.substr(ls, 120).c_str());
            } else {
              std::fprintf(stderr, "[find]   HLSL igual (falha do parser do contentor)\n");
            }
          }
          if (a != b.npos)
            for (size_t c = a; c-- > (a > 4096 ? a - 4096 : 0);)
              if (body[c] == 0x10 && body[c + 1] == 0x2A && body[c + 2] == 0x11 && body[c + 3] <= 1) {
                bool v;
                std::fprintf(stderr, "[find]   contentor em %zu: tipo %u, desl %u, tam %u; shader a +%zu, %zu bytes; tamanho calculado %u\n",
                             c, body[c + 3], be32(&body[c + 4]), be32(&body[c + 8]), a - c, n.all.size(),
                             gpu::microcode_size(reinterpret_cast<const uint32_t*>(n.all.data()), n.all.size() / 4, &v) * 4);
                break;
              }
        }
      }
      for (Found& f : found) {
        const uint64_t h = gpu::hash_bytes(reinterpret_cast<const uint8_t*>(f.ucode.data()), f.ucode.size() * 4) ^ (f.pixel ? 1 : 2);
        {
          std::lock_guard<std::mutex> lock(found_mu);
          if (!seen.insert(h).second) continue;
        }
        gpu::TranslatedShader t = gpu::translate_shader(f.ucode, f.pixel);
        if (!t.ok) continue;
        fs::path file_out = gpu::shader_cache_file(t.hlsl);
        std::error_code ec;
        set([](Progress& p) { p.found++; });
        if (in_pack(gpu::shader_cache_key(t.hlsl)) || fs::exists(file_out, ec)) continue;
        char name[48];
        std::snprintf(name, sizeof name, "%s_%016llX", f.pixel ? "ps" : "vs", (unsigned long long)t.hash);
        std::lock_guard<std::mutex> lock(found_mu);
        jobs.push_back({std::move(t.hlsl), name, f.pixel ? "ps_5_1" : "vs_5_1", std::move(file_out)});
      }
      set([](Progress& p) { p.packages_done++; });
    }
  };
  {
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) pool.emplace_back(scan_worker);
    for (auto& t : pool) t.join();
  }
  if (bad) std::fprintf(stderr, "[shaders] %u pacotes não lidos (compressão desconhecida ou erro de leitura)\n", bad.load());
  // Vértices primeiro: são os mais lentos.
  std::stable_sort(jobs.begin(), jobs.end(), [](const Job& a, const Job& b) { return a.target[0] > b.target[0]; });
  set([&](Progress& p) { p.stage = Progress::kCompiling; p.to_compile = (uint32_t)jobs.size(); });
  next = 0;
  std::error_code ec;
  fs::create_directories(vfs::user_root() / "shader_cache", ec);
  auto compile_worker = [&] {
    for (size_t i; !g_stop && (i = next.fetch_add(1)) < jobs.size();) {
      std::string err;
      if (ID3D10Blob* blob = gpu::compile_hlsl(jobs[i].hlsl, jobs[i].target.c_str(), jobs[i].name.c_str(), &err)) {
        std::ofstream(jobs[i].file, std::ios::binary)
            .write(static_cast<const char*>(blob->GetBufferPointer()), (std::streamsize)blob->GetBufferSize());
        blob->Release();
      }
      set([](Progress& p) { p.compiled++; });
    }
  };
  {
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; t++) pool.emplace_back(compile_worker);
    for (auto& t : pool) t.join();
  }
  if (!needles.empty()) {
    uint32_t missing = 0;
    for (const Needle& n : needles) {
      std::vector<uint32_t> u(n.all.size() / 4);
      std::memcpy(u.data(), n.all.data(), u.size() * 4);
      for (uint32_t& w : u) w = _byteswap_ulong(w);
      const gpu::TranslatedShader t = gpu::translate_shader(u, n.name[0] == 'p');
      std::error_code fe;
      if (!t.ok || !fs::exists(gpu::shader_cache_file(t.hlsl), fe)) {
        missing++;
        std::fprintf(stderr, "[find] sem cobertura: %s (%zu dwords)\n", n.name.c_str(), u.size());
      }
    }
    std::fprintf(stderr, "[find] %u de %zu pedidos sem cobertura\n", missing, needles.size());
  }
  if (g_stop) {
    set([](Progress& p) { p.stage = Progress::kIdle; p.message = "Interrompido."; });
  } else {
    write_marker();
    set([](Progress& p) { p.stage = Progress::kDone; });
  }
  g_running = false;
}

}  // namespace

bool load_cached(uint64_t key, std::vector<char>* out) {
  Pack& p = pack();
  if (p.ok) {
    auto it = p.index.find(key);
    if (it != p.index.end()) {
      std::lock_guard<std::mutex> lock(p.mu);
      out->resize(it->second.second);
      p.in.clear();
      p.in.seekg((std::streamoff)it->second.first);
      if (p.in.read(out->data(), (std::streamsize)out->size())) return true;
    }
  }
  char name[40];
  std::snprintf(name, sizeof name, "%016llX.dxbc", (unsigned long long)key);
  std::ifstream in(vfs::user_root() / "shader_cache" / name, std::ios::binary);
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return !out->empty();
}

bool up_to_date() {
  if (pack().ok && pack().same_exe) return true;
  std::ifstream in(marker());
  std::string s;
  std::getline(in, s);
  return !s.empty() && s == signature();
}

void start() {
  if (g_running.exchange(true)) return;
  g_stop = false;
  std::thread(run).detach();
}

Progress progress() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_progress;
}

bool running() { return g_running.load(); }
void request_stop() { g_stop = true; }

bool write_pack(const fs::path& file) {
  std::vector<std::pair<uint64_t, std::vector<char>>> entries;
  std::error_code ec;
  for (auto& e : fs::directory_iterator(vfs::user_root() / "shader_cache", ec)) {
    if (e.path().extension() != ".dxbc") continue;
    const uint64_t key = std::strtoull(e.path().stem().string().c_str(), nullptr, 16);
    std::ifstream in(e.path(), std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!data.empty()) entries.push_back({key, std::move(data)});
  }
  std::ofstream out(file, std::ios::binary);
  const std::string sig = signature();
  const uint32_t n = (uint32_t)entries.size(), sl = (uint32_t)sig.size();
  out.write(kPackMagic, 8);
  out.write(reinterpret_cast<const char*>(&sl), 4);
  out.write(sig.data(), sl);
  out.write(reinterpret_cast<const char*>(&n), 4);
  for (auto& [key, data] : entries) {
    const uint32_t size = (uint32_t)data.size();
    out.write(reinterpret_cast<const char*>(&key), 8);
    out.write(reinterpret_cast<const char*>(&size), 4);
  }
  for (auto& e : entries) out.write(e.second.data(), (std::streamsize)e.second.size());
  std::fprintf(stderr, "[shaders] pacote %s: %u shaders\n", file.string().c_str(), n);
  return (bool)out;
}

int run_cli(const std::wstring& pack_out) {
  const auto t0 = std::chrono::steady_clock::now();
  start();
  Progress last;
  while (running()) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const Progress p = progress();
    std::fprintf(stderr, "[shaders] pacotes %u/%u, encontrados %u, compilados %u/%u\n", p.packages_done, p.packages_total, p.found,
                 p.compiled, p.to_compile);
  }
  const Progress p = progress();
  std::fprintf(stderr, "[shaders] fim em %.0f s: %u pacotes, %u shaders distintos, %u compilados agora\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), p.packages_total, p.found, p.compiled);
  if (p.stage != Progress::kDone) return 1;
  if (!pack_out.empty() && !write_pack(fs::path(pack_out))) return 2;
  return 0;
}

}  // namespace shader_prep
