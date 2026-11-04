// xenos_shader.h — tradução do microcódigo de shaders Xenos (vertex/pixel) para HLSL (Shader Model 5.1).
//
// Formato (como documentado pelo Xenia em ucode.h): instruções de fluxo de controle (CF, 48 bits, 2 por cada 3
// dwords) no início; instruções ALU/fetch de 96 bits nos endereços apontados pelos EXEC. O fluxo de controle vira
// uma máquina de estados (switch sobre o índice da CF dentro de um ciclo), o que cobre jumps, loops e calls.
//
// Interface com o renderer (gpu/d3d12_renderer.cpp):
//   b0  float4 c[256]      constantes ALU do estágio
//   b1  uint4 fetch[48]    192 dwords das constantes de fetch (0x4800…), ordem do host
//   b2  uint4 bools[2], loops[8]
//   b3  sistema (viewport, VTE, alpha test, tamanhos de textura) — ver kSystemCbuffer
//   VS: ByteAddressBuffer idx (t0, space1) = índices expandidos; vb<n> (t1+n, space1) = buffers de vértices
//   PS: Texture2D t<k> / SamplerState s<k> para cada constante de fetch de textura k usada.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gpu {

struct TranslatedShader {
  bool ok = false;
  bool pixel = false;
  uint64_t hash = 0;            // do microcódigo
  std::string hlsl, error;
  std::vector<uint32_t> vfetch_slots;  // VS: slots de fetch de vértices usados (0..95), na ordem dos registradores
  std::vector<uint32_t> vfetch_at;     // VS: dword de cada vfetch no microcódigo (entrada k de xe_vf)
  bool bound_vfetch = false;           // VS: formatos e slots dos vfetch lidos de xe_vf (senão, fixos; xe_vf por slot)
  uint32_t texture_mask = 0;    // PS: constantes de fetch de textura usadas (bit k)
  uint32_t cube_mask = 0;       // PS: dessas, as amostradas como cube map (TextureCube)
  // PS: filtro pedido pela própria instrução tfetch, por constante: mag | min << 2 | mip << 4 (0 = ponto, 1 = linear,
  // 2 = nível base, 3 = o da constante de fetch).
  uint8_t tex_filter[32];
  uint64_t const_used[4] = {};  // constantes ALU lidas (bit por constante); todas se houver endereçamento relativo
  uint32_t color_exports = 0;   // PS: oC0..oC3 escritos (bit i)
  bool writes_depth = false;
  uint32_t alu_count = 0, fetch_count = 0, cf_count = 0;
};

constexpr size_t kMaxBoundVfetch = 64;  // tamanho de xe_vf; acima disto o VS fica com os formatos fixos
TranslatedShader translate_shader(const std::vector<uint32_t>& ucode, bool pixel);
uint64_t hash_words(const uint32_t* w, size_t n);

}  // namespace gpu
