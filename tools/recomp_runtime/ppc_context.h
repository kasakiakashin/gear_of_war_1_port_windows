// ppc_context.h — estado da CPU Xenon (PowerPC 64-bit + VMX128) e primitivas usadas pelo
// código gerado por tools/ppc_recomp.py. Este arquivo é copiado tal como está para generated_src/.
//
// Convenções:
//  * Memória do convidado: `mem_base + (uint32_t)EA`. Os dados são big-endian; toda leitura/escrita
//    passa por std::byteswap (ppc_bswap) para o host x86_64 little-endian.
//  * GPRs têm 64 bits e seguem a semântica do modo 64-bit (MSR[SF]=1) do Xenon: Rc=1 compara o
//    resultado de 64 bits, XER[CA] vem do carry de 64 bits.
//  * Registos vetoriais são guardados com os 16 bytes em ordem invertida (o byte BE 0 fica em u8[15]).
//    Assim, um lvx é um byteswap de 128 bits e a palavra BE i fica em u32[3 - i]. As macros
//    PPC_VB8/PPC_VH16/PPC_VW32/PPC_VF32 dão acesso pelo índice de elemento BE.
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cfenv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <version>

// ---------------------------------------------------------------------------------------------
// Byteswap: std::byteswap (C++23 <bit>) quando disponível; senão o builtin equivalente.
// ---------------------------------------------------------------------------------------------
#if defined(__cpp_lib_byteswap)
template <typename T> constexpr T ppc_bswap(T v) noexcept { return std::byteswap(v); }
#else
constexpr uint8_t ppc_bswap(uint8_t v) noexcept { return v; }
constexpr uint16_t ppc_bswap(uint16_t v) noexcept { return __builtin_bswap16(v); }
constexpr uint32_t ppc_bswap(uint32_t v) noexcept { return __builtin_bswap32(v); }
constexpr uint64_t ppc_bswap(uint64_t v) noexcept { return __builtin_bswap64(v); }
#endif

// ---------------------------------------------------------------------------------------------
// Estado dos registos
// ---------------------------------------------------------------------------------------------
struct PPCCRField {
  uint8_t lt, gt, eq, so;
};

union alignas(16) PPCVReg {
  uint8_t u8[16];
  int8_t s8[16];
  uint16_t u16[8];
  int16_t s16[8];
  uint32_t u32[4];
  int32_t s32[4];
  float f32[4];
  uint64_t u64[2];
};

struct PPCXER {
  uint8_t so, ov, ca;
};

struct PPCContext {
  uint64_t r[32];       // GPRs
  double f[32];         // FPRs
  PPCVReg v[128];       // VR0-VR127 (VMX128)
  PPCCRField cr[8];     // CR0-CR7
  uint64_t lr;
  uint64_t ctr;
  PPCXER xer;
  uint32_t fpscr;
  uint32_t vscr;
  uint32_t vrsave;
  uint64_t msr;
  uint32_t reserve_addr;  // lwarx/ldarx
  uint64_t reserve_val;   // valor bruto (BE) lido pelo lwarx/ldarx
  void* user;             // ponteiro livre para o runtime (thread/kernel state)
};

using PPCFunc = void(PPCContext& ctx, uint8_t* mem_base);
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* mem_base)

struct PPCFuncMapping {
  uint32_t guest;
  PPCFunc* host;
};

// ---------------------------------------------------------------------------------------------
// Ganchos do runtime (implementação padrão em ppc_runtime.cpp; a fase 3 substitui)
// ---------------------------------------------------------------------------------------------
PPCFunc* ppc_lookup(uint32_t guest);
void ppc_call_indirect(PPCContext& ctx, uint8_t* mem_base, uint32_t target);
void ppc_import_call(PPCContext& ctx, uint8_t* mem_base, uint32_t lib, uint32_t ordinal);
void ppc_syscall(PPCContext& ctx, uint8_t* mem_base, uint32_t addr);
void ppc_trap(PPCContext& ctx, uint8_t* mem_base, uint32_t addr, uint32_t insn);
void ppc_unimplemented(PPCContext& ctx, uint8_t* mem_base, uint32_t addr, uint32_t insn);
void ppc_unreachable(PPCContext& ctx, uint8_t* mem_base, uint32_t addr);
uint64_t ppc_mftb();
// Escrita byte-reversed (stwbrx) no bloco MMIO do XMA (0x7FEA0000): o jogo escreve Kick/Lock/Clear assim e o hardware
// reage antes da instrução seguinte (o Xenia também processa o Kick de forma síncrona).
void ppc_mmio_st32r(uint8_t* mem_base, uint32_t ea, uint32_t v);

extern const PPCFuncMapping g_ppc_func_table[];
extern const uint32_t g_ppc_func_count;
extern const uint32_t g_ppc_entry_point;
extern const uint32_t g_ppc_image_base;
extern const uint32_t g_ppc_image_size;

// Variáveis importadas (xboxkrnl): o loader escreve em `record` o endereço convidado do objeto.
struct PPCImportVar {
  uint32_t lib;      // 0 = xam.xex, 1 = xboxkrnl.exe
  uint32_t ordinal;
  uint32_t record;   // endereço do slot na imagem
};
extern const PPCImportVar g_ppc_import_vars[];
extern const uint32_t g_ppc_import_var_count;

// ---------------------------------------------------------------------------------------------
// Memória (big-endian)
// ---------------------------------------------------------------------------------------------
// Espelho físico 0xE0000000-0xFFFFFFFF: o hardware desloca-o 0x1000 bytes (virtual 0xE0000000 + x = físico
// x + 0x1000). Views de ficheiro do host só alinham a 64 KiB, por isso — como no Xenia — o deslocamento entra
// na tradução de endereço: o host reserva 4 GiB + 64 KiB e a view do espelho E fica sem deslocamento.
static inline uint64_t ppc_host_offset(uint32_t ea) { return (uint64_t)ea + ((ea >= 0xE0000000u) ? 0x1000u : 0u); }
#define PPC_HOST(b, ea) ((b) + ppc_host_offset((uint32_t)(ea)))
#define PPC_MEM(ea) PPC_HOST(mem_base, ea)

static inline uint8_t ppc_ld8(const uint8_t* b, uint32_t ea) { return *PPC_HOST(b, ea); }
static inline uint16_t ppc_ld16(const uint8_t* b, uint32_t ea) {
  uint16_t v; std::memcpy(&v, PPC_HOST(b, ea), 2); return ppc_bswap(v);
}
static inline uint32_t ppc_ld32(const uint8_t* b, uint32_t ea) {
  uint32_t v; std::memcpy(&v, PPC_HOST(b, ea), 4); return ppc_bswap(v);
}
static inline uint64_t ppc_ld64(const uint8_t* b, uint32_t ea) {
  uint64_t v; std::memcpy(&v, PPC_HOST(b, ea), 8); return ppc_bswap(v);
}
static inline void ppc_st8(uint8_t* b, uint32_t ea, uint8_t v) { *PPC_HOST(b, ea) = v; }
static inline void ppc_st16(uint8_t* b, uint32_t ea, uint16_t v) {
  v = ppc_bswap(v); std::memcpy(PPC_HOST(b, ea), &v, 2);
}
static inline void ppc_st32(uint8_t* b, uint32_t ea, uint32_t v) {
  v = ppc_bswap(v); std::memcpy(PPC_HOST(b, ea), &v, 4);
}
static inline void ppc_st64(uint8_t* b, uint32_t ea, uint64_t v) {
  v = ppc_bswap(v); std::memcpy(PPC_HOST(b, ea), &v, 8);
}
// Variantes byte-reversed (lwbrx & cia.): o dado na memória já está em LE.
static inline uint16_t ppc_ld16r(const uint8_t* b, uint32_t ea) { uint16_t v; std::memcpy(&v, PPC_HOST(b, ea), 2); return v; }
static inline uint32_t ppc_ld32r(const uint8_t* b, uint32_t ea) { uint32_t v; std::memcpy(&v, PPC_HOST(b, ea), 4); return v; }
static inline uint64_t ppc_ld64r(const uint8_t* b, uint32_t ea) { uint64_t v; std::memcpy(&v, PPC_HOST(b, ea), 8); return v; }
static inline void ppc_st16r(uint8_t* b, uint32_t ea, uint16_t v) { std::memcpy(PPC_HOST(b, ea), &v, 2); }
static inline void ppc_st32r(uint8_t* b, uint32_t ea, uint32_t v) {
  if (__builtin_expect((ea >> 16) == 0x7FEAu, 0)) return ppc_mmio_st32r(b, ea, v);
  std::memcpy(PPC_HOST(b, ea), &v, 4);
}
static inline void ppc_st64r(uint8_t* b, uint32_t ea, uint64_t v) { std::memcpy(PPC_HOST(b, ea), &v, 8); }

static inline float ppc_ldf32(const uint8_t* b, uint32_t ea) { return std::bit_cast<float>(ppc_ld32(b, ea)); }
static inline double ppc_ldf64(const uint8_t* b, uint32_t ea) { return std::bit_cast<double>(ppc_ld64(b, ea)); }
static inline void ppc_stf32(uint8_t* b, uint32_t ea, double v) { ppc_st32(b, ea, std::bit_cast<uint32_t>((float)v)); }
static inline void ppc_stf64(uint8_t* b, uint32_t ea, double v) { ppc_st64(b, ea, std::bit_cast<uint64_t>(v)); }

// Reserva atómica (lwarx/stwcx., ldarx/stdcx.)
static inline uint32_t ppc_lwarx(PPCContext& ctx, uint8_t* b, uint32_t ea) {
  uint32_t raw = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(PPC_HOST(b, ea))).load();
  ctx.reserve_addr = ea; ctx.reserve_val = raw;
  return ppc_bswap(raw);
}
static inline uint64_t ppc_ldarx(PPCContext& ctx, uint8_t* b, uint32_t ea) {
  uint64_t raw = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(PPC_HOST(b, ea))).load();
  ctx.reserve_addr = ea; ctx.reserve_val = raw;
  return ppc_bswap(raw);
}
static inline bool ppc_stwcx(PPCContext& ctx, uint8_t* b, uint32_t ea, uint32_t v) {
  bool ok = false;
  if (ctx.reserve_addr == ea) {
    uint32_t expected = (uint32_t)ctx.reserve_val;
    ok = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(PPC_HOST(b, ea))).compare_exchange_strong(expected, ppc_bswap(v));
  }
  ctx.reserve_addr = 0xFFFFFFFFu;
  return ok;
}
static inline bool ppc_stdcx(PPCContext& ctx, uint8_t* b, uint32_t ea, uint64_t v) {
  bool ok = false;
  if (ctx.reserve_addr == ea) {
    uint64_t expected = ctx.reserve_val;
    ok = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(PPC_HOST(b, ea))).compare_exchange_strong(expected, ppc_bswap(v));
  }
  ctx.reserve_addr = 0xFFFFFFFFu;
  return ok;
}

static inline void ppc_dcbz(uint8_t* b, uint32_t ea, uint32_t size) { std::memset(PPC_HOST(b, ea & ~(size - 1)), 0, size); }

// ---------------------------------------------------------------------------------------------
// Inteiros / CR / XER
// ---------------------------------------------------------------------------------------------
static inline void ppc_cmp_s(PPCCRField& c, int64_t a, int64_t b, const PPCXER& x) {
  c.lt = a < b; c.gt = a > b; c.eq = a == b; c.so = x.so;
}
static inline void ppc_cmp_u(PPCCRField& c, uint64_t a, uint64_t b, const PPCXER& x) {
  c.lt = a < b; c.gt = a > b; c.eq = a == b; c.so = x.so;
}
static inline void ppc_cmp_f(PPCCRField& c, double a, double b) {
  c.lt = a < b; c.gt = a > b; c.eq = a == b; c.so = std::isnan(a) || std::isnan(b);
}
static inline void ppc_cr_set4(PPCCRField& c, uint32_t n) {
  c.lt = (n >> 3) & 1; c.gt = (n >> 2) & 1; c.eq = (n >> 1) & 1; c.so = n & 1;
}
static inline uint32_t ppc_cr_get4(const PPCCRField& c) {
  return (uint32_t)(c.lt << 3 | c.gt << 2 | c.eq << 1 | c.so);
}
static inline uint32_t ppc_get_cr(const PPCContext& ctx) {
  uint32_t v = 0;
  for (int i = 0; i < 8; i++) v |= ppc_cr_get4(ctx.cr[i]) << (28 - 4 * i);
  return v;
}
static inline void ppc_set_cr(PPCContext& ctx, uint32_t v, uint32_t fxm) {
  for (int i = 0; i < 8; i++)
    if (fxm & (0x80u >> i)) ppc_cr_set4(ctx.cr[i], (v >> (28 - 4 * i)) & 0xF);
}
static inline uint32_t ppc_get_xer(const PPCContext& ctx) {
  return (uint32_t)ctx.xer.so << 31 | (uint32_t)ctx.xer.ov << 30 | (uint32_t)ctx.xer.ca << 29;
}
static inline void ppc_set_xer(PPCContext& ctx, uint64_t v) {
  ctx.xer.so = (v >> 31) & 1; ctx.xer.ov = (v >> 30) & 1; ctx.xer.ca = (v >> 29) & 1;
}

// rlw*: rotação da palavra baixa duplicada nas duas metades (semântica de 64 bits)
static inline uint64_t ppc_rotl32d(uint64_t x, unsigned n) {
  uint64_t w = (uint32_t)x;
  return std::rotl(w | (w << 32), (int)(n & 31));
}

// Adição com carry de 64 bits: devolve soma e escreve CA
static inline uint64_t ppc_add_ca(uint64_t a, uint64_t b, uint64_t cin, uint8_t& ca) {
  uint64_t s = a + b;
  uint8_t c1 = s < a;
  uint64_t r = s + cin;
  ca = c1 | (r < s);
  return r;
}

static inline uint64_t ppc_divw(uint64_t a, uint64_t b) {
  int32_t x = (int32_t)a, y = (int32_t)b;
  if (y == 0 || (x == INT32_MIN && y == -1)) return 0;
  return (uint32_t)(x / y);
}
static inline uint64_t ppc_divwu(uint64_t a, uint64_t b) {
  uint32_t x = (uint32_t)a, y = (uint32_t)b;
  return y == 0 ? 0 : (uint64_t)(x / y);
}
static inline uint64_t ppc_divd(uint64_t a, uint64_t b) {
  int64_t x = (int64_t)a, y = (int64_t)b;
  if (y == 0 || (x == INT64_MIN && y == -1)) return 0;
  return (uint64_t)(x / y);
}
static inline uint64_t ppc_divdu(uint64_t a, uint64_t b) { return b == 0 ? 0 : a / b; }

static inline uint64_t ppc_slw(uint64_t s, uint64_t b) {
  unsigned n = (unsigned)b & 0x3F;
  return n > 31 ? 0 : (uint64_t)(uint32_t)((uint32_t)s << n);
}
static inline uint64_t ppc_srw(uint64_t s, uint64_t b) {
  unsigned n = (unsigned)b & 0x3F;
  return n > 31 ? 0 : (uint64_t)((uint32_t)s >> n);
}
static inline uint64_t ppc_sld(uint64_t s, uint64_t b) {
  unsigned n = (unsigned)b & 0x7F;
  return n > 63 ? 0 : s << n;
}
static inline uint64_t ppc_srd(uint64_t s, uint64_t b) {
  unsigned n = (unsigned)b & 0x7F;
  return n > 63 ? 0 : s >> n;
}
static inline uint64_t ppc_sraw(uint64_t s, unsigned n, uint8_t& ca) {
  int32_t v = (int32_t)s;
  n &= 0x3F;
  if (n > 31) { ca = v < 0; return (uint64_t)(int64_t)(v < 0 ? -1 : 0); }
  ca = (v < 0) && n && ((uint32_t)v & ((1u << n) - 1));
  return (uint64_t)(int64_t)(v >> n);
}
static inline uint64_t ppc_srad(uint64_t s, unsigned n, uint8_t& ca) {
  int64_t v = (int64_t)s;
  n &= 0x7F;
  if (n > 63) { ca = v < 0; return (uint64_t)(v < 0 ? -1 : 0); }
  ca = (v < 0) && n && ((uint64_t)v & ((1ull << n) - 1));
  return (uint64_t)(v >> n);
}
static inline uint64_t ppc_mulhw(uint64_t a, uint64_t b) {
  return (uint64_t)(int64_t)(int32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
}
static inline uint64_t ppc_mulhwu(uint64_t a, uint64_t b) {
  return (uint64_t)(uint32_t)(((uint64_t)(uint32_t)a * (uint64_t)(uint32_t)b) >> 32);
}
static inline uint64_t ppc_mulhd(uint64_t a, uint64_t b) {
  return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64);
}
static inline uint64_t ppc_mulhdu(uint64_t a, uint64_t b) {
  return (uint64_t)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
}

// Trap: TO = lt(16) gt(8) eq(4) ltu(2) gtu(1)
static inline bool ppc_trap_cond(uint32_t to, int64_t a, int64_t b) {
  return ((to & 16) && a < b) || ((to & 8) && a > b) || ((to & 4) && a == b) ||
         ((to & 2) && (uint64_t)a < (uint64_t)b) || ((to & 1) && (uint64_t)a > (uint64_t)b);
}

// ---------------------------------------------------------------------------------------------
// Ponto flutuante escalar
// ---------------------------------------------------------------------------------------------
static inline double ppc_f2s(double v) { return (double)(float)v; }
static inline uint64_t ppc_fbits(double v) { return std::bit_cast<uint64_t>(v); }
static inline double ppc_fbits_d(uint64_t v) { return std::bit_cast<double>(v); }

static inline double ppc_fctiw(double v, bool trunc) {
  int32_t r;
  if (std::isnan(v)) r = INT32_MIN;
  else {
    double t = trunc ? std::trunc(v) : std::nearbyint(v);
    if (t >= 2147483647.0) r = INT32_MAX;
    else if (t <= -2147483648.0) r = INT32_MIN;
    else r = (int32_t)t;
  }
  return std::bit_cast<double>(0xFFF8000000000000ull | (uint32_t)r);
}
static inline double ppc_fctid(double v, bool trunc) {
  int64_t r;
  if (std::isnan(v)) r = INT64_MIN;
  else {
    double t = trunc ? std::trunc(v) : std::nearbyint(v);
    if (t >= 9223372036854775807.0) r = INT64_MAX;
    else if (t <= -9223372036854775808.0) r = INT64_MIN;
    else r = (int64_t)t;
  }
  return std::bit_cast<double>((uint64_t)r);
}
static inline double ppc_fcfid(double v) { return (double)(int64_t)std::bit_cast<uint64_t>(v); }
static inline double ppc_fsel(double a, double b, double c) { return a >= 0.0 ? b : c; }
static inline double ppc_fnabs(double v) { return -std::fabs(v); }

static inline void ppc_set_fpscr(PPCContext& ctx, uint32_t v) {
  static const int modes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
  if ((v ^ ctx.fpscr) & 3) std::fesetround(modes[v & 3]);
  ctx.fpscr = v;
}
static inline void ppc_mtfsf(PPCContext& ctx, uint32_t fm, double src) {
  uint32_t v = (uint32_t)std::bit_cast<uint64_t>(src), m = 0;
  for (int i = 0; i < 8; i++)
    if (fm & (0x80u >> i)) m |= 0xFu << (28 - 4 * i);
  ppc_set_fpscr(ctx, (ctx.fpscr & ~m) | (v & m));
}
static inline void ppc_fp_cr1(PPCContext& ctx) { ppc_cr_set4(ctx.cr[1], ctx.fpscr >> 28); }

// ---------------------------------------------------------------------------------------------
// Vetores (VMX / VMX128). Acesso por índice de elemento big-endian:
// ---------------------------------------------------------------------------------------------
#define PPC_VB8(v, i) ((v).u8[15 - (i)])
#define PPC_VH16(v, i) ((v).u16[7 - (i)])
#define PPC_VW32(v, i) ((v).u32[3 - (i)])
#define PPC_VF32(v, i) ((v).f32[3 - (i)])

static inline void ppc_lvx(PPCVReg& d, const uint8_t* b, uint32_t ea) {
  ea &= ~0xFu;
  for (int i = 0; i < 16; i++) d.u8[15 - i] = PPC_HOST(b, ea)[i];
}
static inline void ppc_stvx(const PPCVReg& s, uint8_t* b, uint32_t ea) {
  ea &= ~0xFu;
  for (int i = 0; i < 16; i++) PPC_HOST(b, ea)[i] = s.u8[15 - i];
}
// lvebx/lvehx/lvewx: carrega o bloco alinhado inteiro (o elemento endereçado fica no lugar certo;
// os demais são "indefinidos" pela arquitetura).
#define ppc_lvebx ppc_lvx
#define ppc_lvehx ppc_lvx
#define ppc_lvewx ppc_lvx
static inline void ppc_stvebx(const PPCVReg& s, uint8_t* b, uint32_t ea) { *PPC_HOST(b, ea) = PPC_VB8(s, ea & 15); }
static inline void ppc_stvehx(const PPCVReg& s, uint8_t* b, uint32_t ea) {
  ea &= ~1u;
  for (int i = 0; i < 2; i++) PPC_HOST(b, ea)[i] = PPC_VB8(s, (ea & 15) + i);
}
static inline void ppc_stvewx(const PPCVReg& s, uint8_t* b, uint32_t ea) {
  ea &= ~3u;
  for (int i = 0; i < 4; i++) PPC_HOST(b, ea)[i] = PPC_VB8(s, (ea & 15) + i);
}
static inline void ppc_lvlx(PPCVReg& d, const uint8_t* b, uint32_t ea) {
  PPCVReg r{}; uint32_t n = 16 - (ea & 15);
  for (uint32_t i = 0; i < n; i++) PPC_VB8(r, i) = PPC_HOST(b, ea)[i];
  d = r;
}
static inline void ppc_lvrx(PPCVReg& d, const uint8_t* b, uint32_t ea) {
  PPCVReg r{}; uint32_t sh = ea & 15, al = ea & ~15u;
  for (uint32_t i = 0; i < sh; i++) PPC_VB8(r, 16 - sh + i) = PPC_HOST(b, al)[i];
  d = r;
}
static inline void ppc_stvlx(const PPCVReg& s, uint8_t* b, uint32_t ea) {
  uint32_t n = 16 - (ea & 15);
  for (uint32_t i = 0; i < n; i++) PPC_HOST(b, ea)[i] = PPC_VB8(s, i);
}
static inline void ppc_stvrx(const PPCVReg& s, uint8_t* b, uint32_t ea) {
  uint32_t sh = ea & 15, al = ea & ~15u;
  for (uint32_t i = 0; i < sh; i++) PPC_HOST(b, al)[i] = PPC_VB8(s, 16 - sh + i);
}
static inline void ppc_lvsl(PPCVReg& d, uint32_t ea) {
  for (uint32_t i = 0; i < 16; i++) PPC_VB8(d, i) = (uint8_t)((ea & 15) + i);
}
static inline void ppc_lvsr(PPCVReg& d, uint32_t ea) {
  for (uint32_t i = 0; i < 16; i++) PPC_VB8(d, i) = (uint8_t)(16 - (ea & 15) + i);
}

// Saturações
template <typename T> static inline T ppc_sat(int64_t v) {
  return (T)std::clamp<int64_t>(v, std::numeric_limits<T>::min(), std::numeric_limits<T>::max());
}

// Operações elemento a elemento (independentes da ordem dos elementos)
#define PPC_VOP2(name, F, N, expr)                                         \
  static inline void name(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) { \
    PPCVReg r;                                                              \
    for (int i = 0; i < N; i++) { auto x = a.F[i]; auto y = b.F[i]; r.F[i] = (expr); } \
    d = r;                                                                  \
  }
#define PPC_VOP1(name, F, N, expr)                       \
  static inline void name(PPCVReg& d, const PPCVReg& b) { \
    PPCVReg r;                                            \
    for (int i = 0; i < N; i++) { auto x = b.F[i]; r.F[i] = (expr); } \
    d = r;                                                \
  }
#define PPC_VCMP(name, F, N, expr)                                          \
  static inline void name(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) { \
    PPCVReg r;                                                              \
    for (int i = 0; i < N; i++) { auto x = a.F[i]; auto y = b.F[i]; r.F[i] = (expr) ? (std::remove_reference_t<decltype(r.F[i])>)-1 : 0; } \
    d = r;                                                                  \
  }

PPC_VOP2(ppc_vaddubm, u8, 16, (uint8_t)(x + y))
PPC_VOP2(ppc_vadduhm, u16, 8, (uint16_t)(x + y))
PPC_VOP2(ppc_vadduwm, u32, 4, x + y)
PPC_VOP2(ppc_vaddubs, u8, 16, ppc_sat<uint8_t>((int64_t)x + y))
PPC_VOP2(ppc_vadduhs, u16, 8, ppc_sat<uint16_t>((int64_t)x + y))
PPC_VOP2(ppc_vadduws, u32, 4, (uint32_t)std::min<uint64_t>((uint64_t)x + y, 0xFFFFFFFFull))
PPC_VOP2(ppc_vaddsbs, s8, 16, ppc_sat<int8_t>((int64_t)x + y))
PPC_VOP2(ppc_vaddshs, s16, 8, ppc_sat<int16_t>((int64_t)x + y))
PPC_VOP2(ppc_vaddsws, s32, 4, ppc_sat<int32_t>((int64_t)x + y))
PPC_VOP2(ppc_vaddcuw, u32, 4, (uint32_t)(((uint64_t)x + y) >> 32))
PPC_VOP2(ppc_vsububm, u8, 16, (uint8_t)(x - y))
PPC_VOP2(ppc_vsubuhm, u16, 8, (uint16_t)(x - y))
PPC_VOP2(ppc_vsubuwm, u32, 4, x - y)
PPC_VOP2(ppc_vsububs, u8, 16, ppc_sat<uint8_t>((int64_t)x - y))
PPC_VOP2(ppc_vsubuhs, u16, 8, ppc_sat<uint16_t>((int64_t)x - y))
PPC_VOP2(ppc_vsubuws, u32, 4, x > y ? x - y : 0u)
PPC_VOP2(ppc_vsubsbs, s8, 16, ppc_sat<int8_t>((int64_t)x - y))
PPC_VOP2(ppc_vsubshs, s16, 8, ppc_sat<int16_t>((int64_t)x - y))
PPC_VOP2(ppc_vsubsws, s32, 4, ppc_sat<int32_t>((int64_t)x - y))
PPC_VOP2(ppc_vsubcuw, u32, 4, x >= y ? 1u : 0u)
PPC_VOP2(ppc_vmaxub, u8, 16, std::max(x, y))
PPC_VOP2(ppc_vmaxuh, u16, 8, std::max(x, y))
PPC_VOP2(ppc_vmaxuw, u32, 4, std::max(x, y))
PPC_VOP2(ppc_vmaxsb, s8, 16, std::max(x, y))
PPC_VOP2(ppc_vmaxsh, s16, 8, std::max(x, y))
PPC_VOP2(ppc_vmaxsw, s32, 4, std::max(x, y))
PPC_VOP2(ppc_vminub, u8, 16, std::min(x, y))
PPC_VOP2(ppc_vminuh, u16, 8, std::min(x, y))
PPC_VOP2(ppc_vminuw, u32, 4, std::min(x, y))
PPC_VOP2(ppc_vminsb, s8, 16, std::min(x, y))
PPC_VOP2(ppc_vminsh, s16, 8, std::min(x, y))
PPC_VOP2(ppc_vminsw, s32, 4, std::min(x, y))
PPC_VOP2(ppc_vavgub, u8, 16, (uint8_t)(((uint32_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vavguh, u16, 8, (uint16_t)(((uint32_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vavguw, u32, 4, (uint32_t)(((uint64_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vavgsb, s8, 16, (int8_t)(((int32_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vavgsh, s16, 8, (int16_t)(((int32_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vavgsw, s32, 4, (int32_t)(((int64_t)x + y + 1) >> 1))
PPC_VOP2(ppc_vand, u64, 2, x & y)
PPC_VOP2(ppc_vandc, u64, 2, x & ~y)
PPC_VOP2(ppc_vor, u64, 2, x | y)
PPC_VOP2(ppc_vnor, u64, 2, ~(x | y))
PPC_VOP2(ppc_vxor, u64, 2, x ^ y)
PPC_VOP2(ppc_vslb, u8, 16, (uint8_t)(x << (y & 7)))
PPC_VOP2(ppc_vslh, u16, 8, (uint16_t)(x << (y & 15)))
PPC_VOP2(ppc_vslw, u32, 4, x << (y & 31))
PPC_VOP2(ppc_vsrb, u8, 16, (uint8_t)(x >> (y & 7)))
PPC_VOP2(ppc_vsrh, u16, 8, (uint16_t)(x >> (y & 15)))
PPC_VOP2(ppc_vsrw, u32, 4, x >> (y & 31))
PPC_VOP2(ppc_vsrab, s8, 16, (int8_t)(x >> (y & 7)))
PPC_VOP2(ppc_vsrah, s16, 8, (int16_t)(x >> (y & 15)))
PPC_VOP2(ppc_vsraw, s32, 4, x >> (y & 31))
PPC_VOP2(ppc_vrlb, u8, 16, std::rotl(x, (int)(y & 7)))
PPC_VOP2(ppc_vrlh, u16, 8, std::rotl(x, (int)(y & 15)))
PPC_VOP2(ppc_vrlw, u32, 4, std::rotl(x, (int)(y & 31)))
PPC_VOP2(ppc_vaddfp, f32, 4, x + y)
PPC_VOP2(ppc_vsubfp, f32, 4, x - y)
PPC_VOP2(ppc_vmulfp, f32, 4, x * y)
PPC_VOP2(ppc_vmaxfp, f32, 4, x > y ? x : y)
PPC_VOP2(ppc_vminfp, f32, 4, x < y ? x : y)
PPC_VOP1(ppc_vrefp, f32, 4, 1.0f / x)
PPC_VOP1(ppc_vrsqrtefp, f32, 4, 1.0f / std::sqrt(x))
PPC_VOP1(ppc_vexptefp, f32, 4, std::exp2(x))
PPC_VOP1(ppc_vlogefp, f32, 4, std::log2(x))
PPC_VOP1(ppc_vrfin, f32, 4, std::nearbyint(x))
PPC_VOP1(ppc_vrfiz, f32, 4, std::trunc(x))
PPC_VOP1(ppc_vrfip, f32, 4, std::ceil(x))
PPC_VOP1(ppc_vrfim, f32, 4, std::floor(x))
PPC_VCMP(ppc_vcmpequb, u8, 16, x == y)
PPC_VCMP(ppc_vcmpequh, u16, 8, x == y)
PPC_VCMP(ppc_vcmpequw, u32, 4, x == y)
PPC_VCMP(ppc_vcmpgtub, u8, 16, x > y)
PPC_VCMP(ppc_vcmpgtuh, u16, 8, x > y)
PPC_VCMP(ppc_vcmpgtuw, u32, 4, x > y)
PPC_VCMP(ppc_vcmpgtsb, s8, 16, x > y)
PPC_VCMP(ppc_vcmpgtsh, s16, 8, x > y)
PPC_VCMP(ppc_vcmpgtsw, s32, 4, x > y)

static inline void ppc_vcmpfp_mask(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, int kind) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    float x = a.f32[i], y = b.f32[i];
    bool c = kind == 0 ? x == y : kind == 1 ? x >= y : x > y;
    r.u32[i] = c ? 0xFFFFFFFFu : 0;
  }
  d = r;
}
static inline void ppc_vcmpeqfp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) { ppc_vcmpfp_mask(d, a, b, 0); }
static inline void ppc_vcmpgefp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) { ppc_vcmpfp_mask(d, a, b, 1); }
static inline void ppc_vcmpgtfp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) { ppc_vcmpfp_mask(d, a, b, 2); }
static inline void ppc_vcmpbfp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    float x = a.f32[i], y = b.f32[i];
    r.u32[i] = (!(x <= y) ? 0x80000000u : 0) | (!(x >= -y) ? 0x40000000u : 0);
  }
  d = r;
}
// CR6 após vcmp*.: lt = todos verdadeiros, eq = nenhum verdadeiro
static inline void ppc_vcmp_cr6(PPCCRField& c, const PPCVReg& r) {
  bool all = (r.u64[0] & r.u64[1]) == ~0ull, none = (r.u64[0] | r.u64[1]) == 0;
  c.lt = all; c.gt = 0; c.eq = none; c.so = 0;
}
static inline void ppc_vcmpb_cr6(PPCCRField& c, const PPCVReg& r) {
  c.lt = 0; c.gt = 0; c.eq = (r.u64[0] | r.u64[1]) == 0; c.so = 0;
}

static inline void ppc_vmaddfp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 4; i++) r.f32[i] = a.f32[i] * b.f32[i] + c.f32[i]; d = r;
}
static inline void ppc_vnmsubfp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 4; i++) r.f32[i] = -(a.f32[i] * b.f32[i] - c.f32[i]); d = r;
}
static inline void ppc_vmsum3fp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  float s = PPC_VF32(a, 0) * PPC_VF32(b, 0) + PPC_VF32(a, 1) * PPC_VF32(b, 1) + PPC_VF32(a, 2) * PPC_VF32(b, 2);
  for (int i = 0; i < 4; i++) d.f32[i] = s;
}
static inline void ppc_vmsum4fp(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  float s = a.f32[0] * b.f32[0] + a.f32[1] * b.f32[1] + a.f32[2] * b.f32[2] + a.f32[3] * b.f32[3];
  for (int i = 0; i < 4; i++) d.f32[i] = s;
}
static inline void ppc_vsel(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 2; i++) r.u64[i] = (a.u64[i] & ~c.u64[i]) | (b.u64[i] & c.u64[i]); d = r;
}

// Conversões
static inline void ppc_vcfsx(PPCVReg& d, const PPCVReg& b, unsigned uimm) {
  float s = std::ldexp(1.0f, -(int)uimm);
  PPCVReg r; for (int i = 0; i < 4; i++) r.f32[i] = (float)b.s32[i] * s; d = r;
}
static inline void ppc_vcfux(PPCVReg& d, const PPCVReg& b, unsigned uimm) {
  float s = std::ldexp(1.0f, -(int)uimm);
  PPCVReg r; for (int i = 0; i < 4; i++) r.f32[i] = (float)b.u32[i] * s; d = r;
}
static inline void ppc_vctsxs(PPCVReg& d, const PPCVReg& b, unsigned uimm) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    double v = std::trunc((double)b.f32[i] * std::ldexp(1.0, (int)uimm));
    r.s32[i] = std::isnan(v) ? 0 : v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)v;
  }
  d = r;
}
static inline void ppc_vctuxs(PPCVReg& d, const PPCVReg& b, unsigned uimm) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    double v = std::trunc((double)b.f32[i] * std::ldexp(1.0, (int)uimm));
    r.u32[i] = std::isnan(v) || v <= 0.0 ? 0u : v >= 4294967295.0 ? 0xFFFFFFFFu : (uint32_t)v;
  }
  d = r;
}

// Permutações (definidas na ordem BE)
static inline void ppc_vperm(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r;
  for (int i = 0; i < 16; i++) {
    unsigned s = PPC_VB8(c, i) & 0x1F;
    PPC_VB8(r, i) = s < 16 ? PPC_VB8(a, s) : PPC_VB8(b, s - 16);
  }
  d = r;
}
static inline void ppc_vsldoi(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, unsigned sh) {
  PPCVReg r;
  for (unsigned i = 0; i < 16; i++) {
    unsigned s = i + sh;
    PPC_VB8(r, i) = s < 16 ? PPC_VB8(a, s) : PPC_VB8(b, s - 16);
  }
  d = r;
}
static inline void ppc_vspltb(PPCVReg& d, const PPCVReg& b, unsigned i) { uint8_t v = PPC_VB8(b, i & 15); for (auto& x : d.u8) x = v; }
static inline void ppc_vsplth(PPCVReg& d, const PPCVReg& b, unsigned i) { uint16_t v = PPC_VH16(b, i & 7); for (auto& x : d.u16) x = v; }
static inline void ppc_vspltw(PPCVReg& d, const PPCVReg& b, unsigned i) { uint32_t v = PPC_VW32(b, i & 3); for (auto& x : d.u32) x = v; }
static inline void ppc_vspltisb(PPCVReg& d, int v) { for (auto& x : d.s8) x = (int8_t)v; }
static inline void ppc_vspltish(PPCVReg& d, int v) { for (auto& x : d.s16) x = (int16_t)v; }
static inline void ppc_vspltisw(PPCVReg& d, int v) { for (auto& x : d.s32) x = v; }

#define PPC_VMRG(name, ACC, N, OFF)                                             \
  static inline void name(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {     \
    PPCVReg r;                                                                  \
    for (int i = 0; i < N / 2; i++) { ACC(r, 2 * i) = ACC(a, i + OFF); ACC(r, 2 * i + 1) = ACC(b, i + OFF); } \
    d = r;                                                                      \
  }
PPC_VMRG(ppc_vmrghb, PPC_VB8, 16, 0)
PPC_VMRG(ppc_vmrglb, PPC_VB8, 16, 8)
PPC_VMRG(ppc_vmrghh, PPC_VH16, 8, 0)
PPC_VMRG(ppc_vmrglh, PPC_VH16, 8, 4)
PPC_VMRG(ppc_vmrghw, PPC_VW32, 4, 0)
PPC_VMRG(ppc_vmrglw, PPC_VW32, 4, 2)

// Pack: metade alta do resultado (BE) vem de A, metade baixa de B
#define PPC_VPK(name, SACC, STYPE, DACC, DTYPE, N, conv)                        \
  static inline void name(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {     \
    PPCVReg r;                                                                  \
    for (int i = 0; i < N; i++) {                                               \
      { int64_t x = (STYPE)SACC(a, i); DACC(r, i) = (DTYPE)(conv); }           \
      { int64_t x = (STYPE)SACC(b, i); DACC(r, i + N) = (DTYPE)(conv); }       \
    }                                                                           \
    d = r;                                                                      \
  }
PPC_VPK(ppc_vpkuhum, PPC_VH16, uint16_t, PPC_VB8, uint8_t, 8, x)
PPC_VPK(ppc_vpkuwum, PPC_VW32, uint32_t, PPC_VH16, uint16_t, 4, x)
PPC_VPK(ppc_vpkuhus, PPC_VH16, uint16_t, PPC_VB8, uint8_t, 8, ppc_sat<uint8_t>(x))
PPC_VPK(ppc_vpkuwus, PPC_VW32, uint32_t, PPC_VH16, uint16_t, 4, ppc_sat<uint16_t>(x))
PPC_VPK(ppc_vpkshss, PPC_VH16, int16_t, PPC_VB8, uint8_t, 8, ppc_sat<int8_t>(x))
PPC_VPK(ppc_vpkshus, PPC_VH16, int16_t, PPC_VB8, uint8_t, 8, ppc_sat<uint8_t>(x))
PPC_VPK(ppc_vpkswss, PPC_VW32, int32_t, PPC_VH16, uint16_t, 4, ppc_sat<int16_t>(x))
PPC_VPK(ppc_vpkswus, PPC_VW32, int32_t, PPC_VH16, uint16_t, 4, ppc_sat<uint16_t>(x))

static inline void ppc_vpkpx(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    uint32_t x = PPC_VW32(a, i), y = PPC_VW32(b, i);
    PPC_VH16(r, i) = (uint16_t)(((x >> 9) & 0x8000) | ((x >> 9) & 0x7C00) | ((x >> 6) & 0x3E0) | ((x >> 3) & 0x1F));
    PPC_VH16(r, i + 4) = (uint16_t)(((y >> 9) & 0x8000) | ((y >> 9) & 0x7C00) | ((y >> 6) & 0x3E0) | ((y >> 3) & 0x1F));
  }
  d = r;
}

#define PPC_VUPK(name, SACC, STYPE, DACC, DTYPE, N, OFF)                        \
  static inline void name(PPCVReg& d, const PPCVReg& b) {                       \
    PPCVReg r;                                                                  \
    for (int i = 0; i < N; i++) DACC(r, i) = (DTYPE)(STYPE)SACC(b, i + OFF);    \
    d = r;                                                                      \
  }
PPC_VUPK(ppc_vupkhsb, PPC_VB8, int8_t, PPC_VH16, uint16_t, 8, 0)
PPC_VUPK(ppc_vupklsb, PPC_VB8, int8_t, PPC_VH16, uint16_t, 8, 8)
PPC_VUPK(ppc_vupkhsh, PPC_VH16, int16_t, PPC_VW32, uint32_t, 4, 0)
PPC_VUPK(ppc_vupklsh, PPC_VH16, int16_t, PPC_VW32, uint32_t, 4, 4)

static inline void ppc_vupkpx(PPCVReg& d, const PPCVReg& b, int off) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    uint32_t p = PPC_VH16(b, i + off);
    PPC_VW32(r, i) = ((p & 0x8000) ? 0xFF000000u : 0) | ((p >> 10) & 0x1F) << 16 | ((p >> 5) & 0x1F) << 8 | (p & 0x1F);
  }
  d = r;
}

// Deslocamentos de 128 bits
static inline void ppc_vslo(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  unsigned sh = (PPC_VB8(b, 15) >> 3) & 15; PPCVReg r;
  for (unsigned i = 0; i < 16; i++) PPC_VB8(r, i) = i + sh < 16 ? PPC_VB8(a, i + sh) : 0;
  d = r;
}
static inline void ppc_vsro(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  unsigned sh = (PPC_VB8(b, 15) >> 3) & 15; PPCVReg r;
  for (unsigned i = 0; i < 16; i++) PPC_VB8(r, i) = i >= sh ? PPC_VB8(a, i - sh) : 0;
  d = r;
}
static inline void ppc_vsl(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  unsigned sh = PPC_VB8(b, 15) & 7; PPCVReg r;
  r.u64[1] = (a.u64[1] << sh) | (sh ? a.u64[0] >> (64 - sh) : 0);
  r.u64[0] = a.u64[0] << sh;
  d = r;
}
static inline void ppc_vsr(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  unsigned sh = PPC_VB8(b, 15) & 7; PPCVReg r;
  r.u64[0] = (a.u64[0] >> sh) | (sh ? a.u64[1] << (64 - sh) : 0);
  r.u64[1] = a.u64[1] >> sh;
  d = r;
}

// Multiplicações par/ímpar (elementos BE pares = 0,2,4...)
#define PPC_VMULEO(name, SACC, STYPE, DACC, DTYPE, N, ODD)                       \
  static inline void name(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {      \
    PPCVReg r;                                                                   \
    for (int i = 0; i < N; i++) DACC(r, i) = (DTYPE)((int64_t)(STYPE)SACC(a, 2 * i + ODD) * (STYPE)SACC(b, 2 * i + ODD)); \
    d = r;                                                                       \
  }
PPC_VMULEO(ppc_vmuleub, PPC_VB8, uint8_t, PPC_VH16, uint16_t, 8, 0)
PPC_VMULEO(ppc_vmuloub, PPC_VB8, uint8_t, PPC_VH16, uint16_t, 8, 1)
PPC_VMULEO(ppc_vmulesb, PPC_VB8, int8_t, PPC_VH16, uint16_t, 8, 0)
PPC_VMULEO(ppc_vmulosb, PPC_VB8, int8_t, PPC_VH16, uint16_t, 8, 1)
PPC_VMULEO(ppc_vmuleuh, PPC_VH16, uint16_t, PPC_VW32, uint32_t, 4, 0)
PPC_VMULEO(ppc_vmulouh, PPC_VH16, uint16_t, PPC_VW32, uint32_t, 4, 1)
PPC_VMULEO(ppc_vmulesh, PPC_VH16, int16_t, PPC_VW32, uint32_t, 4, 0)
PPC_VMULEO(ppc_vmulosh, PPC_VH16, int16_t, PPC_VW32, uint32_t, 4, 1)

// Somas horizontais / multiplica-soma inteiras
static inline void ppc_vsumsws(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  int64_t s = (int32_t)PPC_VW32(b, 3);
  for (int i = 0; i < 4; i++) s += (int32_t)PPC_VW32(a, i);
  PPCVReg r{}; PPC_VW32(r, 3) = (uint32_t)ppc_sat<int32_t>(s); d = r;
}
static inline void ppc_vsum2sws(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r{};
  for (int j = 0; j < 2; j++) {
    int64_t s = (int64_t)(int32_t)PPC_VW32(a, 2 * j) + (int32_t)PPC_VW32(a, 2 * j + 1) + (int32_t)PPC_VW32(b, 2 * j + 1);
    PPC_VW32(r, 2 * j + 1) = (uint32_t)ppc_sat<int32_t>(s);
  }
  d = r;
}
static inline void ppc_vsum4ubs(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    uint64_t s = PPC_VW32(b, i);
    for (int j = 0; j < 4; j++) s += PPC_VB8(a, 4 * i + j);
    PPC_VW32(r, i) = (uint32_t)std::min<uint64_t>(s, 0xFFFFFFFFull);
  }
  d = r;
}
static inline void ppc_vsum4sbs(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    int64_t s = (int32_t)PPC_VW32(b, i);
    for (int j = 0; j < 4; j++) s += (int8_t)PPC_VB8(a, 4 * i + j);
    PPC_VW32(r, i) = (uint32_t)ppc_sat<int32_t>(s);
  }
  d = r;
}
static inline void ppc_vsum4shs(PPCVReg& d, const PPCVReg& a, const PPCVReg& b) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    int64_t s = (int32_t)PPC_VW32(b, i) + (int64_t)(int16_t)PPC_VH16(a, 2 * i) + (int16_t)PPC_VH16(a, 2 * i + 1);
    PPC_VW32(r, i) = (uint32_t)ppc_sat<int32_t>(s);
  }
  d = r;
}
static inline void ppc_vmsumshm(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    int64_t s = (int32_t)PPC_VW32(c, i);
    for (int j = 0; j < 2; j++) s += (int64_t)(int16_t)PPC_VH16(a, 2 * i + j) * (int16_t)PPC_VH16(b, 2 * i + j);
    PPC_VW32(r, i) = (uint32_t)s;
  }
  d = r;
}
static inline void ppc_vmsumuhm(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    uint64_t s = PPC_VW32(c, i);
    for (int j = 0; j < 2; j++) s += (uint64_t)PPC_VH16(a, 2 * i + j) * PPC_VH16(b, 2 * i + j);
    PPC_VW32(r, i) = (uint32_t)s;
  }
  d = r;
}
static inline void ppc_vmsumubm(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) {
    uint64_t s = PPC_VW32(c, i);
    for (int j = 0; j < 4; j++) s += (uint64_t)PPC_VB8(a, 4 * i + j) * PPC_VB8(b, 4 * i + j);
    PPC_VW32(r, i) = (uint32_t)s;
  }
  d = r;
}
static inline void ppc_vmladduhm(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 8; i++) r.u16[i] = (uint16_t)(a.u16[i] * b.u16[i] + c.u16[i]); d = r;
}
static inline void ppc_vmhaddshs(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 8; i++) r.s16[i] = ppc_sat<int16_t>((((int32_t)a.s16[i] * b.s16[i]) >> 15) + c.s16[i]); d = r;
}
static inline void ppc_vmhraddshs(PPCVReg& d, const PPCVReg& a, const PPCVReg& b, const PPCVReg& c) {
  PPCVReg r; for (int i = 0; i < 8; i++) r.s16[i] = ppc_sat<int16_t>((((int32_t)a.s16[i] * b.s16[i] + 0x4000) >> 15) + c.s16[i]); d = r;
}

// ---- Extensões VMX128 ----
// vpermwi128: palavra i do resultado = vB[(PERM >> (6 - 2i)) & 3]
static inline void ppc_vpermwi(PPCVReg& d, const PPCVReg& b, unsigned perm) {
  PPCVReg r;
  for (int i = 0; i < 4; i++) PPC_VW32(r, i) = PPC_VW32(b, (perm >> (6 - 2 * i)) & 3);
  d = r;
}
// vrlimi128: roda vB z palavras para a esquerda e insere em vD as palavras marcadas em IMM (bit 3 = x)
static inline void ppc_vrlimi(PPCVReg& d, const PPCVReg& b, unsigned mask, unsigned rot) {
  PPCVReg t;
  for (unsigned i = 0; i < 4; i++) PPC_VW32(t, i) = PPC_VW32(b, (i + rot) & 3);
  for (unsigned i = 0; i < 4; i++)
    if (mask & (8u >> i)) PPC_VW32(d, i) = PPC_VW32(t, i);
}

static inline float ppc_half_to_float(uint16_t h) {
  uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 0) {
    if (m == 0) return std::bit_cast<float>(s);
    float f = std::ldexp((float)m, -24);
    return s ? -f : f;
  }
  // Xenos: expoente 31 não é Inf/NaN (formato "half" estendido do D3D do X360)
  return std::bit_cast<float>(s | ((e + 112) << 23) | (m << 13));
}
static inline uint16_t ppc_float_to_half(float f) {
  uint32_t x = std::bit_cast<uint32_t>(f), s = (x >> 16) & 0x8000;
  int32_t e = (int32_t)((x >> 23) & 0xFF) - 112;
  uint32_t m = x & 0x7FFFFF;
  if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(s | 0x7FFF);  // satura
  if (e >= 32) return (uint16_t)(s | 0x7FFF);
  if (e <= 0) {
    if (e < -10) return (uint16_t)s;
    m |= 0x800000;
    return (uint16_t)(s | (m >> (14 - e)));
  }
  return (uint16_t)(s | (uint32_t)e << 10 | (m >> 13));
}

// vupkd3d128: tipos D3D do X360 (IMM >> 2). Resultado em formato "3.0 + x*2^-22" / float.
static inline void ppc_vupkd3d(PPCVReg& d, const PPCVReg& b, unsigned type, PPCContext& ctx, uint8_t* mem_base, uint32_t addr, uint32_t insn) {
  PPCVReg r{};
  switch (type) {
    case 0: {  // D3DCOLOR: ARGB (palavra w) -> RGBA em 1.0 + c*2^-22
      uint32_t c = PPC_VW32(b, 3);
      PPC_VW32(r, 0) = 0x3F800000u | ((c >> 16) & 0xFF);
      PPC_VW32(r, 1) = 0x3F800000u | ((c >> 8) & 0xFF);
      PPC_VW32(r, 2) = 0x3F800000u | (c & 0xFF);
      PPC_VW32(r, 3) = 0x3F800000u | (c >> 24);
      break;
    }
    case 1: {  // NORMSHORT2
      uint32_t c = PPC_VW32(b, 3);
      PPC_VW32(r, 0) = 0x40400000u + (uint32_t)(int32_t)(int16_t)(c >> 16);
      PPC_VW32(r, 1) = 0x40400000u + (uint32_t)(int32_t)(int16_t)c;
      PPC_VF32(r, 2) = 0.0f; PPC_VF32(r, 3) = 1.0f;
      break;
    }
    case 2: {  // NORMPACKED32 (2_10_10_10)
      uint32_t c = PPC_VW32(b, 3);
      auto s10 = [](uint32_t v) { return (uint32_t)(((int32_t)(v << 22)) >> 22); };
      PPC_VW32(r, 0) = 0x40400000u + s10(c);
      PPC_VW32(r, 1) = 0x40400000u + s10(c >> 10);
      PPC_VW32(r, 2) = 0x40400000u + s10(c >> 20);
      PPC_VW32(r, 3) = 0x40400000u + (c >> 30);
      break;
    }
    case 3: {  // FLOAT16_2
      uint32_t c = PPC_VW32(b, 3);
      PPC_VF32(r, 0) = ppc_half_to_float((uint16_t)(c >> 16));
      PPC_VF32(r, 1) = ppc_half_to_float((uint16_t)c);
      PPC_VF32(r, 2) = 0.0f; PPC_VF32(r, 3) = 1.0f;
      break;
    }
    case 4: {  // NORMSHORT4
      for (int i = 0; i < 4; i++) PPC_VW32(r, i) = 0x40400000u + (uint32_t)(int32_t)(int16_t)PPC_VH16(b, 4 + i);
      break;
    }
    case 5: {  // FLOAT16_4
      for (int i = 0; i < 4; i++) PPC_VF32(r, i) = ppc_half_to_float(PPC_VH16(b, 4 + i));
      break;
    }
    default: ppc_unimplemented(ctx, mem_base, addr, insn); return;
  }
  d = r;
}

// vpkd3d128: empacota vB conforme `type` (IMM>>2) e insere em vD conforme pack (IMM&3) e shift (z).
// Modelo: o valor empacotado (32 ou 64 bits) entra na(s) palavra(s) mais à direita deslocada(s)
// `shift` palavras para a esquerda. A confirmar contra hardware/Xenia na fase 3.
static inline void ppc_vpkd3d(PPCVReg& d, const PPCVReg& b, unsigned type, unsigned pack, unsigned shift,
                              PPCContext& ctx, uint8_t* mem_base, uint32_t addr, uint32_t insn) {
  auto clampf = [](float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; };
  uint32_t p[2] = {0, 0}; int words = 1;
  switch (type) {
    case 0: {  // D3DCOLOR a partir de 1.0+c*2^-22 (RGBA) -> ARGB
      uint32_t c[4];
      for (int i = 0; i < 4; i++) {
        float v = clampf(PPC_VF32(b, i), 1.0f, std::bit_cast<float>(0x3F8000FFu));
        c[i] = std::bit_cast<uint32_t>(v) & 0xFF;
      }
      p[0] = c[3] << 24 | c[0] << 16 | c[1] << 8 | c[2];
      break;
    }
    case 1: {  // NORMSHORT2
      for (int i = 0; i < 2; i++) {
        float v = clampf(PPC_VF32(b, i), std::bit_cast<float>(0x403F8001u), std::bit_cast<float>(0x40407FFFu));
        p[0] |= (std::bit_cast<uint32_t>(v) & 0xFFFF) << (16 - 16 * i);
      }
      break;
    }
    case 3: {  // FLOAT16_2
      p[0] = (uint32_t)ppc_float_to_half(PPC_VF32(b, 0)) << 16 | ppc_float_to_half(PPC_VF32(b, 1));
      break;
    }
    case 5: {  // FLOAT16_4
      words = 2;
      p[0] = (uint32_t)ppc_float_to_half(PPC_VF32(b, 0)) << 16 | ppc_float_to_half(PPC_VF32(b, 1));
      p[1] = (uint32_t)ppc_float_to_half(PPC_VF32(b, 2)) << 16 | ppc_float_to_half(PPC_VF32(b, 3));
      break;
    }
    default: ppc_unimplemented(ctx, mem_base, addr, insn); return;
  }
  (void)pack;
  int last = 3 - (int)shift;
  if (words == 1) { if (last >= 0) PPC_VW32(d, last) = p[0]; }
  else {
    if (last >= 1) { PPC_VW32(d, last - 1) = p[0]; PPC_VW32(d, last) = p[1]; }
    else PPC_VW32(d, 0) = p[1];
  }
}
