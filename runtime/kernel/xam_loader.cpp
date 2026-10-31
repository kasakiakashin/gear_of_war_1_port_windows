// xam_loader.cpp — dados de arranque do título (launch data) e consultas da consola (região, AV pack, idioma).
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <string>

#include "kernel/diag.h"
#include "kernel/kernel.h"
#include "language.h"
#include <cstring>

using namespace kernel;

namespace {
constexpr uint32_t kErrorInvalidParameter = 0x57, kErrorNotFound = 0x490;
}

// Launch data: sem GOW_LAUNCH_DATA o título foi arrancado do dashboard (não há). O UE3 deste título usa os launch
// data como linha de comandos (sub_82218F98), por isso GOW_LAUNCH_DATA="SP_Prison_P -nomovie" carrega um mapa
// diretamente — atalho de depuração para chegar a um nível sem passar pelos menus.
namespace {
const std::string& launch_data() {
  static const std::string data = [] {
    const char* e = std::getenv("GOW_LAUNCH_DATA");
    return std::string(e ? e : "");
  }();
  return data;
}
}  // namespace

// XamLoaderGetLaunchDataSize(PDWORD Size)
XBOX_IMPORT(XamLoaderGetLaunchDataSize) {
  if (!arg(ctx, 0)) return ret(ctx, kErrorInvalidParameter);
  uint32_t n = launch_data().empty() ? 0 : (uint32_t)launch_data().size() + 1;
  ppc_st32(mem_base, arg(ctx, 0), n);
  if (n) std::fprintf(stderr, "[xam] launch data: \"%s\"\n", launch_data().c_str());
  ret(ctx, n ? 0 : kErrorNotFound);
}

// XamLoaderGetLaunchData(PVOID Buffer, DWORD Size)
XBOX_IMPORT(XamLoaderGetLaunchData) {
  uint32_t buf = arg(ctx, 0), size = arg(ctx, 1);
  if (launch_data().empty()) return ret(ctx, kErrorNotFound);
  uint32_t n = std::min<uint32_t>(size, (uint32_t)launch_data().size() + 1);
  for (uint32_t i = 0; i < n; i++) ppc_st8(mem_base, buf + i, i < launch_data().size() ? (uint8_t)launch_data()[i] : 0);
  ret(ctx, 0);
}

// Todas as regiões; o jogo só escolhe os pacotes ESM (espanhol da América Latina) com a região das Américas.
XBOX_IMPORT(XGetGameRegion) { ret(ctx, std::strcmp(language::current().key, "SpanishLatin") == 0 ? 0x00FF : 0xFFFF); }
XBOX_IMPORT(XGetAVPack) { ret(ctx, 6); }           // HDMI
XBOX_IMPORT(XGetLanguage) { ret(ctx, language::current().xbox); }  // igual a ExGetXConfigSetting

// Erro fatal de leitura do disco: na consola mostra a UI e o título termina. Aqui regista quem pediu e sai.
XBOX_IMPORT(XamShowDirtyDiscErrorUI) {  // (DWORD UserIndex, ...) — r4 é um código interno do título
  std::fprintf(stderr, "[xam] XamShowDirtyDiscErrorUI(%u, 0x%X)\n", arg(ctx, 0), arg(ctx, 1));
  diag::guest_backtrace("dirty disc", ctx.r[1], ctx.lr);
  diag::crash_report();
  std::fflush(stderr);
  std::_Exit(4);
}

// XamLoaderLaunchTitle(PSZ Name, DWORD Flags) / XamLoaderTerminateTitle(): não voltam.
XBOX_IMPORT(XamLoaderLaunchTitle) {
  const char* name = arg(ctx, 0) ? reinterpret_cast<const char*>(PPC_HOST(mem_base, arg(ctx, 0))) : "(dashboard)";
  std::fprintf(stderr, "[xam] XamLoaderLaunchTitle(\"%s\", 0x%X): título terminou\n", name, arg(ctx, 1));
  std::fflush(stderr);
  std::_Exit(0);
}
XBOX_IMPORT(XamLoaderTerminateTitle) {
  std::fprintf(stderr, "[xam] XamLoaderTerminateTitle: título terminou\n");
  std::fflush(stderr);
  std::_Exit(0);
}
