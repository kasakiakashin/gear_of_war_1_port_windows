// window.h — janela nativa Win32 do jogo (thread própria com o ciclo de mensagens) e comando de teclado/rato.
//
// Teclado/rato como comando do jogador 1, somados ao XInput (só com a janela em foco); as teclas vêm do gow_config.ini
// (keybinds.h). Por omissão: WASD = analógico esquerdo, Espaço = A, E/F = B, C = X, Q = Y, R = RB, Shift = LB,
//   1/2/3/4 ou setas = direcional, Enter = Start, Backspace = Back, Ctrl = clique L3, botão do meio = clique R3, IJKL = analógico direito.
//   Clicar na janela captura o rato (movimento = analógico direito, esquerdo = RT, direito = LT); Esc ou F10 soltam.
//   Com o rato solto, Esc = B (voltar nos menus). Alt+Enter alterna o ecrã inteiro. F8 abre o menu de configurações,
//   F9 copia o código do túnel.
#pragma once

#include <cstdint>

namespace window {

struct PadState {
  uint16_t buttons = 0;  // bits XINPUT_GAMEPAD_* (iguais aos do Xbox 360)
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};

// Cria a janela (GOW_NO_WINDOW=1 desliga); devolve o HWND como void* (nullptr sem janela). O tamanho e o ecrã inteiro
// vêm do gow_config.ini (base = width × height, multiplicado por ResolutionScale).
void* create(int width, int height);
void* handle();
bool closed();  // o utilizador fechou a janela
// Tamanho atual da área cliente (o renderer redimensiona o swapchain para ele).
void client_size(uint32_t& w, uint32_t& h);
// Estado atual do teclado/rato como comando; false se a janela não tiver foco ou nada estiver premido.
bool keyboard_pad(PadState& out);
void set_title(const char* title);  // UTF-8

// Usados pelo menu F8 (thread da janela).
bool is_fullscreen();
void toggle_fullscreen_now();
bool mouse_captured();
void capture_mouse(bool on);

// settings_menu.cpp
namespace menu {
bool is_open();
void toggle(void* owner_hwnd);        // abre/fecha
bool pre_translate(void* msg);        // true se a mensagem foi tratada pelo menu
}  // namespace menu

}  // namespace window
