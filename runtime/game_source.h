// game_source.h — de onde vem o jogo: pasta extracted/ e/ou imagem de disco, e a imagem base do executável.
#pragma once

#include <cstdint>
#include <vector>

namespace game_source {

// Pasta de trabalho = pasta do gow.exe, quando o exe foi aberto de outro sítio sem configuração nem extracted/ à vista.
void init_working_dir();
// Encontra o jogo (extracted/, IsoPath, .iso na pasta, seletor de ficheiros) e devolve a imagem base.
// false = sem jogo (o jogador cancelou ou a imagem não é a certa; já foi avisado).
bool prepare(std::vector<uint8_t>* image);

}  // namespace game_source
