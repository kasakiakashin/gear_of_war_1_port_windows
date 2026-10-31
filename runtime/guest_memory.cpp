#include "guest_memory.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "kernel/diag.h"
#include "kernel/thread.h"
#include "ppc_context.h"

namespace gm {
namespace {

constexpr uint64_t kSpace = 0x100000000ull;
constexpr uint64_t kGuard = 0x10000ull;  // após os 4 GiB: absorve o deslocamento +0x1000 do espelho 0xE0000000
constexpr uint32_t kNullGuard = 0x10000u;
constexpr uint32_t kPhysicalBase[3] = {0xA0000000u, 0xC0000000u, 0xE0000000u};

uint8_t* g_base = nullptr;
std::atomic<uint64_t> g_demand{0};
bool g_demand_commit = false;
std::mutex g_alloc_mu;
uint32_t g_runtime_next = kRuntimeBase;

// Heap de páginas: cada página guarda o estado e a região (alocação) a que pertence.
class Heap {
 public:
  // `host_base`: onde as páginas vivem em g_base (o heap físico usa endereços 0..512 MiB, mas as páginas
  // estão no espelho 0xA0000000).
  Heap(uint32_t base, uint32_t size, uint32_t page, bool host_commit, uint32_t host_base)
      : base_(base), host_base_(host_base), page_(page), host_commit_(host_commit), pages_(size / page) {}

  bool contains(uint32_t a) const { return a >= base_ && a - base_ < pages_.size() * (uint64_t)page_; }
  uint32_t page_size() const { return page_; }

  bool alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi, bool top_down, uint32_t type,
             uint32_t protect, uint32_t& out) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t n = pages_for(size), step = align > page_ ? align / page_ : 1;
    uint64_t first = lo > base_ ? (lo - base_ + page_ - 1) / page_ : 0;
    uint64_t last = hi >= base_ ? std::min<uint64_t>((hi - (uint64_t)base_ + 1) / page_, pages_.size()) : 0;
    first = (first + step - 1) / step * step;
    if (last < n || first > last - n) return false;
    auto fits = [&](uint64_t p) {
      for (uint32_t i = 0; i < n; i++)
        if (pages_[p + i].state) return false;
      return true;
    };
    int64_t found = -1;
    if (top_down) {
      for (int64_t p = (int64_t)((last - n) / step * step); p >= (int64_t)first; p -= step)
        if (fits(p)) { found = p; break; }
    } else {
      for (uint64_t p = first; p + n <= last; p += step)
        if (fits(p)) { found = (int64_t)p; break; }
    }
    if (found < 0) return false;
    mark((uint32_t)found, n, type, protect);
    out = base_ + (uint32_t)found * page_;
    return true;
  }

  // Reserva num endereço fixo, ou confirma páginas dentro de uma reserva existente.
  bool alloc_fixed(uint32_t addr, uint32_t size, uint32_t type, uint32_t protect) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_, n = pages_for(size);
    if (p + n > pages_.size()) return false;
    bool all_reserved = true;
    for (uint32_t i = 0; i < n; i++) all_reserved &= pages_[p + i].state != kFree;
    if (type & kMemReserve) {
      for (uint32_t i = 0; i < n; i++)
        if (pages_[p + i].state) return false;
      mark(p, n, type, protect);
      return true;
    }
    if (!all_reserved) return false;  // MEM_COMMIT sozinho só vale dentro de uma reserva
    for (uint32_t i = 0; i < n; i++) commit_page(p + i, protect);
    return true;
  }

  bool decommit(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_, n = pages_for(size + (addr - base_) % page_);
    if (p + n > pages_.size()) return false;
    for (uint32_t i = 0; i < n; i++) {
      Page& pg = pages_[p + i];
      if (!pg.state) return false;
      if (pg.state == kCommitted) {
        pg.state = kReserved;
        pg.dirty = true;
        if (host_commit_) VirtualFree(host(p + i), page_, MEM_DECOMMIT);
      }
    }
    return true;
  }

  // Liberta a região que começa em `addr`; devolve o tamanho libertado.
  bool release(uint32_t addr, uint32_t& size) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_;
    if (p >= pages_.size() || !pages_[p].state || pages_[p].region != p) return false;
    uint32_t n = pages_[p].region_pages;
    for (uint32_t i = 0; i < n; i++) {
      Page& pg = pages_[p + i];
      if (pg.state == kCommitted && host_commit_) VirtualFree(host(p + i), page_, MEM_DECOMMIT);
      pg = Page{};
      pg.dirty = true;
    }
    size = n * page_;
    return true;
  }

  bool region_start(uint32_t addr, uint32_t& start) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_;
    if (p >= pages_.size() || !pages_[p].state) return false;
    start = base_ + pages_[p].region * page_;
    return true;
  }

  bool query(uint32_t addr, RegionInfo& r) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_;
    const Page& pg = pages_[p];
    uint32_t q = p;  // páginas seguintes com o mesmo estado/proteção na mesma região
    while (q < pages_.size() && pages_[q].state == pg.state && pages_[q].protect == pg.protect &&
           (pg.state ? pages_[q].region == pg.region : true))
      q++;
    r.base = base_ + p * page_;
    r.size = (q - p) * page_;
    if (!pg.state) {
      r = {r.base, 0, 0, r.size, kMemFree, kPageNoAccess, 0};
      return true;
    }
    r.allocation_base = base_ + pg.region * page_;
    r.allocation_protect = pages_[pg.region].alloc_protect;
    r.state = pg.state == kCommitted ? kMemCommit : kMemReserve;
    r.protect = pg.state == kCommitted ? pg.protect : 0;
    r.type = kMemPrivate;
    return true;
  }

  uint32_t protect_of(uint32_t addr) {
    std::lock_guard<std::mutex> lock(mu_);
    const Page& pg = pages_[(addr - base_) / page_];
    return pg.state == kCommitted ? pg.protect : 0;
  }

  bool set_protect(uint32_t addr, uint32_t size, uint32_t protect) {
    std::lock_guard<std::mutex> lock(mu_);
    uint32_t p = (addr - base_) / page_, n = pages_for(size + (addr - base_) % page_);
    if (p + n > pages_.size()) return false;
    for (uint32_t i = 0; i < n; i++) pages_[p + i].protect = protect;
    return true;
  }

 private:
  enum : uint8_t { kFree = 0, kReserved = 1, kCommitted = 2 };
  struct Page {
    uint8_t state = kFree;
    bool dirty = false;      // já foi usada: zerar ao confirmar de novo (heaps sem commit no host)
    uint32_t region = 0;     // índice da primeira página da região
    uint32_t region_pages = 0;
    uint32_t alloc_protect = 0;
    uint32_t protect = 0;
  };

  uint8_t* host(uint32_t p) const { return g_base + host_base_ + (uint64_t)p * page_; }
  uint32_t pages_for(uint32_t size) const { return (uint32_t)(((uint64_t)size + page_ - 1) / page_); }

  void mark(uint32_t p, uint32_t n, uint32_t type, uint32_t protect) {
    for (uint32_t i = 0; i < n; i++) {
      Page& pg = pages_[p + i];
      pg.state = kReserved;
      pg.region = p;
      pg.region_pages = i == 0 ? n : 0;
      pg.alloc_protect = protect;
      pg.protect = protect;
      if (type & kMemCommit) commit_page(p + i, protect);
    }
  }

  void commit_page(uint32_t p, uint32_t protect) {
    Page& pg = pages_[p];
    if (pg.state == kCommitted) return;
    if (host_commit_) {
      VirtualAlloc(host(p), page_, MEM_COMMIT, PAGE_READWRITE);  // páginas novas vêm zeradas
    } else if (pg.dirty) {
      std::memset(host(p), 0, page_);
    }
    pg.state = kCommitted;
    pg.dirty = false;
    pg.protect = protect;
  }

  uint32_t base_, host_base_, page_;
  bool host_commit_;
  std::vector<Page> pages_;
  std::mutex mu_;
};

Heap* g_v00 = nullptr;   // 0x00010000, 4 KiB
Heap* g_v40 = nullptr;   // 0x40000000, 64 KiB
Heap* g_phys = nullptr;  // endereços físicos 0..512 MiB, granularidade 4 KiB

Heap* virtual_heap(uint32_t addr) {
  if (g_v00->contains(addr)) return g_v00;
  if (g_v40->contains(addr)) return g_v40;
  return nullptr;
}

bool is_physical(uint32_t addr) { return addr >= kPhysicalBase[0]; }
// Endereço físico de um endereço dos espelhos (o 0xE0000000 está deslocado 0x1000, ver PPC_HOST).
uint32_t to_phys(uint32_t addr) {
  return ((addr & (kPhysicalSize - 1)) + (addr >= kPhysicalBase[2] ? 0x1000u : 0u)) & (kPhysicalSize - 1);
}
uint32_t mirror_base(uint32_t addr) { return addr & ~(kPhysicalSize - 1); }

LONG CALLBACK on_fault(EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  auto host = reinterpret_cast<uint8_t*>(er->ExceptionInformation[1]);
  if (host < g_base || host >= g_base + kSpace + kGuard) return EXCEPTION_CONTINUE_SEARCH;
  uint64_t off = (uint64_t)(host - g_base);
  uint32_t guest = (uint32_t)(off >= kPhysicalBase[2] + 0x1000ull ? off - 0x1000 : off);
  bool write = er->ExceptionInformation[0] != 0;
  if (g_demand_commit && guest >= kNullGuard && guest < kPhysicalBase[0]) {
    uint32_t page = guest & ~0xFFFFu;
    if (VirtualAlloc(g_base + page, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
      if (++g_demand <= 64) std::fprintf(stderr, "[mem] página confirmada sob demanda: 0x%08X\n", page);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  }
  std::fprintf(stderr, "[mem] FALHA: %s em 0x%08X do convidado (%s; host rip=%p)\n", write ? "escrita" : "leitura",
               guest, guest < kNullGuard ? "ponteiro nulo" : "memória não alocada",
               reinterpret_cast<void*>(ep->ContextRecord->Rip));
  kernel::dump_current_registers();
  diag::crash_report();
  return EXCEPTION_CONTINUE_SEARCH;
}

bool reserve_layout() {
  HANDLE phys = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, kPhysicalSize, nullptr);
  if (!phys) return false;
  for (int attempt = 0; attempt < 8; attempt++) {
    void* probe = VirtualAlloc(nullptr, kSpace + kGuard, MEM_RESERVE, PAGE_NOACCESS);
    if (!probe) return false;
    VirtualFree(probe, 0, MEM_RELEASE);
    auto b = static_cast<uint8_t*>(probe);
    if (VirtualAlloc(b, kPhysicalBase[0], MEM_RESERVE, PAGE_NOACCESS) != b) continue;
    bool ok = true;
    void* views[3] = {};
    for (int i = 0; i < 3 && ok; i++) {
      views[i] = MapViewOfFileEx(phys, FILE_MAP_ALL_ACCESS, 0, 0, kPhysicalSize, b + kPhysicalBase[i]);
      ok = views[i] == b + kPhysicalBase[i];
    }
    ok = ok && VirtualAlloc(b + kSpace, kGuard, MEM_RESERVE, PAGE_NOACCESS) == b + kSpace;
    if (ok) {
      g_base = b;
      return true;
    }
    for (void* v : views)
      if (v) UnmapViewOfFile(v);
    VirtualFree(b, 0, MEM_RELEASE);
  }
  return false;
}

}  // namespace

bool init() {
  if (!reserve_layout()) return false;
  const char* dc = std::getenv("GOW_DEMAND_COMMIT");
  g_demand_commit = dc && *dc == '1';
  g_v00 = new Heap(0x00010000u, 0x40000000u - 0x00010000u, 0x1000, true, 0x00010000u);
  g_v40 = new Heap(0x40000000u, kStackBase - 0x40000000u, 0x10000, true, 0x40000000u);
  g_phys = new Heap(0, kPhysicalSize, 0x1000, false, kPhysicalBase[0]);
  if (!commit(kGpuRegBase, kMmioBlockSize) || !commit(kXmaRegBase, kMmioBlockSize)) return false;
  AddVectoredExceptionHandler(1, on_fault);
  return true;
}

uint8_t* base() { return g_base; }

bool commit(uint32_t addr, uint32_t size) {
  return VirtualAlloc(g_base + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

uint32_t alloc_runtime(uint32_t size, uint32_t align) {
  std::lock_guard<std::mutex> lock(g_alloc_mu);
  uint32_t addr = (g_runtime_next + align - 1) & ~(align - 1);
  uint32_t page = addr & ~0xFFFu, end = (addr + size + 0xFFF) & ~0xFFFu;
  if (addr + size > kRuntimeEnd || !commit(page, end - page)) return 0;
  g_runtime_next = addr + size;
  return addr;
}

bool virtual_alloc(uint32_t& addr, uint32_t& size, uint32_t type, uint32_t protect) {
  if (!size) return false;
  if (addr) {
    Heap* h = virtual_heap(addr);
    if (!h) return false;
    uint32_t ps = h->page_size(), a = addr & ~(ps - 1);
    uint32_t s = (uint32_t)(((uint64_t)addr + size - a + ps - 1) & ~(uint64_t)(ps - 1));
    if (!h->alloc_fixed(a, s, type, protect)) return false;
    addr = a;
    size = s;
    return true;
  }
  if (!(type & (kMemReserve | kMemCommit))) return false;
  bool big = type & (kMemLargePages | kMem16MBPages);
  Heap* h = big ? g_v40 : g_v00;
  uint32_t align = (type & kMem16MBPages) ? 0x1000000u : h->page_size();
  uint32_t s = (uint32_t)(((uint64_t)size + align - 1) & ~(uint64_t)(align - 1));
  uint32_t out = 0;
  if (!h->alloc(s, align, 0, 0xFFFFFFFFu, type & kMemTopDown, type | kMemReserve, protect, out)) return false;
  addr = out;
  size = s;
  return true;
}

bool virtual_free(uint32_t& addr, uint32_t& size, uint32_t type) {
  Heap* h = virtual_heap(addr);
  if (!h) return false;
  if (type & kMemRelease) {
    uint32_t start;
    if (!h->region_start(addr, start) || start != (addr & ~(h->page_size() - 1))) return false;
    addr = start;
    return h->release(start, size);
  }
  if (type & kMemDecommit) {
    if (!size) {
      RegionInfo r;
      uint32_t start;
      if (!h->region_start(addr, start) || !h->query(start, r)) return false;
      addr = start;
      size = r.size;
    }
    return h->decommit(addr, size);
  }
  return false;
}

bool query(uint32_t addr, RegionInfo& out) {
  if (Heap* h = virtual_heap(addr)) return h->query(addr, out);
  if (is_physical(addr)) {
    if (!g_phys->query(to_phys(addr), out)) return false;
    uint32_t mb = mirror_base(addr) - (addr >= kPhysicalBase[2] ? 0x1000u : 0u);  // físico -> virtual
    out.base += mb;
    if (out.allocation_base || out.state != kMemFree) out.allocation_base += mb;
    return true;
  }
  if (addr >= g_ppc_image_base && addr - g_ppc_image_base < g_ppc_image_size) {
    out = {g_ppc_image_base, g_ppc_image_base, kPageReadWrite, g_ppc_image_size, kMemCommit, kPageReadWrite,
           kMemImage};
    return true;
  }
  return false;
}

uint32_t physical_alloc(uint32_t size, uint32_t protect, uint32_t min_phys, uint32_t max_phys, uint32_t align) {
  int mirror = (protect & kMemLargePages) ? 0 : (protect & kMem16MBPages) ? 1 : 2;
  uint32_t page = mirror == 0 ? 0x10000u : mirror == 1 ? 0x1000000u : 0x1000u;
  if (align < page) align = page;
  if (!max_phys || max_phys >= kPhysicalSize) max_phys = kPhysicalSize - 1;
  if (mirror == 2 && min_phys < 0x1000) min_phys = 0x1000;  // virtual 0xE0000000 + p - 0x1000
  uint32_t s = (uint32_t)(((uint64_t)size + page - 1) & ~(uint64_t)(page - 1));
  uint32_t out = 0;
  if (!size || !g_phys->alloc(s, align, min_phys, max_phys, false, kMemReserve | kMemCommit, protect, out)) return 0;
  return kPhysicalBase[mirror] + out - (mirror == 2 ? 0x1000u : 0u);
}

bool physical_free(uint32_t addr) {
  uint32_t size;
  return is_physical(addr) && g_phys->release(to_phys(addr), size);
}

uint32_t query_protect(uint32_t addr) {
  if (Heap* h = virtual_heap(addr)) return h->protect_of(addr);
  if (is_physical(addr)) return g_phys->protect_of(to_phys(addr));
  return kPageReadWrite;  // imagem, pilhas, área do runtime
}

bool set_protect(uint32_t addr, uint32_t size, uint32_t protect) {
  if (Heap* h = virtual_heap(addr)) return h->set_protect(addr, size, protect);
  if (is_physical(addr)) return g_phys->set_protect(to_phys(addr), size, protect);
  return true;
}

// O espelho 0xE0000000 está deslocado 0x1000 bytes físicos (o D3D converte endereços com esse +0x1000).
uint32_t physical_address(uint32_t addr) { return is_physical(addr) ? to_phys(addr) : addr; }

uint64_t demand_commits() { return g_demand.load(); }

}  // namespace gm
