// shader_compile.h — HLSL → DXBC com o d3dcompiler_47 do Windows, partilhado pelo renderer e pelo pré-compilador do launcher.
// O bytecode não depende da placa gráfica: o que fica na cache em disco (user_data/shader_cache/<hash do HLSL>.dxbc) serve a qualquer GPU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

struct ID3D10Blob;

namespace gpu {

// Carrega o d3dcompiler_47.dll na 1ª chamada. Tenta otimização 1, depois sem otimização, depois 3 (o FXC tem erros internos que
// dependem das passagens). nullptr se falhar.
ID3D10Blob* compile_hlsl(const std::string& src, const char* target, const char* name, std::string* err);
bool compiler_available();
// Ficheiro da cache em disco para este HLSL.
uint64_t shader_cache_key(const std::string& hlsl);  // hash do HLSL: nome do .dxbc e chave no prebuilt_shaders.bin
std::filesystem::path shader_cache_file(const std::string& hlsl);
// Microcódigo Xenos válido a partir de `be` (dwords big-endian): nº de dwords até ao fim das instruções, ou 0. `vertex` = há um
// ALLOC de posição, que só o VS faz.
uint32_t microcode_size(const uint32_t* be, size_t left, bool* vertex);

}  // namespace gpu
