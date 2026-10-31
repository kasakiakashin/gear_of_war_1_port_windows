// xex_image.h — default.xex (XEX2: AES + LZX) -> imagem base PE, em memória. Porte de tools/xex2.py e tools/lzx.py.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xex_image {

// SHA-1 da imagem para a qual generated_src/ foi traduzido: o código compilado só serve para esta versão do jogo.
constexpr const char* kExpectedSha1 = "083d7d0ab2ae1ee6d9d536bc5ca987862ad8c8f7";
// SHA-1 do default.xex dessa versão: valida uma imagem de disco sem descodificar nada.
constexpr const char* kExpectedXexSha1 = "ee2f204979f9811e99c4e08b70e11de869d005c0";

bool decode(const std::vector<uint8_t>& xex, std::vector<uint8_t>* image, std::string* error);
std::string sha1_hex(const uint8_t* data, size_t size);

}  // namespace xex_image
