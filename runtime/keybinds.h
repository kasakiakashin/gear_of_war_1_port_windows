// keybinds.h — ações do comando que o teclado e o rato podem acionar; guardadas no gow_config.ini como Key_<ação>=<tecla>.
#pragma once

#include <cstdint>
#include <string>

namespace keybinds {

enum Action {
  kMoveForward, kMoveBack, kMoveLeft, kMoveRight,
  kFire, kAim, kCoverRun, kMelee, kReload, kLook, kInteract, kTactical, kZoom, kCrouchL3,
  kWeapon1, kWeapon2, kWeapon3, kWeapon4,
  kLookUp, kLookDown, kLookLeft, kLookRight,
  kStart, kBack,
  kActionCount
};

struct ActionInfo {
  const char* key;       // Key_<key> no gow_config.ini
  const wchar_t* label;  // texto no menu F8
  uint16_t default_vk;
  uint16_t button;       // bit XINPUT_GAMEPAD_*; 0 = eixo ou gatilho
};

const ActionInfo& info(int action);
// "W", "Space", "Mouse1", "F5"...; "None" = sem tecla. VK_LBUTTON..VK_XBUTTON2 são os botões do rato.
std::string name_of(uint16_t vk);
uint16_t vk_of(const std::string& name);  // 0 se for desconhecido ou "None"
bool reserved(uint16_t vk);               // Esc, F8, F9 e F10 têm função fixa

}  // namespace keybinds
