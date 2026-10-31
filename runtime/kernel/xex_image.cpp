// xex_image.cpp — ver tools/xex2.py (formato XEX2) e tools/lzx.py (formato LZX), de que isto é o porte direto.
// Só o que este título usa: cifra normal (chave retail) e compressão normal (LZX). A cadeia SHA-1 dos blocos não é
// verificada, porque o chamador compara o SHA-1 da imagem final.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <bcrypt.h>

#include "kernel/xex_image.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xex_image {
namespace {

const uint8_t kRetailKey[16] = {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91};

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// AES-128 sem preenchimento: ECB para a chave de sessão, CBC com IV = 0 para os dados.
bool aes_decrypt(const uint8_t* key, bool cbc, const uint8_t* in, uint8_t* out, ULONG size) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_KEY_HANDLE k = nullptr;
  bool ok = false;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0) {
    const wchar_t* mode = cbc ? BCRYPT_CHAIN_MODE_CBC : BCRYPT_CHAIN_MODE_ECB;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)((wcslen(mode) + 1) * sizeof(wchar_t)), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 16, 0) == 0) {
      uint8_t iv[16] = {};
      ULONG done = 0;
      ok = BCryptDecrypt(k, (PUCHAR)in, size, nullptr, cbc ? iv : nullptr, cbc ? 16 : 0, out, size, &done, 0) == 0 && done == size;
      BCryptDestroyKey(k);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
  }
  return ok;
}

// ---- LZX ---------------------------------------------------------------------------------------------------------
constexpr int kPretreeSyms = 20, kAlignedSyms = 8, kSecondaryLengths = 249, kMinMatch = 2;
constexpr uint32_t kFrameSize = 0x8000;
enum { kVerbatim = 1, kAligned = 2, kUncompressed = 3 };

struct LzxTables {
  uint8_t extra[290];
  uint32_t base[290];
  LzxTables() {
    int j = 0;
    for (int i = 0; i < 290; i += 2) {
      extra[i] = extra[i + 1] = (uint8_t)j;
      if (i != 0 && j < 17) j++;
    }
    uint32_t b = 0;
    for (int i = 0; i < 290; i++) {
      base[i] = b;
      b += 1u << extra[i];
    }
  }
};

// Palavras de 16 bits LE, bits do mais para o menos significativo; depois do fim lê zeros, como o libmspack.
struct Bits {
  const uint8_t* d;
  size_t size, p = 0;
  uint64_t bb = 0;
  int bl = 0;
  void refill() {
    uint32_t w = p + 2 <= size ? (uint32_t)d[p] | (uint32_t)d[p + 1] << 8 : 0;
    p += 2;
    bb = bb << 16 | w;
    bl += 16;
  }
  uint32_t read(int n) {
    if (!n) return 0;
    while (bl < n) refill();
    bl -= n;
    return (uint32_t)(bb >> bl) & ((1u << n) - 1);
  }
  uint32_t sym(const std::vector<uint16_t>& table, const uint8_t* lens) {
    if (bl < 16) refill();
    uint32_t s = table[(uint32_t)(bb >> (bl - 16)) & 0xFFFF];
    bl -= lens[s];
    return s;
  }
};

// Tabela direta de 16 bits de um código de Huffman canónico.
bool make_table(const uint8_t* lens, int nsyms, std::vector<uint16_t>& table) {
  table.assign(65536, 0);
  uint32_t code = 0, filled = 0;
  for (int len = 1; len <= 16; len++) {
    const int shift = 16 - len;
    const uint32_t span = 1u << shift;
    for (int sym = 0; sym < nsyms; sym++) {
      if (lens[sym] != len) continue;
      const uint32_t start = code << shift;
      if (start + span > 65536) return false;
      std::fill_n(table.begin() + start, span, (uint16_t)sym);
      code++;
      filled += span;
    }
    code <<= 1;
  }
  return !filled || filled == 65536 || filled == 32768;  // incompleta só com um único código
}

bool read_lengths(Bits& br, uint8_t* lens, int first, int last) {
  uint8_t pre_len[kPretreeSyms];
  for (auto& l : pre_len) l = (uint8_t)br.read(4);
  std::vector<uint16_t> pre;
  if (!make_table(pre_len, kPretreeSyms, pre)) return false;
  int x = first;
  auto delta = [&](uint32_t z) { return (uint8_t)(lens[x] >= z ? lens[x] - z : lens[x] + 17 - z); };
  while (x < last) {
    uint32_t z = br.sym(pre, pre_len);
    if (z == 17) {
      int y = (int)br.read(4) + 4;
      std::memset(lens + x, 0, y);
      x += y;
    } else if (z == 18) {
      int y = (int)br.read(5) + 20;
      std::memset(lens + x, 0, y);
      x += y;
    } else if (z == 19) {
      int y = (int)br.read(1) + 4;
      const uint8_t v = delta(br.sym(pre, pre_len));
      std::memset(lens + x, v, y);
      x += y;
    } else {
      lens[x] = delta(z);
      x++;
    }
  }
  return true;  // as séries podem passar de 'last': os vetores têm 64 posições de folga
}

bool lzx_decompress(const std::vector<uint8_t>& stream, std::vector<uint8_t>& out, uint32_t window, std::string* error) {
  static const LzxTables t;
  auto fail = [&](const char* why) {
    *error = std::string("LZX: ") + why;
    return false;
  };
  int window_bits = 0;
  while ((1u << window_bits) < window) window_bits++;
  if ((1u << window_bits) != window || window_bits < 15 || window_bits > 21) return fail("janela não suportada");
  static const int kSlots[] = {30, 32, 34, 36, 38, 42, 50};
  const int main_syms = 256 + (kSlots[window_bits - 15] << 3);

  Bits br{stream.data(), stream.size()};
  std::vector<uint8_t> main_len(main_syms + 64), len_len(kSecondaryLengths + 64);
  uint8_t aligned_len[kAlignedSyms] = {};
  std::vector<uint16_t> main_tab, len_tab, aligned_tab;
  uint32_t r0 = 1, r1 = 1, r2 = 1, block_len = 0, block_rem = 0;
  int block_type = 0;
  bool intel_started = false;
  uint32_t intel_size = 0;
  if (br.read(1)) {
    uint32_t hi = br.read(16), lo = br.read(16);
    intel_size = hi << 16 | lo;
  }
  const size_t out_len = out.size();
  size_t pos = 0;
  std::vector<std::pair<size_t, uint32_t>> e8_frames;
  for (uint32_t frame = 0; pos < out_len; frame++) {
    const uint32_t frame_size = (uint32_t)std::min<size_t>(kFrameSize, out_len - pos);
    const size_t frame_start = pos, frame_end = pos + frame_size;
    while (pos < frame_end) {
      if (!block_rem) {
        if (block_type == kUncompressed && (block_len & 1)) br.p++;  // realinha depois de um bloco ímpar sem compressão
        block_type = (int)br.read(3);
        uint32_t hi = br.read(16), lo = br.read(8);
        block_rem = block_len = hi << 8 | lo;
        if (block_type == kAligned) {
          for (auto& l : aligned_len) l = (uint8_t)br.read(3);
          if (!make_table(aligned_len, kAlignedSyms, aligned_tab)) return fail("tabela alinhada inválida");
        }
        if (block_type == kVerbatim || block_type == kAligned) {
          if (!read_lengths(br, main_len.data(), 0, 256) || !read_lengths(br, main_len.data(), 256, main_syms) ||
              !make_table(main_len.data(), main_syms, main_tab))
            return fail("tabela principal inválida");
          if (main_len[0xE8]) intel_started = true;
          if (!read_lengths(br, len_len.data(), 0, kSecondaryLengths) || !make_table(len_len.data(), kSecondaryLengths, len_tab))
            return fail("tabela de comprimentos inválida");
        } else if (block_type == kUncompressed) {
          intel_started = true;
          if (!br.bl) br.refill();
          br.bl = 0;
          br.bb = 0;
          if (br.p + 12 > stream.size()) return fail("fluxo truncado");
          const auto le = [&](size_t o) {
            return (uint32_t)stream[o] | (uint32_t)stream[o + 1] << 8 | (uint32_t)stream[o + 2] << 16 | (uint32_t)stream[o + 3] << 24;
          };
          r0 = le(br.p), r1 = le(br.p + 4), r2 = le(br.p + 8);
          br.p += 12;
        } else {
          return fail("tipo de bloco inválido");
        }
      }
      const uint32_t run = (uint32_t)std::min<size_t>(block_rem, frame_end - pos);
      block_rem -= run;
      const size_t run_end = pos + run;
      if (block_type == kUncompressed) {
        if (br.p + run > stream.size()) return fail("fluxo truncado");
        std::memcpy(&out[pos], &stream[br.p], run);
        br.p += run;
        pos = run_end;
        continue;
      }
      const bool aligned = block_type == kAligned;
      while (pos < run_end) {
        uint32_t s = br.sym(main_tab, main_len.data());
        if (s < 256) {
          out[pos++] = (uint8_t)s;
          continue;
        }
        s -= 256;
        uint32_t mlen = s & 7;
        if (mlen == 7) mlen += br.sym(len_tab, len_len.data());
        mlen += kMinMatch;
        const uint32_t slot = s >> 3;
        uint32_t off;
        if (slot == 0) {
          off = r0;
        } else if (slot == 1) {
          off = r1;
          r1 = r0;
          r0 = off;
        } else if (slot == 2) {
          off = r2;
          r2 = r0;
          r0 = off;
        } else if (slot == 3) {
          off = 1;
          r2 = r1;
          r1 = r0;
          r0 = off;
        } else {
          const int extra = slot >= 36 ? 17 : t.extra[slot];
          off = t.base[slot] - 2;
          if (aligned && extra >= 3) {
            if (extra > 3) off += br.read(extra - 3) << 3;
            off += br.sym(aligned_tab, aligned_len);
          } else {
            off += br.read(extra);
          }
          r2 = r1;
          r1 = r0;
          r0 = off;
        }
        if (off > pos) return fail("deslocamento antes do início");
        const size_t end = pos + mlen;
        if (end > frame_end) return fail("cópia atravessa o quadro");
        // Byte a byte: a origem pode sobrepor-se ao destino (repete o padrão).
        for (size_t src = pos - off; pos < end;) out[pos++] = out[src++];
      }
      if (pos > run_end) {  // a última cópia passou do troço para o resto do bloco
        const size_t over = pos - run_end;
        if (over > block_rem) return fail("cópia passa do fim do bloco");
        block_rem -= (uint32_t)over;
      }
    }
    br.bl &= ~15;  // realinha a 16 bits no fim de cada quadro
    if (intel_started && intel_size && frame < 32768 && frame_size > 10) e8_frames.push_back({frame_start, frame_size});
  }
  if (e8_frames.empty()) return true;
  // A tradução E8 só se aplica à saída final: as cópias acima usam os bytes não traduzidos.
  const std::vector<uint8_t> src = out;
  for (auto [start, size] : e8_frames) {
    const size_t end = start + size - 10;
    for (size_t i = start; i < end;) {
      if (src[i] != 0xE8) {
        i++;
        continue;
      }
      int32_t abs_off;
      std::memcpy(&abs_off, &src[i + 1], 4);
      if (abs_off >= -(int64_t)i && abs_off < (int64_t)intel_size) {
        uint32_t rel = abs_off >= 0 ? (uint32_t)(abs_off - (int64_t)i) : (uint32_t)abs_off + intel_size;
        std::memcpy(&out[i + 1], &rel, 4);
      }
      i += 5;
    }
  }
  return true;
}

}  // namespace

std::string sha1_hex(const uint8_t* data, size_t size) {
  uint8_t digest[20] = {};
  BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, (PUCHAR)data, (ULONG)size, digest, sizeof digest);
  char hex[41];
  for (int i = 0; i < 20; i++) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
  return hex;
}

bool decode(const std::vector<uint8_t>& xex, std::vector<uint8_t>* image, std::string* error) {
  auto fail = [&](const char* why) {
    *error = why;
    return false;
  };
  const size_t n = xex.size();
  if (n < 0x18 || std::memcmp(xex.data(), "XEX2", 4) != 0) return fail("default.xex não é um XEX2");
  const uint32_t pe_offset = be32(&xex[8]), sec = be32(&xex[0x10]), count = be32(&xex[0x14]);
  if (pe_offset >= n || (uint64_t)sec + 0x160 > n || 0x18 + (uint64_t)count * 8 > n) return fail("cabeçalho XEX inválido");
  uint32_t ff = 0;
  for (uint32_t i = 0; i < count; i++)
    if (be32(&xex[0x18 + i * 8]) == 0x3FF) ff = be32(&xex[0x1C + i * 8]);
  if (!ff || (uint64_t)ff + 36 > n) return fail("XEX sem FILE_FORMAT_INFO");
  const uint32_t image_size = be32(&xex[sec + 4]);
  const uint16_t encryption = be16(&xex[ff + 4]), compression = be16(&xex[ff + 6]);
  if (compression != 2) return fail("compressão do XEX não suportada");
  if (image_size > 0x10000000) return fail("tamanho de imagem inválido");

  std::vector<uint8_t> payload(xex.begin() + pe_offset, xex.end());
  if (encryption == 1) {
    uint8_t session[16];
    const ULONG whole = (ULONG)(payload.size() & ~size_t(15));
    if (!aes_decrypt(kRetailKey, false, &xex[sec + 0x150], session, 16) ||
        !aes_decrypt(session, true, payload.data(), payload.data(), whole))
      return fail("falha ao decifrar o XEX");
  } else if (encryption != 0) {
    return fail("cifra do XEX não suportada");
  }

  // Cada bloco: u32 tamanho do seguinte, SHA-1 do seguinte, pedaços {u16 tamanho, dados} até um de tamanho 0.
  std::vector<uint8_t> stream;
  stream.reserve(payload.size());
  size_t p = 0;
  for (uint32_t block_size = be32(&xex[ff + 12]); block_size;) {
    if (p + block_size > payload.size() || block_size < 26) return fail("bloco comprimido truncado");
    const uint8_t* block = &payload[p];
    const uint32_t next = be32(block);
    for (size_t q = 24;;) {
      if (q + 2 > block_size) return fail("bloco comprimido inválido");
      const uint32_t chunk = be16(block + q);
      q += 2;
      if (!chunk) break;
      if (q + chunk > block_size) return fail("bloco comprimido inválido");
      stream.insert(stream.end(), block + q, block + q + chunk);
      q += chunk;
    }
    p += block_size;
    block_size = next;
  }
  image->assign(image_size, 0);
  return lzx_decompress(stream, *image, be32(&xex[ff + 8]), error);
}

}  // namespace xex_image
