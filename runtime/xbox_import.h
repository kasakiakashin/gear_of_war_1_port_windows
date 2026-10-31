// xbox_import.h — registo de implementações HLE das funções importadas de xboxkrnl.exe / xam.xex.
//
// Uma função importada é identificada pelo nome (tabela em import_bindings.inc). Implementações:
//   XBOX_IMPORT(KeTlsAlloc) { ...; ctx.r[3] = resultado; }        implementação real
//   XBOX_IMPORT_STUB(KeTlsAlloc) { STUBBED_IMPORT("KeTlsAlloc"); } stub (gerado em imports_stubs.cpp)
// Uma implementação real tem precedência sobre o stub do mesmo nome. Argumentos chegam em ctx.r[3..10]
// (inteiros/ponteiros convidados) e ctx.f[1..] (float); o retorno vai em ctx.r[3].
#pragma once

#include <cstdint>

#include "ppc_context.h"

namespace xbox {

using ImportFn = void (*)(PPCContext& ctx, uint8_t* mem_base);

struct ImportRegistrar {
  ImportRegistrar(const char* name, ImportFn fn, bool stub);
};

// Regista a primeira chamada de um stub (com o LR do chamador) e conta todas.
void stub_hit(const char* name, const PPCContext& ctx);

// Inicializa a tabela (lib, ordinal) -> implementação; devolve false se houver nomes desconhecidos.
bool init_imports();
// Escreve em stderr quantas vezes cada stub foi chamado.
void print_import_stats();

}  // namespace xbox

#define XBOX_IMPORT_IMPL(name, stub)                                             \
  static void xbox_import_##name(PPCContext& ctx, uint8_t* mem_base);            \
  static const ::xbox::ImportRegistrar xbox_reg_##name(#name, xbox_import_##name, stub); \
  static void xbox_import_##name([[maybe_unused]] PPCContext& ctx, [[maybe_unused]] uint8_t* mem_base)

#define XBOX_IMPORT(name) XBOX_IMPORT_IMPL(name, false)
#define XBOX_IMPORT_STUB(name) XBOX_IMPORT_IMPL(name, true)
#define STUBBED_IMPORT(name) ::xbox::stub_hit(name, ctx)
