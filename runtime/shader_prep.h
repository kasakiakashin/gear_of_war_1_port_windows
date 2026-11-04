// shader_prep.h — compila de antemão todos os shaders do jogo, a partir dos pacotes do disco, para a cache em disco.
//
// Os pacotes UE3 do disco (WarGame/CookedXenon/*.xxx) guardam os shaders dos materiais dentro de contentores do D3D do Xbox
// (assinatura 0x102A1100 = pixel, 0x102A1101 = vértices; +4 = deslocamento do código, +8 = tamanho), em blocos LZO de 128 KiB.
// Não é preciso arrancar o jogo: lê-se cada pacote (da ISO ou de extracted/), descomprime-se, extrai-se o microcódigo, traduz-se
// para HLSL e compila-se com todas as threads. O renderer encontra depois o DXBC pela mesma chave (hash do HLSL).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shader_prep {

struct Progress {
  enum Stage { kIdle, kScanning, kCompiling, kDone, kFailed } stage = kIdle;
  uint32_t packages_done = 0, packages_total = 0;
  uint32_t found = 0;         // shaders distintos encontrados no disco
  uint32_t compiled = 0, to_compile = 0;
  std::string message;        // UTF-8
};

// A cache já corresponde a este gow.exe: marcador gravado no fim de uma preparação completa, ou um prebuilt_shaders.bin feito
// com este mesmo exe ao lado dele.
bool up_to_date();
// DXBC pela chave (gpu::shader_cache_key): do prebuilt_shaders.bin, lido no sítio (sem instalar nada), ou de user_data/shader_cache.
bool load_cached(uint64_t key, std::vector<char>* out);
void start();              // em fundo; não faz nada se já estiver a correr
Progress progress();
bool running();
void request_stop();       // ao fechar o launcher a meio
// Linha de comandos: gow.exe --prepare-shaders [--pack ficheiro]; devolve o código de saída.
int run_cli(const std::wstring& pack_out);

// Pacote de shaders pré-compilados que pode vir no zip (prebuilt_shaders.bin, junto do gow.exe): "GOWSPK01", assinatura do exe,
// número de entradas, índice {chave u64, tamanho u32} e os DXBC seguidos.
bool write_pack(const std::filesystem::path& file);

}  // namespace shader_prep
