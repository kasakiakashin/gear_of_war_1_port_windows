// ppc_smoke_main.cpp — teste de fumo da fase 2.
//   ppc_smoke <base_image.bin>                       valida tabela de despacho e primitivas
//   ppc_smoke <base_image.bin> --call ADDR [r3 r4 r5] executa uma rotina e imprime r3
//   ppc_smoke <base_image.bin> --difftest FILE       compara com tools/recomp_difftest.py
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ppc_context.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif


static uint8_t* reserve_guest_memory() {
  const size_t size = 0x100000000ull;  // 4 GiB de espaço de endereçamento do convidado
#ifdef _WIN32
  void* p = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
  return static_cast<uint8_t*>(p);
#else
  void* p = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

static bool commit(uint8_t* base, uint32_t addr, uint32_t size) {
#ifdef _WIN32
  return VirtualAlloc(base + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
  return mprotect(base + addr, size, PROT_READ | PROT_WRITE) == 0;
#endif
}

static int g_fail = 0;
#define CHECK(c)                                                         \
  do {                                                                   \
    if (!(c)) { std::fprintf(stderr, "FALHOU: %s (linha %d)\n", #c, __LINE__); g_fail++; } \
  } while (0)

static void test_primitives(uint8_t* mem) {
  // Memória BE
  ppc_st32(mem, 0x10000, 0x11223344u);
  CHECK(mem[0x10000] == 0x11 && mem[0x10003] == 0x44);
  CHECK(ppc_ld32(mem, 0x10000) == 0x11223344u);
  CHECK(ppc_ld16(mem, 0x10002) == 0x3344);
  ppc_stf32(mem, 0x10010, 1.5);
  CHECK(ppc_ld32(mem, 0x10010) == 0x3FC00000u);
  // Vetores: lvx/stvx e acesso por elemento BE
  for (int i = 0; i < 16; i++) mem[0x10020 + i] = (uint8_t)i;
  PPCVReg v;
  ppc_lvx(v, mem, 0x10020);
  CHECK(PPC_VB8(v, 0) == 0 && PPC_VB8(v, 15) == 15);
  CHECK(PPC_VW32(v, 0) == 0x00010203u && PPC_VW32(v, 3) == 0x0C0D0E0Fu);
  ppc_stvx(v, mem, 0x10030);
  CHECK(std::memcmp(mem + 0x10020, mem + 0x10030, 16) == 0);
  // vperm identidade via lvsl(0)
  PPCVReg ctl, r;
  ppc_lvsl(ctl, 0);
  ppc_vperm(r, v, v, ctl);
  CHECK(std::memcmp(&r, &v, 16) == 0);
  // vsldoi 4 e vspltw
  ppc_vsldoi(r, v, v, 4);
  CHECK(PPC_VW32(r, 0) == 0x04050607u && PPC_VW32(r, 3) == 0x00010203u);
  ppc_vspltw(r, v, 1);
  CHECK(r.u32[0] == 0x04050607u && r.u32[3] == 0x04050607u);
  // vmrghw
  ppc_vmrghw(r, v, ctl);
  CHECK(PPC_VW32(r, 0) == 0x00010203u && PPC_VW32(r, 1) == 0x00010203u && PPC_VW32(r, 2) == 0x04050607u);
  // lvlx/lvrx (carga desalinhada)
  for (int i = 0; i < 32; i++) mem[0x10040 + i] = (uint8_t)(0x80 + i);
  PPCVReg l, rr, u;
  ppc_lvlx(l, mem, 0x10043);
  ppc_lvrx(rr, mem, 0x10053);
  ppc_vor(u, l, rr);
  for (int i = 0; i < 16; i++) CHECK(PPC_VB8(u, i) == 0x83 + i);
  // CR / XER / carry
  PPCContext ctx{};
  ppc_set_cr(ctx, 0x12345678u, 0xFF);
  CHECK(ppc_get_cr(ctx) == 0x12345678u);
  uint8_t ca = 0;
  CHECK(ppc_add_ca(~0ull, 1, 0, ca) == 0 && ca == 1);
  CHECK(ppc_sraw(0xFFFFFFF1ull, 4, ca) == 0xFFFFFFFFFFFFFFFFull && ca == 1);
  CHECK(ppc_rotl32d(0x80000001ull, 1) == 0x0000000300000003ull);
  // fctiwz / half
  CHECK((uint32_t)ppc_fbits(ppc_fctiw(-3.7, true)) == (uint32_t)-3);
  CHECK(ppc_half_to_float(0x3C00) == 1.0f && ppc_float_to_half(1.0f) == 0x3C00);
}

// Teste diferencial: casos gerados por tools/recomp_difftest.py (interpretador de referência)
static void reset_test_area(uint8_t* mem) {
  static const char text[] = "Gears of War recompiled from PowerPC to C++20";
  uint8_t* a = mem + 0x10000;
  for (uint32_t i = 0; i < 0x10000; i++) a[i] = (uint8_t)(i * 7 + 3);
  size_t n = sizeof(text);  // inclui o NUL
  std::memcpy(a, text, n);
  for (size_t i = 0; i + 1 < n; i++) a[0x800 + i] = (uint8_t)text[n - 2 - i];
  a[0x800 + n - 1] = 0;
}

static uint64_t fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

static int run_difftest(uint8_t* mem, const char* path) {
  FILE* f = std::fopen(path, "r");
  if (!f) { std::perror(path); return 1; }
  char line[512];
  int total = 0, bad = 0;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    unsigned long long v[7];
    if (std::sscanf(line, "%llx %llx %llx %llx %llx %llx %llx", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7)
      continue;
    PPCFunc* fn = ppc_lookup((uint32_t)v[0]);
    total++;
    if (!fn) { std::fprintf(stderr, "difftest: sem rotina 0x%08llX\n", v[0]); bad++; continue; }
    reset_test_area(mem);
    static PPCContext ctx;
    ctx = PPCContext{};
    ctx.r[1] = 0x1F000;
    for (int k = 0; k < 4; k++) ctx.r[3 + k] = v[1 + k];
    fn(ctx, mem);
    uint64_t h = fnv1a64(mem + 0x10000, 0x10000);
    if (ctx.r[3] != v[5] || h != v[6]) {
      if (bad < 20)
        std::fprintf(stderr, "difftest: sub_%08llX(r3=%llx) -> r3=%llx esperado %llx, mem %s\n", v[0], v[1],
                     (unsigned long long)ctx.r[3], v[5], h == v[6] ? "ok" : "DIFERENTE");
      bad++;
    }
  }
  std::fclose(f);
  std::printf("difftest: %d/%d casos coincidem com o interpretador de referência\n", total - bad, total);
  return bad;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "uso: %s <base_image.bin> [--call ADDR [r3 r4 r5]]\n", argv[0]);
    return 2;
  }
  uint8_t* mem = reserve_guest_memory();
  if (!mem || !commit(mem, g_ppc_image_base, g_ppc_image_size) || !commit(mem, 0x10000, 0x10000) ||
      !commit(mem, 0x70000000, 0x100000)) {
    std::fprintf(stderr, "falha ao reservar memória do convidado\n");
    return 1;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) { std::perror(argv[1]); return 1; }
  size_t n = std::fread(mem + g_ppc_image_base, 1, g_ppc_image_size, f);
  std::fclose(f);
  std::printf("imagem: %zu bytes em 0x%08X\n", n, g_ppc_image_base);

  // 1) Tabela de despacho
  uint32_t bad_order = 0, bad_lookup = 0;
  for (uint32_t i = 0; i < g_ppc_func_count; i++) {
    if (i && g_ppc_func_table[i - 1].guest >= g_ppc_func_table[i].guest) bad_order++;
    if (ppc_lookup(g_ppc_func_table[i].guest) != g_ppc_func_table[i].host) bad_lookup++;
  }
  CHECK(bad_order == 0);
  CHECK(bad_lookup == 0);
  CHECK(ppc_lookup(g_ppc_entry_point) != nullptr);
  std::printf("tabela de despacho: %u rotinas, ordem ok=%d, lookup ok=%d, entry 0x%08X presente=%d\n",
              g_ppc_func_count, bad_order == 0, bad_lookup == 0, g_ppc_entry_point,
              ppc_lookup(g_ppc_entry_point) != nullptr);

  // 2) Primitivas
  test_primitives(mem);

  // 3) Teste diferencial / execução opcional de uma rotina
  for (int i = 2; i < argc; i++) {
    if (std::strcmp(argv[i], "--difftest") == 0 && i + 1 < argc) {
      if (run_difftest(mem, argv[i + 1])) g_fail++;
      i++;
      continue;
    }
    if (std::strcmp(argv[i], "--call") == 0 && i + 1 < argc) {
      uint32_t addr = (uint32_t)std::strtoul(argv[i + 1], nullptr, 0);
      PPCFunc* fn = ppc_lookup(addr);
      if (!fn) { std::fprintf(stderr, "sem rotina em 0x%08X\n", addr); return 1; }
      static PPCContext ctx{};
      ctx.r[1] = 0x70000000 + 0x100000 - 0x1000;  // pilha
      for (int k = 0; k < 3 && i + 2 + k < argc; k++) ctx.r[3 + k] = std::strtoull(argv[i + 2 + k], nullptr, 0);
      ctx.lr = 0;
      fn(ctx, mem);
      std::printf("sub_%08X -> r3 = 0x%016llX\n", addr, (unsigned long long)ctx.r[3]);
      break;
    }
  }
  std::printf(g_fail ? "SMOKE: %d falha(s)\n" : "SMOKE: OK\n", g_fail);
  return g_fail ? 1 : 0;
}
