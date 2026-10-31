#include "language.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "config.h"

namespace language {
namespace {

// O disco tem pacotes de falas e legendas para INT, DEU, ESM, ESN, FRA e ITA; CHI e KOR só têm o texto (Coalesced).
// Os restantes idiomas do Xbox 360 não existem neste disco: o jogo recorre ao inglês.
const Info kTable[] = {
    {"English", L"English (inglês)", 1, 103, "int"},
    {"German", L"Deutsch (alemão)", 3, 24, "deu"},
    {"French", L"Français (francês)", 4, 34, "fra"},
    {"Spanish", L"Español - España", 5, 31, "esn"},
    {"SpanishLatin", L"Español - América Latina", 5, 71, "esm"},
    {"Italian", L"Italiano", 6, 50, "ita"},
    {"Korean", L"한국어 (coreano, só texto)", 7, 56, "kor"},
    {"Chinese", L"中文 (chinês tradicional, só texto)", 8, 101, "chi"},
    {"Portuguese", L"Português (não existe no disco: inglês)", 9, 13, nullptr},
    {"Japanese", L"日本語 (não existe no disco: inglês)", 2, 53, nullptr},
    {"SimplifiedChinese", L"简体中文 (não existe no disco: inglês)", 10, 20, nullptr},
    {"Polish", L"Polski (não existe no disco: inglês)", 11, 82, nullptr},
    {"Russian", L"Русский (não existe no disco: inglês)", 12, 88, nullptr},
};

}  // namespace

const Info* table(int* count) {
  *count = (int)(sizeof kTable / sizeof kTable[0]);
  return kTable;
}

const Info& current() {
  static const Info& info = []() -> const Info& {
    const char* e = std::getenv("GOW_LANGUAGE");
    const std::string want = e ? e : config::get().language;
    for (const Info& i : kTable)
      if (_stricmp(i.key, want.c_str()) == 0) return i;
    std::fprintf(stderr, "[config] idioma desconhecido: %s (a usar inglês)\n", want.c_str());
    return kTable[0];
  }();
  return info;
}

}  // namespace language
