// xex_image.h — default.xex (XEX2: AES + LZX) -> imagem base PE, em memória. Porte de tools/xex2.py e tools/lzx.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xex_image {

// kExpectedSha1: SHA-1 da imagem para a qual o código foi traduzido (o código compilado só serve para essa versão do jogo).
// kExpectedXexSha1: SHA-1 do default.xex dessa versão (valida uma imagem de disco sem descodificar nada).
// kExpectedCodeSha1: SHA-1 da imagem com o recurso HashSec a zeros. Traduções e mods para RGH reescrevem essa tabela de hashes
// dos ficheiros no seu default.xex; o código é o mesmo, por isso uma imagem que só difere nela também é aceite.
#if GOW_TU5
// Title update 5 (0.0.5.1), traduzida para generated_src_tu5/ (cópia RGH com a HashSec de uma tradução).
constexpr const char* kExpectedSha1 = "abe96b6c5248a4e1b9899aa248ea0a8c1977e9b7";
constexpr const char* kExpectedXexSha1 = "36a909f2bb3a73a65ddec0cf79edc694746bcae6";
constexpr const char* kExpectedCodeSha1 = "af003ccf23386dcec157d22de5b7ce2349c74feb";
constexpr size_t kHashSecRva = 0x10E0000, kHashSecSize = 0x8B0;
constexpr const char* kVersion = "0.0.5.1";
#else
constexpr const char* kExpectedSha1 = "083d7d0ab2ae1ee6d9d536bc5ca987862ad8c8f7";
constexpr const char* kExpectedXexSha1 = "ee2f204979f9811e99c4e08b70e11de869d005c0";
constexpr const char* kExpectedCodeSha1 = "b7df854b4c897c11d26c45daade7f201bc76be71";
constexpr size_t kHashSecRva = 0xCD0000, kHashSecSize = 0x933;
constexpr const char* kVersion = "0.0.0.1";
#endif

bool decode(const std::vector<uint8_t>& xex, std::vector<uint8_t>* image, std::string* error);
std::string sha1_hex(const uint8_t* data, size_t size);
// Imagem / default.xex da versão traduzida (inteira, ou só com a HashSec diferente). Com `image`, devolve a imagem descodificada.
bool image_supported(const std::vector<uint8_t>& image);
bool xex_supported(const std::vector<uint8_t>& xex, std::vector<uint8_t>* image = nullptr);

// Versões com tradução: cada gow*.exe corre uma e reconhece a outra, para abrir o executável irmão (mesma pasta) quando o
// jogo escolhido é dessa versão.
struct KnownVersion {
  const char* version;
  const char* xex_sha1;
  const char* code_sha1;  // imagem com a HashSec a zeros
  size_t image_size, hashsec_rva, hashsec_size;
  const wchar_t* exe;
};
constexpr KnownVersion kKnown[] = {
    {"0.0.0.1", "ee2f204979f9811e99c4e08b70e11de869d005c0", "b7df854b4c897c11d26c45daade7f201bc76be71", 13500416, 0xCD0000, 0x933,
     L"gow.exe"},
    {"0.0.5.1", "36a909f2bb3a73a65ddec0cf79edc694746bcae6", "af003ccf23386dcec157d22de5b7ce2349c74feb", 17760256, 0x10E0000, 0x8B0,
     L"gow_tu5.exe"},
};
#if GOW_TU5
constexpr int kSelf = 1;
#else
constexpr int kSelf = 0;
#endif
// Índice em kKnown do default.xex (descodifica-o se o SHA-1 do ficheiro não bater), ou -1.
int identify(const std::vector<uint8_t>& xex);

}  // namespace xex_image
