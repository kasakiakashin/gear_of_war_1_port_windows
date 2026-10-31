// import_dispatch.cpp — liga ppc_import_call(lib, ordinal) às implementações registadas por nome.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "import_table.h"
#include "kernel/diag.h"
#include "kernel/thread.h"
#include "xbox_import.h"

namespace xbox {
namespace {

struct Registered {
  ImportFn fn = nullptr;
  bool stub = true;
};

struct Slot {
  const ImportBinding* binding = nullptr;
  ImportFn fn = nullptr;
  bool stub = true;
  std::atomic<uint64_t> calls{0};
};

// Construído durante a inicialização estática (ordem entre TUs indefinida -> função com static local).
std::unordered_map<std::string, Registered>& registry() {
  static std::unordered_map<std::string, Registered> r;
  return r;
}

std::unordered_map<uint32_t, Slot*>& slots() {
  static std::unordered_map<uint32_t, Slot*> s;
  return s;
}

std::vector<std::string>& duplicates() {
  static std::vector<std::string> d;
  return d;
}

uint32_t key(uint32_t lib, uint32_t ordinal) { return lib << 16 | ordinal; }

bool env_flag(const char* name) {
  const char* e = std::getenv(name);
  return e && *e == '1';
}

}  // namespace

ImportRegistrar::ImportRegistrar(const char* name, ImportFn fn, bool stub) {
  Registered& r = registry()[name];
  if (r.fn && !r.stub && !stub) duplicates().push_back(name);
  if (!r.fn || r.stub) r = {fn, stub};
}

bool init_imports() {
  bool ok = true;
  for (const std::string& d : duplicates()) {
    std::fprintf(stderr, "[import] implementação duplicada: %s\n", d.c_str());
    ok = false;
  }
  size_t real = 0, stub = 0, missing = 0;
  for (size_t i = 0; i < kImportBindingCount; i++) {
    const ImportBinding& b = kImportBindings[i];
    if (b.variable) continue;
    auto* s = new Slot;
    s->binding = &b;
    auto it = registry().find(b.name);
    if (it != registry().end()) {
      s->fn = it->second.fn;
      s->stub = it->second.stub;
      (s->stub ? stub : real)++;
    } else {
      missing++;
      std::fprintf(stderr, "[import] sem implementação nem stub: %s\n", b.name);
    }
    slots()[key(b.lib, b.ordinal)] = s;
  }
  std::fprintf(stderr, "[import] %zu funções: %zu implementadas, %zu stubs, %zu ausentes\n", real + stub + missing,
               real, stub, missing);
  return ok && missing == 0;
}

// A 1ª chamada de cada import (stub ou real) já é registada por ppc_import_call; com GOW_TRACE_STUBS=1
// cada chamada a um stub também é.
void stub_hit(const char* name, const PPCContext& ctx) {
  static const bool trace = env_flag("GOW_TRACE_STUBS");
  if (trace)
    std::fprintf(stderr, "[stub] %s(r3=0x%08X r4=0x%08X) lr=0x%08X\n", name, (uint32_t)ctx.r[3],
                 (uint32_t)ctx.r[4], (uint32_t)ctx.lr);
}

void print_import_stats() {
  std::vector<std::pair<uint64_t, const char*>> v;
  for (auto& [k, s] : slots())
    if (uint64_t n = s->calls.load()) v.emplace_back(n, s->binding->name);
  std::sort(v.rbegin(), v.rend());
  std::fprintf(stderr, "[import] chamadas por função (%zu distintas):\n", v.size());
  for (auto& [n, name] : v) std::fprintf(stderr, "  %10llu  %s\n", (unsigned long long)n, name);
}

}  // namespace xbox

// Gancho chamado pelos thunks de import do código traduzido.
void ppc_import_call(PPCContext& ctx, uint8_t* mem_base, uint32_t lib, uint32_t ordinal) {
  auto& s = xbox::slots();
  auto it = s.find(xbox::key(lib, ordinal));
  if (it == s.end() || !it->second->fn) {
    std::fprintf(stderr, "[import] ordinal desconhecido: lib %u ordinal 0x%04X\n", lib, ordinal);
    ctx.r[3] = 0;
    return;
  }
  auto* slot = it->second;
  diag::set_last_import(slot->binding->name);
  // Cronologia: a 1ª chamada de cada import recebe um número de ordem; GOW_TRACE_IMPORTS=1 regista todas,
  // exceto as listadas em GOW_TRACE_SKIP (nomes separados por vírgula).
  static std::atomic<uint32_t> first_calls{0};
  static const bool trace_enabled = xbox::env_flag("GOW_TRACE_IMPORTS");
  static const std::set<std::string> skip = [] {
    std::set<std::string> out;
    const char* e = std::getenv("GOW_TRACE_SKIP");
    for (std::string s = e ? e : "", item; !s.empty(); s = s.substr(std::min(s.size(), item.size() + 1))) {
      item = s.substr(0, s.find(','));
      if (!item.empty()) out.insert(item);
    }
    return out;
  }();
  const bool trace_all = trace_enabled && !skip.count(slot->binding->name);
  uint64_t n = slot->calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0 || trace_all) {
    char seq[16] = "    ";
    if (n == 0) std::snprintf(seq, sizeof seq, "#%-3u", ++first_calls);
    kernel::GuestThread* t = kernel::current_thread();
    std::fprintf(stderr, "[import] %s %-4s %s(0x%08X, 0x%08X, 0x%08X, 0x%08X) lr=0x%08X t%u\n", seq,
                 slot->stub ? "stub" : "hle", slot->binding->name, (uint32_t)ctx.r[3], (uint32_t)ctx.r[4],
                 (uint32_t)ctx.r[5], (uint32_t)ctx.r[6], (uint32_t)ctx.lr, t ? t->thread_id : 0);
  }
  slot->fn(ctx, mem_base);
  if (trace_all) {
    kernel::GuestThread* t = kernel::current_thread();
    std::fprintf(stderr, "[import]        -> %s = 0x%08X t%u\n", slot->binding->name, (uint32_t)ctx.r[3],
                 t ? t->thread_id : 0);
  }
}
