#include "keybinds.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace keybinds {
namespace {

const ActionInfo kActions[kActionCount] = {
    {"MoveForward", L"Andar para a frente", 'W', 0},
    {"MoveBack", L"Andar para trás", 'S', 0},
    {"MoveLeft", L"Andar para a esquerda", 'A', 0},
    {"MoveRight", L"Andar para a direita", 'D', 0},
    {"Fire", L"Atirar [RT]", VK_LBUTTON, 0},
    {"Aim", L"Mirar [LT]", VK_RBUTTON, 0},
    {"CoverRun", L"Correr / Cobertura [A]", VK_SPACE, 0x1000},
    {"Melee", L"Serra / Corpo a corpo [B]", 'E', 0x2000},
    {"Reload", L"Recarregar [RB]", 'R', 0x0200},
    {"Look", L"Olhar [Y]", 'Q', 0x8000},
    {"Interact", L"Ação [X]", 'C', 0x4000},
    {"Tactical", L"Mapa tático / Granada [LB]", VK_SHIFT, 0x0100},
    {"Zoom", L"Zoom [R3]", VK_MBUTTON, 0x0080},
    {"Crouch", L"Clique do analógico [L3]", VK_CONTROL, 0x0040},
    {"Weapon1", L"Arma 1 [direcional cima]", '1', 0x0001},
    {"Weapon2", L"Arma 2 [direcional direita]", '2', 0x0008},
    {"Weapon3", L"Arma 3 [direcional baixo]", '3', 0x0002},
    {"Weapon4", L"Arma 4 [direcional esquerda]", '4', 0x0004},
    {"LookUp", L"Câmara para cima", 'I', 0},
    {"LookDown", L"Câmara para baixo", 'K', 0},
    {"LookLeft", L"Câmara para a esquerda", 'J', 0},
    {"LookRight", L"Câmara para a direita", 'L', 0},
    {"Start", L"Start", VK_RETURN, 0x0010},
    {"Back", L"Back", VK_BACK, 0x0020},
};

const struct {
  uint16_t vk;
  const char* name;
} kNames[] = {
    {VK_LBUTTON, "Mouse1"}, {VK_RBUTTON, "Mouse2"}, {VK_MBUTTON, "Mouse3"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
    {VK_SPACE, "Space"},    {VK_RETURN, "Enter"},   {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},         {VK_SHIFT, "Shift"},
    {VK_CONTROL, "Ctrl"},   {VK_MENU, "Alt"},       {VK_CAPITAL, "CapsLock"}, {VK_UP, "Up"},         {VK_DOWN, "Down"},
    {VK_LEFT, "Left"},      {VK_RIGHT, "Right"},    {VK_INSERT, "Insert"},  {VK_DELETE, "Delete"},   {VK_HOME, "Home"},
    {VK_END, "End"},        {VK_PRIOR, "PageUp"},   {VK_NEXT, "PageDown"},
};

}  // namespace

const ActionInfo& info(int action) { return kActions[action]; }

std::string name_of(uint16_t vk) {
  if (!vk) return "None";
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
  for (const auto& n : kNames)
    if (n.vk == vk) return n.name;
  char b[16];
  if (vk >= VK_F1 && vk <= VK_F24) std::snprintf(b, sizeof b, "F%d", vk - VK_F1 + 1);
  else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) std::snprintf(b, sizeof b, "Num%d", vk - VK_NUMPAD0);
  else std::snprintf(b, sizeof b, "VK%02X", vk);  // pontuação: depende do esquema do teclado
  return b;
}

uint16_t vk_of(const std::string& name) {
  if (name.size() == 1) {
    const char c = (char)std::toupper((unsigned char)name[0]);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) return (uint16_t)c;
  }
  for (const auto& n : kNames)
    if (_stricmp(n.name, name.c_str()) == 0) return n.vk;
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && std::isdigit((unsigned char)name[1])) {
    const int f = std::atoi(name.c_str() + 1);
    if (f >= 1 && f <= 24) return (uint16_t)(VK_F1 + f - 1);
  }
  if (_strnicmp(name.c_str(), "Num", 3) == 0 && name.size() == 4 && std::isdigit((unsigned char)name[3]))
    return (uint16_t)(VK_NUMPAD0 + name[3] - '0');
  if (_strnicmp(name.c_str(), "VK", 2) == 0 && name.size() == 4) return (uint16_t)std::strtoul(name.c_str() + 2, nullptr, 16);
  return 0;
}

bool reserved(uint16_t vk) { return vk == VK_ESCAPE || vk == VK_F8 || vk == VK_F9 || vk == VK_F10; }

}  // namespace keybinds
