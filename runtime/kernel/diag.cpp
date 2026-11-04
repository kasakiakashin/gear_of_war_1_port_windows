#include "kernel/diag.h"

#include <cstdio>
#include <cstring>

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

namespace {
struct FloatFormat {
  bool set = false;
  uint64_t bits = 0;
  uint32_t buf = 0, size = 0, fmt = 0, precision = 0;
  uint32_t chain[12] = {};
};
thread_local FloatFormat t_float_format;
bool stack_word_ok(uint32_t a) { return a >= 0x70000000u && a < 0x7E000000u; }  // pilhas das threads convidadas
}  // namespace

void note_float_format(const uint8_t* mem, uint32_t value_ptr, uint32_t buf, uint32_t size, uint32_t fmt, uint32_t precision,
                       uint64_t sp, uint64_t lr) {
  uint8_t* m = const_cast<uint8_t*>(mem);
  FloatFormat& f = t_float_format;
  f.set = true;
  f.bits = stack_word_ok(value_ptr) ? ppc_ld64(m, value_ptr) : 0;
  f.buf = buf, f.size = size, f.fmt = fmt, f.precision = precision;
  for (uint32_t& c : f.chain) c = 0;
  f.chain[0] = (uint32_t)lr;
  uint32_t frame = (uint32_t)sp;
  for (int i = 1; i < 12; i++) {
    if (!stack_word_ok(frame)) break;
    const uint32_t up = ppc_ld32(m, frame);
    if (up <= frame || !stack_word_ok(up) || up - frame > 0x10000) break;
    f.chain[i] = ppc_ld32(m, up - 8);
    frame = up;
  }
}

void crash_report() {
  if (g_silent) (void)std::freopen("gow_crash.log", "w", stderr);
  std::fprintf(stderr, "[diag] último import desta thread: %s\n", t_last_import ? t_last_import : "(nenhum)");
  if (const FloatFormat& f = t_float_format; f.set) {
    double d;
    std::memcpy(&d, &f.bits, 8);
    std::fprintf(stderr, "[diag] último float formatado nesta thread: %016llX (%g) fmt '%c' precisão %d buffer 0x%08X tamanho %d; chamadas:",
                 (unsigned long long)f.bits, d, (char)f.fmt, (int32_t)f.precision, f.buf, (int32_t)f.size);
    for (uint32_t a : f.chain)
      if (a) std::fprintf(stderr, " 0x%08X", a);
    std::fprintf(stderr, "\n");
  }
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
