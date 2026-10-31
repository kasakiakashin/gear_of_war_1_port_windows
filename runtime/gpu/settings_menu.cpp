// settings_menu.cpp — menu F8: painel Win32 por cima da janela do jogo, na mesma thread do ciclo de mensagens.
//
// As alterações aplicam-se logo: o renderer compara ResolutionScale/FSR com a config a cada frame, o rato lê-a a cada
// consulta. Cada alteração é gravada no gow_config.ini. O jogo continua a correr por trás, sem receber o teclado (a
// janela principal perde o foco), e o rato solta-se ao abrir e volta a ser capturado ao fechar se estava capturado.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "config.h"
#include "gpu/window.h"
#include "net/tunnel.h"

namespace window::menu {
namespace {

enum Id { kScale = 101, kFullscreen, kFsr, kStrength, kStrengthText, kMouse, kMouseText, kHost, kPaste, kStatus, kClose };
constexpr UINT_PTR kTimer = 1;
constexpr float kScales[] = {1.0f, 1.5f, 2.0f, 3.0f};
const wchar_t* const kScaleNames[] = {L"1.0x  (1280×720)", L"1.5x  (1920×1080)", L"2.0x  (2560×1440)", L"3.0x  (3840×2160)"};
constexpr COLORREF kBack = RGB(28, 28, 30), kText = RGB(235, 235, 235);

HWND g_menu = nullptr;
HFONT g_font = nullptr, g_bold = nullptr;
HBRUSH g_brush = nullptr;
bool g_recapture = false;

std::wstring widen(const std::string& s) {
  std::wstring w(s.size(), L'\0');
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
  w.resize(n > 0 ? n : 0);
  return w;
}

void set_text(HWND h, int id, const wchar_t* fmt, double v) {
  wchar_t b[64];
  swprintf(b, 64, fmt, v);
  SetDlgItemTextW(h, id, b);
}

void add(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int ht, HFONT font = nullptr) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, ht, p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
}

void update_network(HWND h) {
  SetDlgItemTextW(h, kStatus, widen(net::menu_status()).c_str());
  SetDlgItemTextW(h, kHost, net::is_joined() ? L"Copiar Código de Resposta" : L"Hospedar e Copiar Link");
}

void build(HWND h) {
  const config::Config& c = config::get();
  int y = 12;
  add(h, L"STATIC", L"Vídeo", 0, 0, 16, y, 340, 22, g_bold);
  y += 28;
  add(h, L"STATIC", L"Resolução interna", 0, 0, 16, y + 3, 140, 20);
  add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kScale, 160, y, 196, 200);
  size_t sel = 0;
  for (size_t i = 0; i < 4; i++) {
    SendDlgItemMessageW(h, kScale, CB_ADDSTRING, 0, (LPARAM)kScaleNames[i]);
    if (std::fabs(kScales[i] - c.resolution_scale) < std::fabs(kScales[sel] - c.resolution_scale)) sel = i;
  }
  SendDlgItemMessageW(h, kScale, CB_SETCURSEL, sel, 0);
  y += 32;
  add(h, L"BUTTON", L"Ecrã inteiro", BS_AUTOCHECKBOX | WS_TABSTOP, kFullscreen, 16, y, 340, 22);
  CheckDlgButton(h, kFullscreen, window::is_fullscreen() ? BST_CHECKED : BST_UNCHECKED);
  y += 26;
  add(h, L"BUTTON", L"Nitidez / FSR 1.0", BS_AUTOCHECKBOX | WS_TABSTOP, kFsr, 16, y, 340, 22);
  CheckDlgButton(h, kFsr, c.sharpening ? BST_CHECKED : BST_UNCHECKED);
  y += 28;
  add(h, L"STATIC", L"", 0, kStrengthText, 16, y + 3, 140, 20);
  add(h, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kStrength, 156, y, 204, 26);
  SendDlgItemMessageW(h, kStrength, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
  SendDlgItemMessageW(h, kStrength, TBM_SETPOS, TRUE, std::lround(c.sharpening_strength * 100));
  set_text(h, kStrengthText, L"Força: %.2f", c.sharpening_strength);
  y += 30;
  add(h, L"STATIC", L"", 0, kMouseText, 16, y + 3, 140, 20);
  add(h, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, kMouse, 156, y, 204, 26);
  SendDlgItemMessageW(h, kMouse, TBM_SETRANGE, TRUE, MAKELPARAM(10, 500));
  SendDlgItemMessageW(h, kMouse, TBM_SETPOS, TRUE, std::lround(c.mouse_sensitivity * 100));
  set_text(h, kMouseText, L"Sensib. do rato: %.2f", c.mouse_sensitivity);
  y += 40;
  add(h, L"STATIC", L"Multiplayer Co-op", 0, 0, 16, y, 340, 22, g_bold);
  y += 28;
  add(h, L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, kHost, 16, y, 340, 30);
  y += 36;
  add(h, L"BUTTON", L"Colar Link da Área de Transferência e Conectar", BS_PUSHBUTTON | WS_TABSTOP, kPaste, 16, y, 340, 30);
  y += 38;
  add(h, L"STATIC", L"", 0, kStatus, 16, y, 340, 54);
  y += 58;
  add(h, L"BUTTON", L"Fechar (F8)", BS_PUSHBUTTON | WS_TABSTOP, kClose, 236, y, 120, 28);
  update_network(h);
}

void centre_over_owner(HWND h) {
  RECT o, r;
  GetWindowRect(GetWindow(h, GW_OWNER), &o);
  GetWindowRect(h, &r);
  const int w = r.right - r.left, ht = r.bottom - r.top;
  SetWindowPos(h, nullptr, (o.left + o.right - w) / 2, (o.top + o.bottom - ht) / 2, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      build(h);
      SetTimer(h, kTimer, 500, nullptr);
      return 0;
    case WM_TIMER:
      update_network(h);
      return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      SetTextColor((HDC)wp, kText);
      SetBkColor((HDC)wp, kBack);
      return (LRESULT)g_brush;
    case WM_ERASEBKGND: {
      RECT r;
      GetClientRect(h, &r);
      FillRect((HDC)wp, &r, g_brush);
      return 1;
    }
    case WM_HSCROLL: {
      config::Config& c = config::edit();
      const int id = GetDlgCtrlID((HWND)lp);
      const int pos = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
      if (id == kStrength) {
        c.sharpening_strength = pos / 100.0f;
        set_text(h, kStrengthText, L"Força: %.2f", c.sharpening_strength);
      } else if (id == kMouse) {
        c.mouse_sensitivity = pos / 100.0f;
        set_text(h, kMouseText, L"Sensib. do rato: %.2f", c.mouse_sensitivity);
      }
      if (LOWORD(wp) == SB_ENDSCROLL) config::save();
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      config::Config& c = config::edit();
      if (id == kScale && code == CBN_SELCHANGE) {
        const LRESULT i = SendDlgItemMessageW(h, kScale, CB_GETCURSEL, 0, 0);
        if (i >= 0 && i < 4) c.resolution_scale = kScales[i];
        config::save();
      } else if (id == kFullscreen && code == BN_CLICKED) {
        const bool on = IsDlgButtonChecked(h, kFullscreen) == BST_CHECKED;
        if (on != window::is_fullscreen()) window::toggle_fullscreen_now();
        c.fullscreen = on;
        config::save();
        centre_over_owner(h);
      } else if (id == kFsr && code == BN_CLICKED) {
        c.sharpening = IsDlgButtonChecked(h, kFsr) == BST_CHECKED;
        config::save();
      } else if (id == kHost && code == BN_CLICKED) {
        net::on_copy_key();
        update_network(h);
      } else if (id == kPaste && code == BN_CLICKED) {
        net::on_paste_key();
        update_network(h);
      } else if (id == kClose && code == BN_CLICKED) {
        toggle(GetWindow(h, GW_OWNER));
      }
      return 0;
    }
    case WM_CLOSE:
      toggle(GetWindow(h, GW_OWNER));
      return 0;
    case WM_DESTROY:
      KillTimer(h, kTimer);
      g_menu = nullptr;
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void init_once() {
  static bool done = false;
  if (done) return;
  done = true;
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  g_brush = CreateSolidBrush(kBack);
  g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  g_bold = CreateFontW(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = menu_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"GowMenu";
  RegisterClassExW(&wc);
}

}  // namespace

bool is_open() { return g_menu != nullptr; }

void toggle(void* owner_ptr) {
  HWND owner = static_cast<HWND>(owner_ptr);
  if (g_menu) {
    DestroyWindow(g_menu);
    SetForegroundWindow(owner);
    SetFocus(owner);
    if (g_recapture) window::capture_mouse(true);
    g_recapture = false;
    return;
  }
  init_once();
  g_recapture = window::mouse_captured();
  window::capture_mouse(false);
  RECT r{0, 0, 372, 440};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOOLWINDOW);
  g_menu = CreateWindowExW(WS_EX_TOOLWINDOW, L"GowMenu", L"Gears of War - Menu (F8)", style, 0, 0, r.right - r.left, r.bottom - r.top,
                           owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!g_menu) return;
  centre_over_owner(g_menu);
  ShowWindow(g_menu, SW_SHOW);
  SetForegroundWindow(g_menu);
  SetFocus(GetDlgItem(g_menu, kScale));
}

bool pre_translate(void* msg_ptr) {
  MSG* m = static_cast<MSG*>(msg_ptr);
  if (!g_menu) return false;
  if (m->message == WM_KEYDOWN && (m->wParam == VK_F8 || m->wParam == VK_ESCAPE) && (m->hwnd == g_menu || IsChild(g_menu, m->hwnd))) {
    if (!(m->lParam & (1 << 30))) toggle(GetWindow(g_menu, GW_OWNER));
    return true;
  }
  return IsDialogMessageW(g_menu, m) != 0;
}

}  // namespace window::menu
