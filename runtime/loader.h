// loader.h — carrega a imagem do default.xex na memória do convidado e liga os imports.
#pragma once

#include <cstdint>
#include <vector>

namespace loader {

// Confirma a região da imagem e copia a imagem base para 0x82000000.
bool load_image(const std::vector<uint8_t>& image);
// Preenche os registos de import: thunk das funções e objetos das variáveis exportadas do kernel.
bool bind_imports();

}  // namespace loader
