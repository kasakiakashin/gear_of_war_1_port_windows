#include "gpu/texture.h"

#include <dxgiformat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gpu {

FormatInfo texture_format_info(uint32_t format) {
  switch (format) {
    case 2: case 8: case 9: return {DXGI_FORMAT_R8_UNORM, 1, 1, 1};                // k_8, k_8_A, k_8_B
    case 3: return {DXGI_FORMAT_B5G5R5A1_UNORM, 1, 1, 2};                          // k_1_5_5_5
    case 4: return {DXGI_FORMAT_B5G6R5_UNORM, 1, 1, 2};                            // k_5_6_5
    case 6: case 14: case 50: case 62: return {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4};  // k_8_8_8_8…
    case 7: case 54: return {DXGI_FORMAT_R10G10B10A2_UNORM, 1, 1, 4};              // k_2_10_10_10
    case 10: return {DXGI_FORMAT_R8G8_UNORM, 1, 1, 2};                             // k_8_8
    case 15: return {DXGI_FORMAT_B4G4R4A4_UNORM, 1, 1, 2};                         // k_4_4_4_4
    case 18: case 51: return {DXGI_FORMAT_BC1_UNORM, 4, 4, 8};                     // k_DXT1
    case 19: case 52: return {DXGI_FORMAT_BC2_UNORM, 4, 4, 16};                    // k_DXT2_3
    case 20: case 53: return {DXGI_FORMAT_BC3_UNORM, 4, 4, 16};                    // k_DXT4_5
    case 24: return {DXGI_FORMAT_R16_UNORM, 1, 1, 2};
    case 25: return {DXGI_FORMAT_R16G16_UNORM, 1, 1, 4};
    case 26: return {DXGI_FORMAT_R16G16B16A16_UNORM, 1, 1, 8};
    case 30: return {DXGI_FORMAT_R16_FLOAT, 1, 1, 2};
    case 31: return {DXGI_FORMAT_R16G16_FLOAT, 1, 1, 4};
    case 32: return {DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1, 8};
    case 33: return {DXGI_FORMAT_R32_UINT, 1, 1, 4};
    case 36: return {DXGI_FORMAT_R32_FLOAT, 1, 1, 4};
    case 37: return {DXGI_FORMAT_R32G32_FLOAT, 1, 1, 8};
    case 38: return {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 16};
    case 49: return {DXGI_FORMAT_BC5_UNORM, 4, 4, 16};                             // k_DXN
    case 59: return {DXGI_FORMAT_BC4_UNORM, 4, 4, 8};                              // k_DXT5A
    default: return {};
  }
}

uint32_t color_rt_dxgi(uint32_t format) {
  switch (format) {
    case 0: case 1: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case 2: case 10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 3: case 5: case 7: case 12: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 4: case 6: return DXGI_FORMAT_R16G16_FLOAT;
    case 14: return DXGI_FORMAT_R32_FLOAT;
    case 15: return DXGI_FORMAT_R32G32_FLOAT;
    default: return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

uint32_t tiled_offset_2d(uint32_t x, uint32_t y, uint32_t pitch_aligned, uint32_t bpb_log2) {
  uint32_t outer_blocks = ((y >> 5) * (pitch_aligned >> 5) + (x >> 5)) << 6;
  uint32_t inner_blocks = (((y >> 1) & 7) << 3) | (x & 7);
  uint32_t oib = (outer_blocks | inner_blocks) << bpb_log2;
  uint32_t bank = (y >> 4) & 1;
  uint32_t pipe = ((x >> 3) & 3) ^ (((y >> 3) & 1) << 1);
  return ((y & 1) << 4) | (pipe << 6) | (bank << 11) | (oib & 0xF) | (((oib >> 4) & 1) << 5) |
         (((oib >> 5) & 7) << 8) | ((oib >> 8) << 12);
}

namespace {

constexpr uint32_t kPhysicalSize = 0x20000000u, kPhysicalMirror = 0xA0000000u;

uint32_t swap32(uint32_t v, uint32_t e) {
  switch (e) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    case 2: return __builtin_bswap32(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}

uint32_t log2u(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) r++;
  return r;
}

}  // namespace

bool decode_surface(const uint8_t* mem, uint32_t phys, uint32_t format, uint32_t endian, bool tiled, uint32_t width,
                    uint32_t height, uint32_t pitch_texels, DecodedTexture& out, uint32_t ox, uint32_t oy) {
  FormatInfo fi = texture_format_info(format);
  if (!fi.dxgi || !width || !height) return false;
  uint32_t bw = (width + fi.block_w - 1) / fi.block_w, bh = (height + fi.block_h - 1) / fi.block_h;
  uint32_t pitch_blocks = (std::max(pitch_texels, width) + fi.block_w - 1) / fi.block_w;
  uint32_t bpb = fi.bytes_per_block, bpb_log2 = log2u(bpb);
  out.dxgi = fi.dxgi;
  out.width = bw * fi.block_w;
  out.height = bh * fi.block_h;
  out.row_pitch = bw * bpb;
  out.rows = bh;
  out.data.assign((size_t)out.row_pitch * bh, 0);
  phys &= kPhysicalSize - 1;
  const uint8_t* src = mem + kPhysicalMirror + phys;
  uint32_t avail = kPhysicalSize - phys;
  if (tiled) {
    uint32_t pitch_aligned = (pitch_blocks + 31) & ~31u;
    for (uint32_t y = 0; y < bh; y++) {
      uint8_t* row = &out.data[(size_t)y * out.row_pitch];
      for (uint32_t x = 0; x < bw; x++) {
        uint32_t off = tiled_offset_2d(x + ox, y + oy, pitch_aligned, bpb_log2);
        if (off + bpb <= avail) std::memcpy(row + (size_t)x * bpb, src + off, bpb);
      }
    }
  } else {
    uint32_t src_pitch = pitch_blocks * bpb;
    for (uint32_t y = 0; y < bh; y++) {
      uint32_t off = y * src_pitch;
      if (off + out.row_pitch > avail) break;
      std::memcpy(&out.data[(size_t)y * out.row_pitch], src + off, out.row_pitch);
    }
  }
  if (endian) {
    uint32_t* w = reinterpret_cast<uint32_t*>(out.data.data());
    for (size_t i = 0, n = out.data.size() / 4; i < n; i++) w[i] = swap32(w[i], endian);
  }
  return true;
}

bool decode_texture(const uint8_t* mem, const TextureFetch& f, DecodedTexture& out) {
  return decode_surface(mem, f.base(), f.format(), f.endian(), f.tiled(), f.width(), f.height(), f.pitch_texels(), out);
}

uint64_t hash_bytes(const uint8_t* p, size_t n) {
  // 4 acumuladores independentes (32 bytes por volta): ~4x mais rápido que uma cadeia única.
  uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
  size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    for (int k = 0; k < 4; k++) {
      uint64_t v;
      std::memcpy(&v, p + i + 8 * k, 8);
      h[k] = (h[k] ^ v) * 0x100000001B3ull;
      h[k] ^= h[k] >> 29;
    }
  }
  uint64_t r = h[0] ^ (h[1] * 3) ^ (h[2] * 5) ^ (h[3] * 7);
  for (; i < n; i++) r = (r ^ p[i]) * 0x100000001B3ull;
  return r;
}

uint64_t texture_raw_hash(const uint8_t* mem, const TextureFetch& f) {
  FormatInfo fi = texture_format_info(f.format());
  if (!fi.dxgi) return 0;
  auto region = [&](uint32_t phys, uint64_t bytes) {
    phys &= kPhysicalSize - 1;
    bytes = std::min<uint64_t>(bytes, kPhysicalSize - phys);
    return hash_bytes(mem + kPhysicalMirror + phys, (size_t)bytes);
  };
  uint32_t bw = (f.width() + fi.block_w - 1) / fi.block_w, bh = (f.height() + fi.block_h - 1) / fi.block_h;
  uint32_t pitch_blocks = (std::max(f.pitch_texels(), f.width()) + fi.block_w - 1) / fi.block_w;
  uint64_t size0 = f.tiled() ? (uint64_t)((pitch_blocks + 31) & ~31u) * ((bh + 31) & ~31u) * fi.bytes_per_block
                             : (uint64_t)pitch_blocks * bh * fi.bytes_per_block;
  (void)bw;
  uint64_t h = region(f.base(), size0) ^ ((uint64_t)f.dword[1] << 32 | f.dword[2]);
  uint32_t addr = f.mip_address(), max_level = f.mip_max_level();
  if (!addr || !max_level || !f.tiled()) return h;
  auto npot = [](uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
  };
  uint32_t w0 = npot(f.width()), h0 = npot(f.height());
  uint64_t mips = 0;
  for (uint32_t l = 1; l <= max_level; l++) {  // mesma geometria que decode_texture_mips
    uint32_t sw = std::max(w0 >> l, 1u), sh = std::max(h0 >> l, 1u);
    uint32_t pb = (((sw + fi.block_w - 1) / fi.block_w) + 31) & ~31u;
    uint32_t rb = (((sh + fi.block_h - 1) / fi.block_h) + 31) & ~31u;
    mips += ((uint64_t)pb * rb * fi.bytes_per_block + 0xFFF) & ~0xFFFull;
  }
  return h * 31 + region(addr, mips);
}

}  // namespace gpu

namespace gpu {

// Deslocamento em texels de um nível na cauda packed (Xenia, texture_util: GetPackedMipOffset).
static void packed_offset(uint32_t log2w, uint32_t log2h, uint32_t packed_level, uint32_t level, uint32_t& x, uint32_t& y) {
  const uint32_t pm = level - packed_level;
  x = y = 0;
  if (pm < 3) {
    if (log2w > log2h) y = 16 >> pm;
    else x = 16 >> pm;
  } else {
    const uint32_t o = (1u << ((log2w > log2h ? log2w : log2h) - packed_level)) >> (pm - 2);
    if (log2w > log2h) x = o;
    else y = o;
  }
}

bool decode_texture_mips(const uint8_t* mem, const TextureFetch& f, std::vector<DecodedTexture>& levels) {
  levels.clear();
  levels.emplace_back();
  FormatInfo fi = texture_format_info(f.format());
  // Texturas com o lado menor ≤ 16: até o nível 0 está na cauda packed, deslocado dentro do tile de 32×32 blocos.
  const uint32_t log2w0 = log2u(f.width()), log2h0 = log2u(f.height());
  const bool base_packed = f.packed_mips() && f.tiled() && std::min(log2w0, log2h0) <= 4;
  if (base_packed) {
    uint32_t ox, oy;
    packed_offset(log2w0, log2h0, 0, 0, ox, oy);
    if (!decode_surface(mem, f.base(), f.format(), f.endian(), true, f.width(), f.height(), f.pitch_texels(), levels[0],
                        ox / fi.block_w, oy / fi.block_h))
      return false;
  } else if (!decode_texture(mem, f, levels[0])) {
    return false;
  }
  uint32_t addr = f.mip_address(), max_level = f.mip_max_level();
  if (!addr && base_packed) addr = f.base();  // sem endereço de mips: a cauda continua na área da base
  if (!addr || !max_level || !f.tiled()) return true;
  auto npot = [](uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
  };
  uint32_t w0 = npot(f.width()), h0 = npot(f.height());
  // Cauda packed (Xenia, texture_util: GetPackedMipLevel/GetPackedMipOffset): a partir do nível
  // log2(min(w, h)) - 4, todos os mips partilham a área desse nível (uma superfície tiled com o pitch dele), cada um
  // num deslocamento em blocos próprio.
  const uint32_t log2w = log2u(f.width()), log2h = log2u(f.height()), log2s = std::min(log2w, log2h);
  const uint32_t packed_level = f.packed_mips() ? (log2s > 4 ? log2s - 4 : 0) : ~0u;
  uint32_t tail_addr = 0, tail_pitch = 0;
  for (uint32_t l = 1; l <= max_level; l++) {
    uint32_t w = std::max(f.width() >> l, 1u), h = std::max(f.height() >> l, 1u);
    if (l >= packed_level) {
      if (!tail_addr) {
        uint32_t sw = std::max(w0 >> l, 1u);
        tail_addr = addr;
        tail_pitch = (((sw + fi.block_w - 1) / fi.block_w) + 31) & ~31u;
        if (addr == f.base()) tail_pitch = (((f.pitch_texels() + fi.block_w - 1) / fi.block_w) + 31) & ~31u;
      }
      uint32_t x, y;
      packed_offset(log2w, log2h, packed_level, l, x, y);
      DecodedTexture d;
      if (!decode_surface(mem, tail_addr, f.format(), f.endian(), true, w, h, tail_pitch * fi.block_w, d,
                          x / fi.block_w, y / fi.block_h))
        break;
      if (d.width != std::max(levels[0].width >> l, fi.block_w) || d.height != std::max(levels[0].height >> l, fi.block_h))
        break;
      levels.push_back(std::move(d));
      continue;
    }
    uint32_t sw = std::max(w0 >> l, 1u), sh = std::max(h0 >> l, 1u);
    uint32_t pitch_blocks = (((sw + fi.block_w - 1) / fi.block_w) + 31) & ~31u;
    uint32_t rows_blocks = (((sh + fi.block_h - 1) / fi.block_h) + 31) & ~31u;
    DecodedTexture d;
    if (!decode_surface(mem, addr, f.format(), f.endian(), true, w, h, pitch_blocks * fi.block_w, d)) break;
    // DXGI exige que cada mip tenha exatamente max(base >> l, 1) (arredondado aos blocos).
    if (d.width != std::max(levels[0].width >> l, fi.block_w) || d.height != std::max(levels[0].height >> l, fi.block_h))
      break;
    levels.push_back(std::move(d));
    addr += (pitch_blocks * rows_blocks * fi.bytes_per_block + 0xFFF) & ~0xFFFu;
  }
  static const bool trace = std::getenv("GOW_TRACE_PACKED") != nullptr;
  static int traced = 0;
  if (trace && tail_addr && traced < 40) {
    traced++;
    std::fprintf(stderr, "[packed] fmt %u %ux%u nível packed %u:", f.format(), f.width(), f.height(), packed_level);
    for (auto& lv : levels) {
      uint64_t sum = 0;
      for (uint8_t b : lv.data) sum += b;
      std::fprintf(stderr, " %ux%u=%.0f", lv.width, lv.height, lv.data.empty() ? 0.0 : (double)sum / lv.data.size());
    }
    std::fprintf(stderr, "\n");
  }
  return true;
}

// Cube map: por nível, 6 faces seguidas, cada uma com o armazenamento do nível alinhado a 4 KiB. O nível 0 fica em
// base; os outros em mip_address; a cauda packed partilha a área do seu nível em cada face (mesmos deslocamentos 2D).
bool decode_cube_mips(const uint8_t* mem, const TextureFetch& f, std::vector<DecodedTexture>& out, uint32_t& mips) {
  FormatInfo fi = texture_format_info(f.format());
  if (!fi.dxgi) return false;
  out.clear();
  auto storage = [&](uint32_t sw, uint32_t sh, uint32_t& pitch_blocks) {
    pitch_blocks = (((sw + fi.block_w - 1) / fi.block_w) + 31) & ~31u;
    uint32_t rows = (((sh + fi.block_h - 1) / fi.block_h) + 31) & ~31u;
    return (pitch_blocks * rows * fi.bytes_per_block + 0xFFFu) & ~0xFFFu;
  };
  uint32_t pitch0 = 0, face0 = 0;
  {
    uint32_t pb = (std::max(f.pitch_texels(), f.width()) + fi.block_w - 1) / fi.block_w;
    uint32_t bh = (f.height() + fi.block_h - 1) / fi.block_h;
    pitch0 = pb;
    face0 = f.tiled() ? (((pb + 31) & ~31u) * ((bh + 31) & ~31u) * fi.bytes_per_block + 0xFFFu) & ~0xFFFu
                      : (pb * bh * fi.bytes_per_block + 0xFFFu) & ~0xFFFu;
  }
  (void)pitch0;
  uint32_t base_ox = 0, base_oy = 0;
  if (f.packed_mips() && f.tiled() && std::min(log2u(f.width()), log2u(f.height())) <= 4) {
    packed_offset(log2u(f.width()), log2u(f.height()), 0, 0, base_ox, base_oy);
    base_ox /= fi.block_w, base_oy /= fi.block_h;
  }
  for (uint32_t i = 0; i < 6; i++) {
    out.emplace_back();
    if (!decode_surface(mem, f.base() + i * face0, f.format(), f.endian(), f.tiled(), f.width(), f.height(),
                        f.pitch_texels(), out.back(), base_ox, base_oy))
      return false;
  }
  mips = 1;
  const uint32_t addr0 = f.mip_address(), max_level = f.mip_max_level();
  if (!addr0 || !max_level || !f.tiled()) return true;
  auto npot = [](uint32_t v) {
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
  };
  const uint32_t w0 = npot(f.width()), h0 = npot(f.height());
  const uint32_t log2w = log2u(f.width()), log2h = log2u(f.height()), log2s = std::min(log2w, log2h);
  const uint32_t packed_level = f.packed_mips() ? (log2s > 4 ? log2s - 4 : 0) : ~0u;
  if (packed_level == 0) return true;  // base packed: só o nível 0
  uint32_t addr = addr0, tail_pitch = 0, tail_stride = 0, tail_addr = 0;
  for (uint32_t l = 1; l <= max_level; l++) {
    const uint32_t w = std::max(f.width() >> l, 1u), h = std::max(f.height() >> l, 1u);
    uint32_t pitch_blocks = 0, stride = storage(std::max(w0 >> l, 1u), std::max(h0 >> l, 1u), pitch_blocks), ox = 0, oy = 0;
    uint32_t base = addr;
    if (l >= packed_level) {
      if (!tail_addr) tail_addr = addr, tail_pitch = pitch_blocks, tail_stride = stride;
      base = tail_addr, pitch_blocks = tail_pitch, stride = tail_stride;
      const uint32_t pm = l - packed_level;
      if (pm < 3) oy = 0, ox = 16 >> pm;  // quadradas: "não mais larga"
      else oy = (1u << (log2h - packed_level)) >> (pm - 2), ox = 0;
      ox /= fi.block_w, oy /= fi.block_h;
    } else {
      addr += 6 * stride;
    }
    const size_t mark = out.size();
    bool ok = true;
    for (uint32_t i = 0; i < 6 && ok; i++) {
      DecodedTexture d;
      ok = decode_surface(mem, base + i * stride, f.format(), f.endian(), true, w, h, pitch_blocks * fi.block_w, d, ox, oy) &&
           d.width == std::max(out[0].width >> l, fi.block_w) && d.height == std::max(out[0].height >> l, fi.block_h);
      if (ok) out.push_back(std::move(d));
    }
    if (!ok) {
      out.resize(mark);
      break;
    }
    mips = l + 1;
  }
  return true;
}

}  // namespace gpu
