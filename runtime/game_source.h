// game_source.h — de onde vem o jogo: pasta extracted/ e/ou imagem de disco, e a imagem base do executável.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game_source {

// Pasta de trabalho = pasta do gow.exe, quando o exe foi aberto de outro sítio sem configuração nem extracted/ à vista.
void init_working_dir();
// Encontra o jogo (extracted/, IsoPath, .iso na pasta, seletor de ficheiros) e devolve a imagem base.
// false = sem jogo (o jogador cancelou ou a imagem não é a certa; já foi avisado).
bool prepare(std::vector<uint8_t>* image);
// Procura o jogo sem imagem base (para o launcher); interactive = pode abrir o seletor de ficheiros e avisos.
bool locate(bool interactive);
bool choose_iso();                         // seletor de ficheiros; grava IsoPath
std::wstring status();                     // texto para o launcher
bool has_flag(const wchar_t* name);        // ex.: L"--skip-launcher"
std::wstring option(const wchar_t* name);  // valor a seguir a uma opção, ex.: L"--pack"

}  // namespace game_source
