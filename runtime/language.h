// language.h — idioma pedido no gow_config.ini (Language=) e os valores do Xbox 360 correspondentes.
#pragma once

#include <cstdint>
#include <string>

namespace language {

struct Info {
  const char* key;      // valor de Language= no gow_config.ini
  const wchar_t* name;  // nome no menu F8
  uint32_t xbox;        // XCONFIG_USER_LANGUAGE / XGetLanguage
  uint8_t country;      // XCONFIG_USER_COUNTRY
  const char* suffix;   // sufixo dos ficheiros de localização do jogo; nulo = o disco não tem este idioma
};

const Info* table(int* count);
const Info& current();  // lido uma vez: o jogo escolhe o idioma ao arrancar

}  // namespace language
