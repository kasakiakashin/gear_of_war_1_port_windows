#include "kernel/diag.h"

#include <cstdio>

#include "guest_memory.h"
#include "kernel/thread.h"
#include "xbox_import.h"

namespace diag {
namespace {
thread_local const char* t_last_import = nullptr;
}

void set_last_import(const char* name) {
  t_last_import = name;
  if (kernel::GuestThread* t = kernel::current_thread()) t->last_import = name;
}
const char* last_import() { return t_last_import; }

namespace {
bool g_silent = false;  // sem log: um crash abre gow_crash.log
}  // namespace

void set_silent() { g_silent = true; }

void crash_report() {
  if (g_silent) (void)std::freopen("gow_crash.log", "w", stderr);
  std::fprintf(stderr, "[diag] último import desta thread: %s\n", t_last_import ? t_last_import : "(nenhum)");
  kernel::dump_threads();
  xbox::print_import_stats();
  std::fflush(stderr);
}

void guest_backtrace(const char* why, uint64_t sp, uint64_t lr) {
  uint8_t* m = gm::base();
  std::fprintf(stderr, "[diag] backtrace (%s): lr=0x%08X", why, (uint32_t)lr);
  uint32_t frame = (uint32_t)sp;
  for (int i = 0; i < 16 && frame >= 0x10000u && frame < 0x7E000000u; i++) {
    uint32_t prev = ppc_ld32(m, frame);
    if (prev <= frame || prev - frame > 0x100000u) break;
    std::fprintf(stderr, " <- 0x%08X", ppc_ld32(m, prev - 8));
    frame = prev;
  }
  std::fputc('\n', stderr);
}

}  // namespace diag
