// texture.h — constantes de fetch de textura do Xenos e descodificação para formatos do host (D3D12/DXGI).
//
// Memória: endereços físicos (base << 12). Tiling 2D: Tiled2D (pitch alinhado a 32 blocos), como documentado pelo
// Xenia em texture_address.h. Troca de bytes pela "endianness" da constante, em palavras de 32 bits.
// Só o nível 0 (sem mips) nesta fase.
#pragma once

#include <cstdint>
#include <vector>

namespace gpu {

// Endereço de GPU → físico. O D3D põe por vezes endereços do espelho virtual 0xE0000000 nas constantes (ex.: o
// frontbuffer 0xE030F000); esse espelho está deslocado 0x1000 físicos (ver guest_memory.h / PPC_HOST).
inline uint32_t gpu_physical(uint32_t a) { return a >= 0xE0000000u ? (a & 0x1FFFFFFFu) + 0x1000u : a & 0x1FFFFFFFu; }

struct TextureFetch {
  uint32_t dword[6];
  uint32_t format() const { return dword[1] & 0x3F; }
  uint32_t endian() const { return (dword[1] >> 6) & 3; }
  uint32_t base() const { return gpu_physical(dword[1] & 0xFFFFF000u); }  // físico
  bool tiled() const { return dword[0] >> 31; }
  uint32_t pitch_texels() const { return ((dword[0] >> 22) & 0x1FF) << 5; }
  uint32_t width() const { return (dword[2] & 0x1FFF) + 1; }
  uint32_t height() const { return ((dword[2] >> 13) & 0x1FFF) + 1; }
  uint32_t dimension() const { return (dword[5] >> 9) & 3; }
  uint32_t clamp_x() const { return (dword[0] >> 10) & 7; }
  uint32_t clamp_y() const { return (dword[0] >> 13) & 7; }
  uint32_t mag_filter() const { return (dword[3] >> 19) & 3; }
  uint32_t mip_filter() const { return (dword[3] >> 23) & 3; }
  uint32_t mip_max_level() const { return (dword[4] >> 6) & 15; }
  bool packed_mips() const { return (dword[5] >> 11) & 1; }
  uint32_t mip_address() const { return dword[5] & 0xFFFFF000u ? gpu_physical(dword[5] & 0xFFFFF000u) : 0; }
  uint32_t min_filter() const { return (dword[3] >> 21) & 3; }
};

struct FormatInfo {
  uint32_t dxgi = 0;          // DXGI_FORMAT (0 = não suportado)
  uint32_t block_w = 1, block_h = 1, bytes_per_block = 4;
};

// Formato de textura Xenos (TextureFormat) → host.
FormatInfo texture_format_info(uint32_t format);
// Formato de render target (ColorRenderTargetFormat) → DXGI.
uint32_t color_rt_dxgi(uint32_t format);

struct DecodedTexture {
  uint32_t dxgi = 0, width = 0, height = 0, row_pitch = 0, rows = 0;  // rows = linhas de blocos
  std::vector<uint8_t> data;
};

// Descodifica o nível 0 de `f` a partir da memória convidada (mem = base do espaço de 4 GiB). false se o formato não
// for suportado ou o endereço sair da memória física.
bool decode_texture(const uint8_t* mem, const TextureFetch& f, DecodedTexture& out);
// Nível 0 + mips em mip_address (níveis ≥ 1: tamanho de armazenamento com a potência de 2 seguinte, alinhado a 32
// blocos e a 4 KiB), incluindo a cauda "packed" (níveis ≤ 16 texels numa só área), até mip_max_level.
bool decode_texture_mips(const uint8_t* mem, const TextureFetch& f, std::vector<DecodedTexture>& levels);
// Cube map: níveis × 6 faces em `out` (ordem: as 6 faces do nível 0, depois as do nível 1…), `mips` níveis completos.
bool decode_cube_mips(const uint8_t* mem, const TextureFetch& f, std::vector<DecodedTexture>& out, uint32_t& mips);
// Igual, para uma superfície descrita por campos soltos (frontbuffer / destino de resolve).
bool decode_surface(const uint8_t* mem, uint32_t phys, uint32_t format, uint32_t endian, bool tiled, uint32_t width,
                    uint32_t height, uint32_t pitch_texels, DecodedTexture& out, uint32_t ox = 0, uint32_t oy = 0);
// Endereço tiled 2D (bytes) do bloco (x, y): pitch em blocos já alinhado a 32.
uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_aligned, uint32_t bytes_per_block_log2);
// Hash rápido de um intervalo de memória (para detetar alterações de texturas escritas pelo CPU).
uint64_t hash_bytes(const uint8_t* p, size_t n);
// Hash da memória convidada de uma textura (nível 0 + cadeia de mips, sem descodificar): deteta alterações.
uint64_t texture_raw_hash(const uint8_t* mem, const TextureFetch& f);

}  // namespace gpu
