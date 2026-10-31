#include "kernel/apc.h"

#include <cstdio>
#include <memory>

#include "guest_memory.h"
#include "kernel/sync.h"
#include "kernel/thread.h"

namespace kernel {

void queue_user_apc(GuestThread* t, uint32_t routine, uint32_t a0, uint32_t a1, uint32_t a2) {
  if (!t) t = current_thread();
  if (!t || !routine) return;
  {
    std::lock_guard<std::mutex> lock(t->mu);
    t->apcs.push_back({routine, a0, a1, a2});
    t->apc_count.fetch_add(1);
  }
  notify_waiters();  // as esperas alertáveis verificam user_apc_pending a cada acordar
}

bool user_apc_pending(GuestThread* t) { return t && t->apc_count.load() > 0; }

uint32_t deliver_user_apcs(PPCContext& ctx) {
  GuestThread* t = current_thread();
  if (!t) return 0;
  uint32_t n = 0;
  for (;;) {
    UserApc apc;
    {
      std::lock_guard<std::mutex> lock(t->mu);
      if (t->apcs.empty()) break;
      apc = t->apcs.front();
      t->apcs.pop_front();
      t->apc_count.fetch_sub(1);
    }
    PPCFunc* fn = ppc_lookup(apc.routine);
    if (!fn) {
      std::fprintf(stderr, "[apc] thread %u: rotina 0x%08X não existe\n", t->thread_id, apc.routine);
      continue;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::fprintf(stderr, "[apc] 1ª APC: thread %u, rotina 0x%08X(0x%08X, 0x%08X, 0x%08X)\n", t->thread_id,
                   apc.routine, apc.a0, apc.a1, apc.a2);
    }
    auto saved = std::make_unique<PPCContext>(ctx);
    ctx.r[1] = (ctx.r[1] - 0x400) & ~0xFull;  // abaixo da zona usada pelo chamador do import
    ctx.r[3] = apc.a0;
    ctx.r[4] = apc.a1;
    ctx.r[5] = apc.a2;
    ctx.lr = 0;
    fn(ctx, gm::base());
    ctx = *saved;
    n++;
  }
  return n;
}

}  // namespace kernel
