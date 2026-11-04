// printf_test.cpp — GOW_TEST_PRINTF=1: autoteste da conversão de floats para texto da CRT do próprio jogo.
//
// O jogo formata floats com o _vsnwprintf da sua CRT (0x828D5B60: buf, count, fmt, va_list), que corre traduzido; um erro de
// emulação numa instrução usada pela conversão decimal (_cfltcvt) faz o printf escrever zeros sem fim e rebentar a pilha da
// thread (visto uma vez a meio do 1.º nível). Aqui chama-se essa função com valores especiais e formatos comuns, uma linha
// por caso antes da chamada (se um caso rebentar, o último registado é o culpado), e o resultado ou o comprimento depois.
#include "kernel/printf_test.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include "guest_memory.h"
#include "ppc_context.h"

namespace kernel {

namespace {

constexpr uint32_t kVsnwprintf = 0x828D5B60u;  // _vsnwprintf(wchar_t* buf, size_t count, const wchar_t* fmt, va_list)

void put_wide(uint8_t* m, uint32_t at, const char* s) {
  size_t i = 0;
  for (; s[i]; i++) ppc_st16(m, at + 2 * (uint32_t)i, (uint16_t)(uint8_t)s[i]);
  ppc_st16(m, at + 2 * (uint32_t)i, 0);
}

std::string get_wide(uint8_t* m, uint32_t at, uint32_t max) {
  std::string o;
  for (uint32_t i = 0; i < max; i++) {
    const uint16_t c = ppc_ld16(m, at + 2 * i);
    if (!c) break;
    o += c < 0x80 ? (char)c : '?';
  }
  return o;
}

}  // namespace

void run_guest_printf_test(PPCContext& ctx) {
  static bool done = false;
  if (done || !std::getenv("GOW_TEST_PRINTF")) return;
  done = true;
  PPCFunc* fn = ppc_lookup(kVsnwprintf);
  if (!fn) {
    std::fprintf(stderr, "[printf] 0x%08X não está na tabela de despacho\n", kVsnwprintf);
    return;
  }
  uint8_t* m = gm::base();
  // Área de trabalho na pilha da thread, bem abaixo do chamador: saída (2 KiB), va_list, formato; a pilha da chamada fica abaixo.
  const uint32_t top = ((uint32_t)ctx.r[1] - 0x800) & ~0xFu;
  const uint32_t out = top - 0x1000, va = out - 0x100, fmt = va - 0x100, sp = (fmt - 0x400) & ~0xFu;
  const struct {
    const char* name;
    uint64_t bits;
  } values[] = {
      {"0", 0},
      {"1.5", std::bit_cast<uint64_t>(1.5)},
      {"-2.25", std::bit_cast<uint64_t>(-2.25)},
      {"123456789.125", std::bit_cast<uint64_t>(123456789.125)},
      {"1e10", std::bit_cast<uint64_t>(1e10)},
      {"FLT_MAX", std::bit_cast<uint64_t>((double)std::numeric_limits<float>::max())},
      {"1e300", std::bit_cast<uint64_t>(1e300)},
      {"DBL_MAX", std::bit_cast<uint64_t>(std::numeric_limits<double>::max())},
      {"1e-5", std::bit_cast<uint64_t>(1e-5)},
      {"FLT_MIN", std::bit_cast<uint64_t>((double)std::numeric_limits<float>::min())},
      {"FLT_TRUE_MIN", std::bit_cast<uint64_t>((double)std::numeric_limits<float>::denorm_min())},
      {"DBL_MIN", std::bit_cast<uint64_t>(std::numeric_limits<double>::min())},
      {"1e-310 (denormal)", std::bit_cast<uint64_t>(1e-310)},
      {"DBL_TRUE_MIN", 1},
      {"-0", 0x8000000000000000ull},
      {"+INF", 0x7FF0000000000000ull},
      {"-INF", 0xFFF0000000000000ull},
      {"qNaN", 0x7FF8000000000000ull},
      {"-qNaN (x86)", 0xFFF8000000000000ull},
      {"sNaN", 0x7FF0000000000001ull},
      {"NaN payload", 0x7FFFFFFFFFFFFFFFull},
  };
  const char* formats[] = {"%f", "%.2f", "%.3f", "%.4f", "%g", "%e", "%6.1f", "%.0f"};
  std::fprintf(stderr, "[printf] autoteste do _vsnwprintf do jogo (0x%08X), pilha em 0x%08X\n", kVsnwprintf, sp);
  auto saved = std::make_unique<PPCContext>(ctx);
  for (const char* f : formats) {
    for (const auto& v : values) {
      std::fprintf(stderr, "[printf] %-6s %-18s ... ", f, v.name);
      std::fflush(stderr);
      put_wide(m, fmt, f);
      ppc_st64(m, va, v.bits);
      for (uint32_t i = 0; i < 0x400; i += 2) ppc_st16(m, out + i, 0x2A2A);  // '**' para ver o que foi escrito
      ctx = *saved;
      ctx.r[1] = sp;
      ctx.r[3] = out;
      ctx.r[4] = 0x200;  // count em wchar_t: o resto da área fica de margem
      ctx.r[5] = fmt;
      ctx.r[6] = va;
      ctx.lr = 0;
      fn(ctx, m);
      const int32_t n = (int32_t)(uint32_t)ctx.r[3];
      std::fprintf(stderr, "%d: \"%s\"\n", n, get_wide(m, out, 0x200).c_str());
    }
  }
  // O valor do crash (0,0332 = tempo de um frame a 30 FPS, "%.4f") com os 4 modos de arredondamento do FPSCR.
  for (uint32_t rn = 0; rn < 4; rn++) {
    for (const char* f : {"%.4f", "%f", "%.2f"}) {
      std::fprintf(stderr, "[printf] arredondamento %u %-5s 0.0331879 ... ", rn, f);
      std::fflush(stderr);
      put_wide(m, fmt, f);
      ppc_st64(m, va, 0x3FA0FE0000000000ull);
      ctx = *saved;
      ppc_set_fpscr(ctx, (ctx.fpscr & ~3u) | rn);
      ctx.r[1] = sp, ctx.r[3] = out, ctx.r[4] = 0x200, ctx.r[5] = fmt, ctx.r[6] = va, ctx.lr = 0;
      fn(ctx, m);
      const int32_t n = (int32_t)(uint32_t)ctx.r[3];
      ppc_set_fpscr(ctx, saved->fpscr);
      std::fprintf(stderr, "%d: \"%s\"\n", n, get_wide(m, out, 0x200).c_str());
    }
  }
  // GOW_TEST_PRINTF=2: também padrões aleatórios (doubles quaisquer e floats promovidos, como os do UnrealScript) em %.6e,
  // comparados com o printf do host. O valor em teste fica em printf_test_last.txt antes de cada chamada (se rebentar).
  if (std::atoi(std::getenv("GOW_TEST_PRINTF")) >= 2) {
    uint64_t seed = 0x9E3779B97F4A7C15ull, mismatches = 0;
    auto next = [&] {
      seed ^= seed << 13, seed ^= seed >> 7, seed ^= seed << 17;
      return seed;
    };
    put_wide(m, fmt, "%.6e");
    const int kCount = 40000;
    for (int i = 0; i < kCount; i++) {
      uint64_t bits = next();
      if (i & 1) {  // float promovido a double
        const uint32_t fb = (uint32_t)bits;
        bits = std::bit_cast<uint64_t>((double)std::bit_cast<float>(fb));
      }
      if (FILE* lf = std::fopen("printf_test_last.txt", "wb")) std::fprintf(lf, "%d %016llX\n", i, (unsigned long long)bits), std::fclose(lf);
      ppc_st64(m, va, bits);
      ctx = *saved;
      ctx.r[1] = sp, ctx.r[3] = out, ctx.r[4] = 0x200, ctx.r[5] = fmt, ctx.r[6] = va, ctx.lr = 0;
      fn(ctx, m);
      const std::string got = get_wide(m, out, 0x200);
      const double d = std::bit_cast<double>(bits);
      char want[64];
      std::snprintf(want, sizeof want, "%.6e", d);
      // O host escreve o expoente com 2 dígitos e "inf"/"nan"; o MSVC com 3 e "1.#INF00": só se comparam os finitos.
      std::string w = want;
      if (const size_t e = w.find('e'); e != std::string::npos && w.size() - e == 4) w.insert(e + 2, "0");
      if (std::isfinite(d) && got != w && mismatches++ < 40)
        std::fprintf(stderr, "[printf] diferente: %016llX jogo \"%s\" host \"%s\"\n", (unsigned long long)bits, got.c_str(), w.c_str());
    }
    std::fprintf(stderr, "[printf] %d valores aleatórios, %llu diferentes do host\n", kCount, (unsigned long long)mismatches);
  }
  ctx = *saved;
  std::fprintf(stderr, "[printf] autoteste terminado\n");
}

}  // namespace kernel
