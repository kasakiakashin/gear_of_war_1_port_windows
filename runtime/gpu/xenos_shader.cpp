#include "gpu/xenos_shader.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <set>

namespace gpu {
namespace {

uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((n >= 32) ? ~0u : ((1u << n) - 1)); }

std::string fmt(const char* f, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, f);
  int n = std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  if (n < (int)sizeof buf) return std::string(buf, n > 0 ? n : 0);
  std::string s((size_t)n + 1, '\0');
  va_start(ap, f);
  std::vsnprintf(s.data(), s.size(), f, ap);
  va_end(ap);
  s.resize((size_t)n);
  return s;
}

const char kComp[] = "xyzw";

// Prelúdio comum aos dois estágios (ver o cabeçalho para os registos).
const char kPrelude[] = R"(
cbuffer XeConsts : register(b0) { float4 c[256]; };
cbuffer XeFetch : register(b1) { uint4 xe_fetch[48]; };
cbuffer XeBools : register(b2) { uint4 xe_bools[2]; uint4 xe_loops[8]; };
cbuffer XeSystem : register(b3) {
  float4 xe_vp_scale;
  float4 xe_vp_offset;
  float4 xe_rt;          // x = 2/largura, y = 2/altura, zw = deslocamento da janela
  uint4 xe_flags;        // x = PA_CL_VTE_CNTL, y = alpha test (func | enable << 3), z = param_gen (reg | 0x100), w = VGT_INDX_OFFSET
  float4 xe_alpha_ref;
  uint4 xe_vb_size[2];
  float4 xe_tex_size[32];
  float4 xe_rt_scale;    // 2^color_exp_bias por RT
  float4 xe_res;         // x = y = lógico/host (1/ResolutionScale): os shaders veem sempre coordenadas 1280×720
};
uint xe_fetch_dw(uint i) { return xe_fetch[i >> 2][i & 3]; }
uint xe_swap(uint v, uint e) {
  if (e == 1) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
  if (e == 2) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
  if (e == 3) return (v << 16) | (v >> 16);
  return v;
}
bool xe_bool(uint b) { return ((xe_bools[b >> 7][(b >> 5) & 3] >> (b & 31)) & 1u) != 0; }
uint xe_loop(uint i) { return xe_loops[i >> 2][i & 3]; }
// Conversão de um componente de vfetch: fl bit0 = com sinal, bit1 = inteiro, bit2 = fração sem zero (2v+1)/(2^n-1).
float xe_conv(uint v, uint n, uint fl) {
  if (fl & 1u) {
    int s = (int)(v << (32 - n)) >> (32 - n);
    if (fl & 2u) return (float)s;
    if (fl & 4u) return ((float)s * 2.0 + 1.0) / (float)((1u << n) - 1u);
    return max((float)s / (float)((1u << (n - 1)) - 1u), -1.0);
  }
  if (fl & 2u) return (float)v;
  return (float)v / (float)((1u << n) - 1u);
}
float4 xe_tswz(float4 t, uint d3) {
  uint sw = (d3 >> 1) & 0xFFFu;
  float4 o;
  [unroll] for (uint i = 0; i < 4; i++) {
    uint s = (sw >> (3 * i)) & 7u;
    o[i] = s < 4 ? t[s] : (s == 5 ? 1.0 : 0.0);
  }
  return o;
}
uint4 xe_set4u(uint4 v, int i, uint x) { return uint4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
int4 xe_set4i(int4 v, int i, int x) { return int4(i == 0 ? x : v.x, i == 1 ? x : v.y, i == 2 ? x : v.z, i == 3 ? x : v.w); }
float xe_rcp(float a) { return 1.0 / a; }
float xe_rcpc(float a) { float r = 1.0 / a; return clamp(r, -3.402823466e+38, 3.402823466e+38); }
float xe_rcpf(float a) { float r = 1.0 / a; return isinf(r) ? 0.0 : r; }
float xe_rsq(float a) { return rsqrt(a); }
float xe_rsqc(float a) { float r = rsqrt(a); return clamp(r, -3.402823466e+38, 3.402823466e+38); }
float xe_rsqf(float a) { float r = rsqrt(a); return isinf(r) ? 0.0 : r; }
float xe_logc(float a) { float r = log2(a); return isinf(r) ? -3.402823466e+38 : r; }
float4 xe_cube(float4 a, float4 b) {
  float3 v = float3(a.z, a.w, a.x);
  float3 av = abs(v);
  float ma, sc, tc, face;
  if (av.z >= av.x && av.z >= av.y) { ma = v.z; face = v.z < 0 ? 5 : 4; sc = v.z < 0 ? -v.x : v.x; tc = -v.y; }
  else if (av.y >= av.x) { ma = v.y; face = v.y < 0 ? 3 : 2; sc = v.x; tc = v.y < 0 ? -v.z : v.z; }
  else { ma = v.x; face = v.x < 0 ? 1 : 0; sc = v.x < 0 ? v.z : -v.z; tc = -v.y; }
  return float4(tc, sc, 2.0 * ma, face);
}
// tfetch de cube map: o shader passa (sc/(2|ma|) + 1.5, tc/(2|ma|) + 1.5, face) — saída do CUBE com .yx e um MAD.
// Reconstrói a direção 3D (inverso do xe_cube, convenção de faces do D3D) para um TextureCube.
float3 xe_cube_dir(float3 c) {
  float sc = (c.x - 1.5) * 2.0, tc = (c.y - 1.5) * 2.0;
  uint f = (uint)(c.z + 0.5);
  if (f == 0u) return float3(1.0, -tc, -sc);
  if (f == 1u) return float3(-1.0, -tc, sc);
  if (f == 2u) return float3(sc, 1.0, tc);
  if (f == 3u) return float3(sc, -1.0, -tc);
  if (f == 4u) return float3(sc, -tc, 1.0);
  return float3(-sc, -tc, -1.0);
}
)";

struct Cf {
  uint32_t lo, hi;
  uint32_t op() const { return bits(hi, 12, 4); }
};

enum CfOp { kNop = 0, kExec, kExecEnd, kCondExec, kCondExecEnd, kCondExecPred, kCondExecPredEnd, kLoopStart,
            kLoopEnd, kCondCall, kReturn, kCondJmp, kAlloc, kCondExecPredClean, kCondExecPredCleanEnd,
            kMarkVsFetchDone };

class Translator {
 public:
  Translator(const std::vector<uint32_t>& u, bool pixel) : u_(u), pixel_(pixel) {}

  TranslatedShader run() {
    TranslatedShader out;
    out.pixel = pixel_;
    out.hash = hash_words(u_.data(), u_.size());
    if (!parse_cf()) {
      out.error = error_;
      return out;
    }
    std::string body;
    linear_ = is_linear();
    for (size_t i = 0; i < cfs_.size(); i++) body += emit_cf((uint32_t)i);
    out.hlsl = assemble(body);
    out.ok = error_.empty();
    out.error = error_;
    out.vfetch_slots = vslots_;
    out.texture_mask = tex_mask_;
    out.cube_mask = cube_mask_;
    out.color_exports = color_exports_;
    out.writes_depth = writes_depth_;
    out.alu_count = alu_;
    out.fetch_count = fetch_;
    out.cf_count = (uint32_t)cfs_.size();
    return out;
  }

 private:
  const std::vector<uint32_t>& u_;
  bool pixel_;
  std::vector<Cf> cfs_;
  std::string error_;
  std::vector<uint32_t> vslots_;
  std::map<uint32_t, int> vslot_reg_;
  uint32_t tex_mask_ = 0, cube_mask_ = 0, color_exports_ = 0, alu_ = 0, fetch_ = 0;
  bool writes_depth_ = false;
  bool linear_ = false;  // sem loops/calls/saltos para trás: blocos em sequência guardados por `resume`
  // Último vfetch completo (os mini-fetches reutilizam o índice e o passo).
  std::string last_index_;
  uint32_t last_slot_ = 0, last_stride_ = 0;

  bool parse_cf() {
    uint32_t limit = (uint32_t)u_.size();  // em dwords; encolhe para o 1º endereço de EXEC
    for (uint32_t d = 0; d + 3 <= u_.size() && d + 3 <= limit; d += 3) {
      Cf a{u_[d], u_[d + 1] & 0xFFFF}, b{(u_[d + 1] >> 16) | (u_[d + 2] << 16), u_[d + 2] >> 16};
      for (const Cf& cf : {a, b}) {
        cfs_.push_back(cf);
        uint32_t op = cf.op();
        if (op == kExec || op == kExecEnd || (op >= kCondExec && op <= kCondExecPredEnd) ||
            op == kCondExecPredClean || op == kCondExecPredCleanEnd) {
          uint32_t addr = bits(cf.lo, 0, 12), count = bits(cf.lo, 12, 3);
          if (count && addr * 3 < limit) limit = addr * 3;
        }
      }
      if (cfs_.size() > 4096) break;
    }
    if (cfs_.empty()) error_ = "sem instruções de fluxo de controle";
    return error_.empty();
  }

  // ---- Operandos ALU ----
  std::string swz_rel(uint32_t sw) {
    std::string s = ".";
    for (int i = 0; i < 4; i++) s += kComp[((sw >> (2 * i)) + i) & 3];
    return s;
  }

  // Constante: o 1º operando constante usa const_0_rel_abs, os seguintes const_1_rel_abs.
  std::string const_ref(uint32_t index, bool addressed, bool a0_relative) {
    if (!addressed) return fmt("c[%u]", index & 255);
    return fmt("c[(%u + %s) & 255]", index & 255, a0_relative ? "a0" : "aL");
  }

  std::string src(uint32_t w0, uint32_t w1, uint32_t w2, int i, int const_ordinal) {
    // i = 1..3
    uint32_t reg = bits(w2, 24 - 8 * i, 8);   // src1 = bits 16-23, src2 = 8-15, src3 = 0-7
    uint32_t sw = bits(w1, 24 - 8 * i, 8);
    bool neg = bits(w1, 27 - i, 1);           // src1 26, src2 25, src3 24
    bool temp = bits(w2, 32 - i, 1);          // src1 31, src2 30, src3 29
    std::string v;
    bool abs = false;
    if (temp) {
      v = (reg & 0x40) ? fmt("r[(%u + aL) & 63]", reg & 0x3F) : fmt("r[%u]", reg & 0x3F);
      abs = reg & 0x80;
    } else {
      bool addressed = const_ordinal == 0 ? bits(w1, 31, 1) : bits(w1, 30, 1);
      v = const_ref(reg, addressed, bits(w1, 29, 1));
      abs = bits(w0, 7, 1);
    }
    v += swz_rel(sw);
    if (abs) v = "abs(" + v + ")";
    if (neg) v = "(-" + v + ")";
    return v;
  }

  // Escrita parcial por leitura-modificação-escrita do float4 inteiro: o FXC não aceita escrever componentes de
  // elementos de arrays indexáveis dentro do ciclo do fluxo de controle ("l-value expected").
  std::string rmw(const std::string& target, const std::string& assigns) {
    return fmt("      { float4 t_ = %s;%s %s = t_; }\n", target.c_str(), assigns.c_str(), target.c_str());
  }

  std::string dest_reg(uint32_t idx, bool rel) {
    return rel ? fmt("r[(%u + aL) & 63]", idx) : fmt("r[%u]", idx);
  }

  // Registrador de export → expressão HLSL (vazio se ignorado: memexport, etc.).
  std::string export_reg(uint32_t idx) {
    if (pixel_) {
      if (idx < 4) {
        color_exports_ |= 1u << idx;
        return fmt("oC[%u]", idx);
      }
      if (idx == 61) {
        writes_depth_ = true;
        return "oDepth";
      }
      return "";
    }
    if (idx < 16) return fmt("o.o[%u]", idx);
    if (idx == 62) return "o.pos";
    if (idx == 63) return "o.misc";
    return "";
  }

  std::string emit_alu(const uint32_t* w) {
    alu_++;
    uint32_t w0 = w[0], w1 = w[1], w2 = w[2];
    uint32_t vop = bits(w2, 24, 5), sop = bits(w0, 26, 6);
    uint32_t vmask = bits(w0, 16, 4), smask = bits(w0, 20, 4);
    bool exporting = bits(w0, 15, 1);
    std::string s;
    // Ordinais de constante para o endereçamento (o operando N só consome um ordinal se for constante).
    bool t1 = bits(w2, 31, 1), t2 = bits(w2, 30, 1);
    int ord2 = t1 ? 0 : 1, ord3 = (t1 && t2) ? 0 : 1;
    std::string a = src(w0, w1, w2, 1, 0), b = src(w0, w1, w2, 2, ord2), c3 = src(w0, w1, w2, 3, ord3);
    s += fmt("    { float4 s1 = %s, s2 = %s, s3 = %s;\n", a.c_str(), b.c_str(), c3.c_str());
    // Vetor
    std::string v;
    switch (vop) {
      case 0: v = "s1 + s2"; break;
      case 1: v = "s1 * s2"; break;
      case 2: v = "max(s1, s2)"; break;
      case 3: v = "min(s1, s2)"; break;
      case 4: v = "float4(s1 == s2)"; break;
      case 5: v = "float4(s1 > s2)"; break;
      case 6: v = "float4(s1 >= s2)"; break;
      case 7: v = "float4(s1 != s2)"; break;
      case 8: v = "frac(s1)"; break;
      case 9: v = "trunc(s1)"; break;
      case 10: v = "floor(s1)"; break;
      case 11: v = "s1 * s2 + s3"; break;
      case 12: v = "(s1 == 0.0 ? s2 : s3)"; break;
      case 13: v = "(s1 >= 0.0 ? s2 : s3)"; break;
      case 14: v = "(s1 > 0.0 ? s2 : s3)"; break;
      case 15: v = "dot(s1, s2).xxxx"; break;
      case 16: v = "dot(s1.xyz, s2.xyz).xxxx"; break;
      case 17: v = "(s1.x * s2.x + s1.y * s2.y + s3.x).xxxx"; break;
      case 18: v = "xe_cube(s1, s2)"; break;
      case 19: v = "max(max(s1.x, s1.y), max(s1.z, s1.w)).xxxx"; break;
      case 20: case 21: case 22: case 23: {
        static const char* cond[] = {"s1.w == 0.0", "s1.w != 0.0", "s1.w > 0.0", "s1.w >= 0.0"};
        s += fmt("      bool vp = (%s) && s2.w == 0.0; p0 = vp;\n", cond[vop - 20]);
        v = "(vp ? 0.0 : s1.x + 1.0).xxxx";
        break;
      }
      case 24: case 25: case 26: case 27: {
        static const char* cmp[] = {"==", ">", ">=", "!="};
        s += fmt("      bool vk = any(s1 %s s2);\n", cmp[vop - 24]);
        if (pixel_) s += "      if (vk) discard;\n";
        v = "(vk ? 1.0 : 0.0).xxxx";
        break;
      }
      case 28: v = "float4(1.0, s1.y * s2.y, s1.z, s2.w)"; break;
      case 29:
        s += "      a0 = (int)clamp(floor(s1.w + 0.5), -256.0, 255.0);\n";
        v = "max(s1, s2)";
        break;
      default:
        error_ = fmt("opcode vetorial %u", vop);
        v = "s1";
    }
    s += fmt("      float4 vr = %s;\n", v.c_str());
    // Escalar: a = s3.w, b = s3.x (componentes relativos 3 e 0 da swizzle do src3)
    std::string sc;
    bool scalar_writes = true;
    switch (sop) {
      case 0: sc = "s3.w + s3.x"; break;
      case 1: sc = "s3.w + ps"; break;
      case 2: sc = "s3.w * s3.x"; break;
      case 3: sc = "s3.w * ps"; break;
      case 4: sc = "((isinf(ps) || isnan(ps) || ps == -3.402823466e+38 || isinf(s3.x) || isnan(s3.x) || s3.x <= 0.0) ? "
                   "-3.402823466e+38 : s3.w * ps)"; break;
      case 5: sc = "max(s3.w, s3.x)"; break;
      case 6: sc = "min(s3.w, s3.x)"; break;
      case 7: sc = "(s3.w == 0.0 ? 1.0 : 0.0)"; break;
      case 8: sc = "(s3.w > 0.0 ? 1.0 : 0.0)"; break;
      case 9: sc = "(s3.w >= 0.0 ? 1.0 : 0.0)"; break;
      case 10: sc = "(s3.w != 0.0 ? 1.0 : 0.0)"; break;
      case 11: sc = "frac(s3.w)"; break;
      case 12: sc = "trunc(s3.w)"; break;
      case 13: sc = "floor(s3.w)"; break;
      case 14: sc = "exp2(s3.w)"; break;
      case 15: sc = "xe_logc(s3.w)"; break;
      case 16: sc = "log2(s3.w)"; break;
      case 17: sc = "xe_rcpc(s3.w)"; break;
      case 18: sc = "xe_rcpf(s3.w)"; break;
      case 19: sc = "xe_rcp(s3.w)"; break;
      case 20: sc = "xe_rsqc(s3.w)"; break;
      case 21: sc = "xe_rsqf(s3.w)"; break;
      case 22: sc = "xe_rsq(s3.w)"; break;
      case 23:
        s += "      a0 = (int)clamp(floor(s3.w + 0.5), -256.0, 255.0);\n";
        sc = "max(s3.w, s3.x)";
        break;
      case 24:
        s += "      a0 = (int)clamp(floor(s3.w), -256.0, 255.0);\n";
        sc = "max(s3.w, s3.x)";
        break;
      case 25: sc = "s3.w - s3.x"; break;
      case 26: sc = "s3.w - ps"; break;
      case 27: s += "      p0 = s3.w == 0.0;\n"; sc = "(p0 ? 0.0 : 1.0)"; break;
      case 28: s += "      p0 = s3.w != 0.0;\n"; sc = "(p0 ? 0.0 : 1.0)"; break;
      case 29: s += "      p0 = s3.w > 0.0;\n"; sc = "(p0 ? 0.0 : 1.0)"; break;
      case 30: s += "      p0 = s3.w >= 0.0;\n"; sc = "(p0 ? 0.0 : 1.0)"; break;
      case 31: s += "      p0 = s3.w == 1.0;\n"; sc = "(p0 ? 0.0 : (s3.w == 0.0 ? 1.0 : s3.w))"; break;
      case 32: s += "      p0 = s3.w - 1.0 <= 0.0;\n"; sc = "(p0 ? 0.0 : s3.w - 1.0)"; break;
      case 33: s += "      p0 = false;\n"; sc = "3.402823466e+38"; break;
      case 34: s += "      p0 = s3.w == 0.0;\n"; sc = "s3.w"; break;
      case 35: case 36: case 37: case 38: case 39: {
        static const char* cnd[] = {"s3.w == 0.0", "s3.w > 0.0", "s3.w >= 0.0", "s3.w != 0.0", "s3.w == 1.0"};
        s += fmt("      bool sk = %s;\n", cnd[sop - 35]);
        if (pixel_) s += "      if (sk) discard;\n";
        sc = "(sk ? 1.0 : 0.0)";
        break;
      }
      case 40: sc = "sqrt(s3.w)"; break;
      case 42: case 43: case 44: case 45: case 46: case 47: {
        // Constante (componente a) e temporário (componente b) com índice codificado no próprio opcode.
        uint32_t treg = (sop & 1) | (bits(w2, 29, 1) << 1) | (bits(w1, 0, 8) & 0x3C);
        uint32_t sw = bits(w1, 0, 8);
        bool neg = bits(w1, 24, 1), abs = bits(w0, 7, 1);
        std::string cst = const_ref(bits(w2, 0, 8), bits(w1, 30, 1), bits(w1, 29, 1));
        std::string ca = fmt("%s.%c", cst.c_str(), kComp[((sw >> 6) + 3) & 3]);
        std::string tb = fmt("r[%u].%c", treg, kComp[sw & 3]);
        if (abs) ca = "abs(" + ca + ")", tb = "abs(" + tb + ")";
        if (neg) ca = "(-" + ca + ")", tb = "(-" + tb + ")";
        const char* opr = sop < 44 ? "*" : sop < 46 ? "+" : "-";
        sc = fmt("(%s %s %s)", ca.c_str(), opr, tb.c_str());
        break;
      }
      case 48: sc = "sin(s3.w)"; break;
      case 49: sc = "cos(s3.w)"; break;
      case 50: sc = "ps"; break;
      default:
        error_ = fmt("opcode escalar %u", sop);
        sc = "0.0";
        scalar_writes = false;
    }
    if (bits(w0, 25, 1)) sc = "saturate(" + sc + ")";
    if (bits(w0, 24, 1)) s += "      vr = saturate(vr);\n";
    s += fmt("      float sr = %s; ps = sr;\n", sc.c_str());
    // Escritas
    if (exporting) {
      std::string e = export_reg(bits(w0, 0, 6));
      if (!e.empty()) {
        std::string as;
        for (int i = 0; i < 4; i++) {
          bool vb = (vmask >> i) & 1, sb = (smask >> i) & 1;
          if (vb && sb) as += fmt(" t_.%c = 1.0;", kComp[i]);
          else if (vb) as += fmt(" t_.%c = vr.%c;", kComp[i], kComp[i]);
          else if (sb) as += fmt(" t_.%c = sr;", kComp[i]);
          else if (bits(w0, 14, 1)) as += fmt(" t_.%c = 0.0;", kComp[i]);
        }
        if (!as.empty()) s += rmw(e, as);
      }
    } else {
      if (vmask) {
        std::string m;
        for (int i = 0; i < 4; i++)
          if ((vmask >> i) & 1) m += kComp[i];
        s += rmw(dest_reg(bits(w0, 0, 6), bits(w0, 6, 1)), fmt(" t_.%s = vr.%s;", m.c_str(), m.c_str()));
      }
      if (smask && scalar_writes) {
        std::string m;
        for (int i = 0; i < 4; i++)
          if ((smask >> i) & 1) m += kComp[i];
        s += rmw(dest_reg(bits(w0, 8, 6), bits(w0, 14, 1)),
                 fmt(" t_.%s = sr.%s;", m.c_str(), std::string(m.size(), 'x').c_str()));
      }
    }
    s += "    }\n";
    if (bits(w1, 28, 1)) s = fmt("    if (p0 == %s) {\n", bits(w1, 27, 1) ? "true" : "false") + s + "    }\n";
    return s;
  }

  // ---- Fetch ----
  std::string write_fetch_dest(uint32_t dst, bool rel, uint32_t dswz, const char* value) {
    std::string as;
    for (int i = 0; i < 4; i++) {
      uint32_t c = (dswz >> (3 * i)) & 7;
      if (c == 7) continue;
      if (c < 4) as += fmt(" t_.%c = %s.%c;", kComp[i], value, kComp[c]);
      else as += fmt(" t_.%c = %s;", kComp[i], c == 5 ? "1.0" : "0.0");
    }
    return as.empty() ? std::string() : rmw(dest_reg(dst, rel), as);
  }

  int vb_register(uint32_t slot) {
    auto it = vslot_reg_.find(slot);
    if (it != vslot_reg_.end()) return it->second;
    int r = (int)vslots_.size();
    vslots_.push_back(slot);
    vslot_reg_[slot] = r;
    return r;
  }

  std::string emit_vfetch(const uint32_t* w) {
    uint32_t d0 = w[0], d1 = w[1], d2 = w[2];
    bool mini = bits(d1, 30, 1);
    uint32_t dst = bits(d0, 12, 6), fmt_ = bits(d1, 16, 6);
    uint32_t offset = bits(d2, 8, 23), stride = bits(d2, 0, 8);
    int exp_adj = (int)(bits(d1, 24, 6) << 26) >> 26;
    uint32_t fl = (bits(d1, 12, 1) ? 1u : 0u) | (bits(d1, 13, 1) ? 2u : 0u) | (bits(d1, 14, 1) ? 4u : 0u);
    std::string s = "    {\n";
    if (!mini || last_index_.empty()) {
      uint32_t slot = bits(d0, 20, 5) * 3 + bits(d0, 25, 2);
      std::string idx = fmt("r[%u].%c", bits(d0, 5, 6), kComp[bits(d0, 30, 2)]);
      last_index_ = bits(d1, 15, 1) ? "(int)round(" + idx + ")" : "(int)floor(" + idx + ")";
      last_slot_ = slot;
      last_stride_ = stride;
    }
    if (pixel_) {
      s += write_fetch_dest(dst, bits(d0, 18, 1), bits(d1, 0, 12), "float4(0.0, 0.0, 0.0, 1.0)");
      return s + "    }\n";
    }
    int reg = vb_register(last_slot_);
    uint32_t words = 1, comps = 4;
    switch (fmt_) {
      case 26: case 32: case 34: case 37: words = 2; break;
      case 57: words = 3; break;
      case 35: case 38: words = 4; break;
    }
    s += fmt("      uint vbase = (uint)(%s * %u + %u) * 4u;\n", last_index_.c_str(), last_stride_, offset);
    s += fmt("      uint ve = xe_fetch_dw(%u) & 3u;\n", last_slot_ * 2 + 1);
    s += fmt("      uint4 vw = uint4(0, 0, 0, 0);\n");
    s += fmt("      if (vbase + %u <= xe_vb_size[%d >> 2][%d & 3]) {\n", words * 4, reg, reg);
    for (uint32_t i = 0; i < words; i++)
      s += fmt("        vw.%c = xe_swap(vb%d.Load(vbase + %u), ve);\n", kComp[i], reg, i * 4);
    s += "      }\n";
    std::string v;
    auto unpack = [&](const int* widths, int n) {
      std::string r = "float4(";
      int shift = 0;
      for (int i = 0; i < 4; i++) {
        if (i) r += ", ";
        if (i < n) {
          r += fmt("xe_conv((vw.x >> %d) & 0x%Xu, %d, %u)", shift, (1u << widths[i]) - 1, widths[i], fl);
          shift += widths[i];
        } else {
          r += i == 3 ? "1.0" : "0.0";
        }
      }
      return r + ")";
    };
    static const int k8888[] = {8, 8, 8, 8}, k2101010[] = {10, 10, 10, 2}, k101111[] = {11, 11, 10},
                     k111110[] = {10, 11, 11}, k1616[] = {16, 16};
    switch (fmt_) {
      case 6: v = unpack(k8888, 4); comps = 4; break;
      case 7: v = unpack(k2101010, 4); break;
      case 16: v = unpack(k101111, 3); break;
      case 17: v = unpack(k111110, 3); break;
      case 25: v = unpack(k1616, 2); break;
      case 26:
        v = fmt("float4(xe_conv(vw.x & 0xFFFFu, 16, %u), xe_conv(vw.x >> 16, 16, %u), xe_conv(vw.y & 0xFFFFu, 16, %u), "
                "xe_conv(vw.y >> 16, 16, %u))", fl, fl, fl, fl);
        break;
      case 31: v = "float4(f16tof32(vw.x), f16tof32(vw.x >> 16), 0.0, 1.0)"; break;
      case 32: v = "float4(f16tof32(vw.x), f16tof32(vw.x >> 16), f16tof32(vw.y), f16tof32(vw.y >> 16))"; break;
      case 33: v = (fl & 1) ? "float4((float)(int)vw.x, 0.0, 0.0, 1.0)" : "float4((float)vw.x, 0.0, 0.0, 1.0)"; break;
      case 34: v = (fl & 1) ? "float4((float)(int)vw.x, (float)(int)vw.y, 0.0, 1.0)" : "float4((float)vw.x, (float)vw.y, 0.0, 1.0)"; break;
      case 35: v = (fl & 1) ? "float4((float4)(int4)vw)" : "float4((float4)vw)"; break;
      case 36: v = "float4(asfloat(vw.x), 0.0, 0.0, 1.0)"; break;
      case 37: v = "float4(asfloat(vw.xy), 0.0, 1.0)"; break;
      case 38: v = "asfloat(vw)"; break;
      case 57: v = "float4(asfloat(vw.xyz), 1.0)"; break;
      default:
        v = "float4(0.0, 0.0, 0.0, 1.0)";
        break;
    }
    (void)comps;
    bool is_float = fmt_ == 31 || fmt_ == 32 || (fmt_ >= 36 && fmt_ <= 38) || fmt_ == 57;
    if (exp_adj && !is_float) v = fmt("(%s * %f)", v.c_str(), std::ldexp(1.0, exp_adj));
    s += fmt("      float4 fv = %s;\n", v.c_str());
    s += write_fetch_dest(dst, bits(d0, 18, 1), bits(d1, 0, 12), "fv");
    return s + "    }\n";
  }

  std::string emit_tfetch(const uint32_t* w) {
    uint32_t d0 = w[0], d1 = w[1];
    uint32_t op = bits(d0, 0, 5);
    if (op != 1) return "";  // get/set LOD, gradientes: sem efeito visível nesta fase
    uint32_t src_reg = bits(d0, 5, 6), dst = bits(d0, 12, 6), k = bits(d0, 20, 5), sw = bits(d0, 26, 6);
    std::string s = "    {\n";
    std::string sr = bits(d0, 11, 1) ? fmt("r[(%u + aL) & 63]", src_reg) : fmt("r[%u]", src_reg);
    const bool cube = bits(w[2], 14, 2) == 3;  // dimension (dword 2, bits 14..15): 3 = cube
    if (cube) {
      s += fmt("      float3 tc = xe_cube_dir(float3(%s.%c, %s.%c, %s.%c));\n", sr.c_str(), kComp[sw & 3], sr.c_str(),
               kComp[(sw >> 2) & 3], sr.c_str(), kComp[(sw >> 4) & 3]);
      if (pixel_) cube_mask_ |= 1u << k;
    } else {
      s += fmt("      float2 tc = float2(%s.%c, %s.%c);\n", sr.c_str(), kComp[sw & 3], sr.c_str(), kComp[(sw >> 2) & 3]);
    }
    if (bits(d0, 25, 1) && !cube) s += fmt("      tc /= max(xe_tex_size[%u].xy, float2(1.0, 1.0));\n", k);
    // Cópias de resolve podem ser maiores que o que a constante de fetch descreve (pitch de destino alinhado).
    if (pixel_ && !cube) s += fmt("      tc *= xe_tex_size[%u].zw;\n", k);
    if (pixel_) {
      tex_mask_ |= 1u << k;
      // LOD automático (mips) só sem o ciclo da máquina de estados: derivadas dentro de um ciclo de iterações
      // variáveis não são permitidas pelo FXC.
      if (linear_)
        s += fmt("      float4 tv = xe_tswz(t%u.Sample(s%u, tc), xe_fetch_dw(%u));\n", k, k, k * 6 + 3);
      else
        s += fmt("      float4 tv = xe_tswz(t%u.SampleLevel(s%u, tc, 0.0), xe_fetch_dw(%u));\n", k, k, k * 6 + 3);
    } else {
      s += "      float4 tv = float4(0.0, 0.0, 0.0, 0.0);\n";
    }
    s += write_fetch_dest(dst, bits(d0, 18, 1), bits(d1, 0, 12), "tv");
    return s + "    }\n";
  }

  std::string emit_exec(uint32_t addr, uint32_t count, uint32_t seq) {
    std::string s;
    for (uint32_t i = 0; i < count; i++, seq >>= 2) {
      uint32_t d = (addr + i) * 3;
      if (d + 3 > u_.size()) {
        error_ = "EXEC fora do shader";
        break;
      }
      const uint32_t* w = &u_[d];
      if (seq & 1) {
        fetch_++;
        uint32_t op = bits(w[0], 0, 5);
        std::string f = op == 0 ? emit_vfetch(w) : emit_tfetch(w);
        bool pred = bits(w[1], 31, 1);
        if (pred && !f.empty()) f = fmt("    if (p0 == %s) {\n", bits(w[2], 31, 1) ? "true" : "false") + f + "    }\n";
        s += f;
      } else {
        s += emit_alu(w);
      }
    }
    return s;
  }

  std::string emit_cf(uint32_t i) {
    const Cf& cf = cfs_[i];
    uint32_t op = cf.op();
    std::string s = fmt("  %sif (pc == %uu) {\n", i ? "else " : "", i), next = fmt("    npc = %u;\n", i + 1);
    uint32_t addr = bits(cf.lo, 0, 12), count = bits(cf.lo, 12, 3), seq = bits(cf.lo, 16, 12);
    switch (op) {
      case kExec:
      case kExecEnd:
        s += emit_exec(addr, count, seq);
        s += op == kExecEnd ? "    done = true;\n" : next;
        break;
      case kCondExec:
      case kCondExecEnd:
      case kCondExecPredClean:
      case kCondExecPredCleanEnd: {
        uint32_t b = bits(cf.hi, 2, 8), cond = bits(cf.hi, 10, 1);
        s += fmt("    if (xe_bool(%u) == %s) {\n", b, cond ? "true" : "false") + emit_exec(addr, count, seq) + "    }\n";
        s += (op == kCondExecEnd || op == kCondExecPredCleanEnd) ? "    done = true;\n" : next;
        break;
      }
      case kCondExecPred:
      case kCondExecPredEnd: {
        uint32_t cond = bits(cf.hi, 10, 1);
        s += fmt("    if (p0 == %s) {\n", cond ? "true" : "false") + emit_exec(addr, count, seq) + "    }\n";
        s += op == kCondExecPredEnd ? "    done = true;\n" : next;
        break;
      }
      case kLoopStart: {
        uint32_t target = bits(cf.lo, 0, 13), id = bits(cf.lo, 16, 5);
        // Pilha de loops em uint4/int4 (xe_set4*): o FXC não aceita escrever em arrays com índice dinâmico aqui.
        s += fmt("    { uint lc = xe_loop(%u); lsp = min(lsp + 1, 3); lcount = xe_set4u(lcount, lsp, lc & 0xFFu);\n"
                 "      lstep = xe_set4i(lstep, lsp, (int)(lc << 8) >> 24); lal = xe_set4i(lal, lsp, aL);\n"
                 "      aL = (int)((lc >> 8) & 0xFFu);\n"
                 "      if (lcount[lsp] == 0) { aL = lal[lsp]; lsp = max(lsp - 1, 0); npc = %u; } else npc = %u; }\n",
                 id, target, i + 1);
        break;
      }
      case kLoopEnd: {
        uint32_t target = bits(cf.lo, 0, 13);
        bool pbreak = bits(cf.lo, 21, 1);
        uint32_t cond = bits(cf.hi, 10, 1);
        s += fmt("    { lcount = xe_set4u(lcount, lsp, lcount[lsp] - 1u); bool brk = %s;\n"
                 "      if (lcount[lsp] != 0 && !brk) { aL += lstep[lsp]; npc = %u; }\n"
                 "      else { aL = lal[lsp]; lsp = max(lsp - 1, 0); npc = %u; } }\n",
                 pbreak ? fmt("p0 == %s", cond ? "true" : "false").c_str() : "false", target, i + 1);
        break;
      }
      case kCondCall: {
        uint32_t target = bits(cf.lo, 0, 13);
        std::string cond = bits(cf.lo, 13, 1) ? "true"
                           : bits(cf.lo, 14, 1) ? fmt("p0 == %s", bits(cf.hi, 10, 1) ? "true" : "false")
                                                 : fmt("xe_bool(%u) == %s", bits(cf.hi, 2, 8), bits(cf.hi, 10, 1) ? "true" : "false");
        s += fmt("    if (%s) { csp = min(csp + 1, 3); cstack = xe_set4u(cstack, csp, %uu); npc = %u; } else npc = %u;\n", cond.c_str(), i + 1,
                 target, i + 1);
        break;
      }
      case kReturn:
        s += "    if (csp >= 0) { npc = cstack[csp]; csp--; } else done = true;\n";
        break;
      case kCondJmp: {
        uint32_t target = bits(cf.lo, 0, 13);
        std::string cond = bits(cf.lo, 13, 1) ? "true"
                           : bits(cf.lo, 14, 1) ? fmt("p0 == %s", bits(cf.hi, 10, 1) ? "true" : "false")
                                                 : fmt("xe_bool(%u) == %s", bits(cf.hi, 2, 8), bits(cf.hi, 10, 1) ? "true" : "false");
        if (linear_) s += fmt("    if (%s) resume = %uu;\n", cond.c_str(), target);
        else s += fmt("    npc = (%s) ? %uu : %uu;\n", cond.c_str(), target, i + 1);
        break;
      }
      default:  // nop, alloc, mark_vs_fetch_done
        s += next;
        break;
    }
    if (linear_) {
      // Modo linear: cada CF é um bloco guardado por `resume`; avançar para a seguinte é implícito e um fim de
      // programa põe resume no infinito.
      replace_all(s, fmt("  %sif (pc == %uu) {\n", i ? "else " : "", i), fmt("  if (resume <= %uu) {\n", i));
      replace_all(s, next, "");
      replace_all(s, "    done = true;\n", "    resume = 0xFFFFFFFFu;\n");
      return s + "    }\n";
    }
    if (i + 1 == cfs_.size() && op != kExecEnd && op != kCondExecEnd && op != kCondExecPredEnd &&
        op != kCondExecPredCleanEnd)
      s += "    if (npc >= " + std::to_string(cfs_.size()) + ") done = true;\n";
    return s + "    }\n";
  }

  static void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
  }

  // Fluxo de controlo sem loops, calls nem saltos para trás: tradução direta (o FXC demora dezenas de segundos a
  // otimizar a máquina de estados de shaders grandes).
  bool is_linear() const {
    for (size_t i = 0; i < cfs_.size(); i++) {
      uint32_t op = cfs_[i].op();
      if (op == kLoopStart || op == kLoopEnd || op == kCondCall || op == kReturn) return false;
      if (op == kCondJmp && bits(cfs_[i].lo, 0, 13) <= i) return false;
    }
    return true;
  }

  std::string machine(const std::string& body) {
    std::string s;
    s += "  float4 r[64];\n  [unroll] for (uint ri = 0; ri < 64; ri++) r[ri] = float4(0.0, 0.0, 0.0, 0.0);\n";
    s += "  int a0 = 0, aL = 0; bool p0 = false; float ps = 0.0;\n";
    s += "  uint4 lcount = uint4(0, 0, 0, 0); int4 lstep = int4(0, 0, 0, 0); int4 lal = int4(0, 0, 0, 0); int lsp = 0;\n";
    s += "  uint4 cstack = uint4(0, 0, 0, 0); int csp = -1;\n";
    return s;
  }

  std::string state_loop(const std::string& body) {
    std::string s;
    if (linear_) return "  uint resume = 0;\n" + body;
    s += "  uint pc = 0; bool done = false; uint guard = 0;\n";
    // Cadeia de if em vez de switch: o FXC tem erros internos ("l-value expected") com switch dentro do ciclo.
    // Cada bloco escreve npc (o próximo índice de CF).
    s += "  [loop] while (!done && guard < 65536u) {\n    guard++;\n    uint npc = pc;\n";
    s += body;
    s += "    else { done = true; }\n    pc = npc;\n  }\n";
    return s;
  }

  std::string assemble(const std::string& body) {
    std::string h = kPrelude;
    if (!pixel_) {
      h += "ByteAddressBuffer xe_idx : register(t0, space1);\n";
      for (size_t i = 0; i < vslots_.size(); i++) h += fmt("ByteAddressBuffer vb%zu : register(t%zu, space1);\n", i, i + 1);
      h += "struct VSRaw { float4 pos; float4 o[16]; float4 misc; };\n";
      h += "struct VSOut { float4 pos : SV_Position; float4 o[16] : TEXCOORD0; };\n";
      h += "VSRaw xe_run(uint vertex_index) {\n  VSRaw o = (VSRaw)0;\n";
      h += machine(body);
      h += "  r[0].x = (float)vertex_index;\n";
      h += state_loop(body);
      h += "  return o;\n}\n";
      h += R"(
VSOut main(uint vid : SV_VertexID) {
  uint e = xe_idx.Load(vid * 4);
  // Canto sintetizado de uma rect list (0x80000000 | posição do 1º canto): v1 + v2 - v0. Uma só chamada a xe_run
  // num ciclo (o FXC expande a função em cada ponto de chamada).
  uint corners = (e & 0x80000000u) ? 3u : 1u, first = e & 0x7FFFFFFFu;
  VSRaw raw = (VSRaw)0;
  [loop] for (uint k = 0; k < corners; k++) {
    VSRaw t = xe_run((corners == 1 ? e : xe_idx.Load((first + k) * 4)) + xe_flags.w);
    float wgt = (corners == 3 && k == 0) ? -1.0 : 1.0;
    raw.pos += wgt * t.pos;
    [unroll] for (uint i = 0; i < 16; i++) raw.o[i] += wgt * t.o[i];
    raw.misc = t.misc;
  }
  VSOut o;
  uint vte = xe_flags.x;
  float4 p = raw.pos;
  float w = (vte & 0x400u) ? p.w : (p.w != 0.0 ? 1.0 / p.w : 1.0);
  float3 q = p.xyz;
  if (vte & 0x100u) q.xy *= w;
  if (vte & 0x200u) q.z *= w;
  float iw = w != 0.0 ? 1.0 / w : 1.0;
  float3 n = q * iw;
  float2 scr = float2((vte & 1u) ? n.x * xe_vp_scale.x : n.x, (vte & 4u) ? n.y * xe_vp_scale.y : n.y);
  if (vte & 2u) scr.x += xe_vp_offset.x;
  if (vte & 8u) scr.y += xe_vp_offset.y;
  scr += xe_rt.zw;
  float z = (vte & 0x10u) ? n.z * xe_vp_scale.z : n.z;
  if (vte & 0x20u) z += xe_vp_offset.z;
  float2 ndc = float2(scr.x * xe_rt.x - 1.0, 1.0 - scr.y * xe_rt.y);
  o.pos = float4(ndc * w, saturate(z) * w, w);
  [unroll] for (uint i = 0; i < 16; i++) o.o[i] = raw.o[i];
  return o;
}
)";
    } else {
      for (int k = 0; k < 32; k++)
        if (tex_mask_ & (1u << k))
          h += fmt("%s<float4> t%d : register(t%d);\nSamplerState s%d : register(s%d);\n",
                   (cube_mask_ & (1u << k)) ? "TextureCube" : "Texture2D", k, k, k, k);
      h += "struct PSIn { float4 pos : SV_Position; float4 o[16] : TEXCOORD0; };\n";
      h += "struct PSOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;";
      if (writes_depth_) h += " float depth : SV_Depth;";
      h += " };\n";
      h += "PSOut main(PSIn i) {\n";
      h += "  float4 oC[4] = {float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0), float4(0, 0, 0, 0)};\n";
      h += "  float4 oDepth = float4(i.pos.z, 0, 0, 0);\n";
      h += machine(body);
      h += "  [unroll] for (uint k = 0; k < 16; k++) r[k] = i.o[k];\n";
      h += "  if (xe_flags.z & 0x100u) r[xe_flags.z & 63u] = float4(i.pos.xy * xe_res.xy, 0.0, 0.0);\n";
      h += state_loop(body);
      h += R"(  if (xe_flags.y & 8u) {
    uint f = xe_flags.y & 7u; float a = oC[0].w, ref = xe_alpha_ref.x;
    bool apass = f == 0 ? false : f == 1 ? a < ref : f == 2 ? a == ref : f == 3 ? a <= ref : f == 4 ? a > ref :
                f == 5 ? a != ref : f == 6 ? a >= ref : true;
    if (!apass) discard;
  }
  PSOut o;
  o.c0 = oC[0] * xe_rt_scale.x; o.c1 = oC[1] * xe_rt_scale.y; o.c2 = oC[2] * xe_rt_scale.z; o.c3 = oC[3] * xe_rt_scale.w;
)";
      if (writes_depth_) h += "  o.depth = saturate(oDepth.x);\n";
      h += "  return o;\n}\n";
    }
    return h;
  }
};

}  // namespace

uint64_t hash_words(const uint32_t* w, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; i++) {
    h ^= w[i];
    h *= 0x100000001B3ull;
  }
  return h ^ n;
}

TranslatedShader translate_shader(const std::vector<uint32_t>& ucode, bool pixel) {
  return Translator(ucode, pixel).run();
}

}  // namespace gpu
