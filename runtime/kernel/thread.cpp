#include "kernel/thread.h"

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>
#include <cstdio>
#include <mutex>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"
#include "kernel/diag.h"
#include "kernel/sync.h"

namespace kernel {
namespace {

thread_local GuestThread* t_current = nullptr;
std::atomic<uint32_t> g_next_thread_id{1};
std::mutex g_mu;
uint32_t g_next_stack = gm::kStackBase;
std::unordered_map<uint32_t, GuestThread*> g_by_kthread;

}  // namespace

GuestThread* alloc_guest_thread(uint32_t stack_size) {
  if (!stack_size) stack_size = kDefaultStackSize;
  stack_size = (stack_size + 0xFFFF) & ~0xFFFFu;
  uint32_t stack;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    stack = g_next_stack;
    if (stack + stack_size + 0x10000 > gm::kStackEnd || !gm::commit(stack, stack_size)) return nullptr;
    g_next_stack += stack_size + 0x10000;  // página de guarda não confirmada entre pilhas
  }
  uint8_t* m = gm::base();
  auto* t = new GuestThread;
  t->thread_id = g_next_thread_id++;
  t->stack_limit = stack;
  t->stack_base = stack + stack_size;
  t->pcr = gm::alloc_runtime(0x1000, 0x1000);
  t->kthread = gm::alloc_runtime(0x200, 16);
  t->tls = gm::alloc_runtime(kTlsSlotCount * 4, 16);
  if (!t->pcr || !t->kthread || !t->tls) return nullptr;

  // KPCR (layout do Xenia): +0x000 TLS, +0x030 self, +0x070/+0x074 pilha, +0x100 thread atual, +0x10C CPU.
  ppc_st32(m, t->pcr + 0x000, t->tls);
  ppc_st32(m, t->pcr + 0x030, t->pcr);
  ppc_st32(m, t->pcr + 0x070, t->stack_base);
  ppc_st32(m, t->pcr + 0x074, t->stack_limit);
  ppc_st32(m, t->pcr + 0x100, t->kthread);
  ppc_st8(m, t->pcr + 0x10C, 0);
  // KTHREAD: cabeçalho de dispatcher (tipo 6, não sinalizado até terminar) e ThreadId em +0x14C.
  init_header(t->kthread, kThread, 0, 0x200 / 4);
  ppc_st32(m, t->kthread + 0x14C, t->thread_id);
  std::lock_guard<std::mutex> lock(g_mu);
  g_by_kthread[t->kthread] = t;
  return t;
}

void bind_guest_thread(GuestThread* t, PPCContext& ctx) {
  ctx = PPCContext{};
  ctx.r[1] = t->stack_base - 0x100;
  ctx.r[13] = t->pcr;
  ctx.lr = 0;
  ctx.msr = 0x9030;
  ctx.reserve_addr = 0xFFFFFFFFu;
  t->ctx = &ctx;
  t_current = t;
}

GuestThread* create_guest_thread(PPCContext& ctx, uint32_t stack_size) {
  GuestThread* t = alloc_guest_thread(stack_size);
  if (t) bind_guest_thread(t, ctx);
  return t;
}

GuestThread* current_thread() { return t_current; }

GuestThread* thread_by_kthread(uint32_t kthread) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_by_kthread.find(kthread);
  return it == g_by_kthread.end() ? nullptr : it->second;
}

uint32_t run_guest(GuestThread* t, PPCContext& ctx, uint32_t entry) {
  PPCFunc* fn = ppc_lookup(entry);
  if (!fn) {
    std::fprintf(stderr, "[thread] %u: rotina inicial 0x%08X não existe\n", t->thread_id, entry);
    return 0xFFFFFFFFu;
  }
  try {
    fn(ctx, gm::base());
    return (uint32_t)ctx.r[3];
  } catch (const ThreadExit& e) {
    return e.code;
  }
}

void finish_thread(GuestThread* t, uint32_t code) {
  t->exit_code = code;
  t->finished = true;
  set_event(t->kthread);  // SignalState = 1: esperas pelo handle da thread são satisfeitas
}

void dump_current_registers() {
  GuestThread* t = current_thread();
  if (!t || !t->ctx) return;
  const PPCContext& c = *t->ctx;
  std::fprintf(stderr, "[diag] thread %u: lr=0x%08X ctr=0x%08X\n", t->thread_id, (uint32_t)c.lr, (uint32_t)c.ctr);
  for (int i = 0; i < 32; i += 4)
    std::fprintf(stderr, "[diag]   r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i, (uint32_t)c.r[i], i + 1,
                 (uint32_t)c.r[i + 1], i + 2, (uint32_t)c.r[i + 2], i + 3, (uint32_t)c.r[i + 3]);
}

void dump_threads() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::vector<GuestThread*> v;
  for (auto& [k, t] : g_by_kthread) v.push_back(t);
  std::sort(v.begin(), v.end(), [](auto* a, auto* b) { return a->thread_id < b->thread_id; });
  for (GuestThread* t : v) {
    const PPCContext* c = t->ctx;
    std::fprintf(stderr, "[thread] %u%s: último import %s | lr=0x%08X r1=0x%08X r3=0x%08X r4=0x%08X r31=0x%08X\n",
                 t->thread_id, t->finished ? " (terminou)" : "", t->last_import ? t->last_import : "-",
                 c ? (uint32_t)c->lr : 0, c ? (uint32_t)c->r[1] : 0, c ? (uint32_t)c->r[3] : 0,
                 c ? (uint32_t)c->r[4] : 0, c ? (uint32_t)c->r[31] : 0);
    if (c && !t->finished) {  // leitura sem sincronização da pilha da thread: só para diagnóstico
      char why[24];
      std::snprintf(why, sizeof why, "thread %u", t->thread_id);
      diag::guest_backtrace(why, c->r[1], c->lr);
    }
  }
}

namespace {
std::mutex g_sample_mu;
std::map<uint32_t, std::map<std::string, uint64_t>> g_samples;  // thread -> pilha -> amostras
}  // namespace

void start_sampler() {
  std::thread([] {
    uint8_t* m = gm::base();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      std::vector<std::pair<uint32_t, std::string>> keys;
      {
        std::lock_guard<std::mutex> lock(g_mu);
        for (auto& [k, t] : g_by_kthread) {
          const PPCContext* c = t->ctx;
          if (!c || t->finished) continue;
          char buf[256];
          int n = std::snprintf(buf, sizeof buf, "%08X", (uint32_t)c->lr);
          uint32_t frame = (uint32_t)c->r[1];
          for (int i = 0; i < 12 && frame >= 0x10000u && frame < 0x7E000000u; i++) {
            uint32_t prev = ppc_ld32(m, frame);
            if (prev <= frame || prev - frame > 0x100000u) break;
            n += std::snprintf(buf + n, sizeof buf - n, " <- %08X", ppc_ld32(m, prev - 8));
            frame = prev;
          }
          keys.emplace_back(t->thread_id, buf);
        }
      }
      std::lock_guard<std::mutex> lock(g_sample_mu);
      for (auto& [id, key] : keys) g_samples[id][key]++;
    }
  }).detach();
}

void print_samples() {
  std::lock_guard<std::mutex> lock(g_sample_mu);
  for (auto& [id, stacks] : g_samples) {
    uint64_t total = 0;
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [k, n] : stacks) total += n, v.emplace_back(n, k);
    std::sort(v.rbegin(), v.rend());
    std::fprintf(stderr, "[sample] thread %u: %llu amostras\n", id, (unsigned long long)total);
    for (size_t i = 0; i < v.size() && i < 10; i++)
      std::fprintf(stderr, "[sample]   %5.1f%%  %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
  }
}

void wait_while_suspended(GuestThread* t) {
  std::unique_lock<std::mutex> lock(t->mu);
  t->cv.wait(lock, [t] { return t->suspend_count <= 0; });
}

}  // namespace kernel
