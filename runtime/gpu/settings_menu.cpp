// settings_menu.cpp — menu F8: painel Win32 escuro com 4 abas, por cima da janela do jogo, na thread do ciclo de mensagens.
//
// As alterações aplicam-se logo: o renderer compara ResolutionScale/FSR com a config a cada frame, e o rato, as teclas e o
// volume leem-na a cada consulta. O idioma só muda ao reiniciar. Cada alteração é gravada no gow_config.ini. O jogo continua a
// correr por trás, sem receber o teclado (a janela principal perde o foco), e o rato solta-se ao abrir e volta a ser capturado
// ao fechar se estava capturado.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "gpu/window.h"
#include "keybinds.h"
#include "language.h"
#include "net/tunnel.h"

namespace window::menu {
namespace {

enum Id {
  kTabFirst = 100,  // 100..103
  kScale = 110, kScalePreview, kFullscreen, kFsr, kStrength, kStrengthText, kVsync, kFpsLimit,
  kVolume = 130, kVolumeText, kLanguage, kLanguageNote,
  kMouse = 150, kMouseText, kInvertY, kRestoreKeys, kKeysNote,
  kKeyFirst = 200,  // 200..200+kActionCount
  kCreateRoom = 300, kInviteBox, kCopyInvite, kCopyFeedback, kJoinBox, kConnect, kDisconnect, kNetState, kNetDetail,
  kClose = 400
};
constexpr int kTabs = 4, kWidth = 860, kHeight = 660, kContentTop = 92, kContentLeft = 18, kContentW = kWidth - 36;
constexpr UINT_PTR kTimer = 1;
constexpr float kScales[] = {1.0f, 1.5f, 2.0f, 3.0f};
const wchar_t* const kScaleNames[] = {L"1.0×  (720p)", L"1.5×  (1080p)", L"2.0×  (1440p)", L"3.0×  (4K)"};
const wchar_t* const kTabNames[kTabs] = {L"VÍDEO", L"ÁUDIO E IDIOMA", L"CONTROLOS", L"MULTIPLAYER CO-OP"};
constexpr int kFpsChoices[] = {30, 60, 120, 0};
const wchar_t* const kFpsNames[] = {L"30 FPS (como no Xbox 360)", L"60 FPS", L"120 FPS", L"Ilimitado"};

constexpr COLORREF kBack = RGB(16, 15, 16), kPanel = RGB(32, 29, 30), kField = RGB(14, 13, 14), kCrimson = RGB(165, 18, 28),
                   kCrimsonHot = RGB(205, 32, 42), kButton = RGB(58, 52, 53), kText = RGB(236, 232, 230),
                   kDim = RGB(162, 154, 152), kGreen = RGB(92, 200, 110), kAmber = RGB(235, 176, 60), kLine = RGB(70, 60, 60);

HWND g_menu = nullptr;
HFONT g_font = nullptr, g_bold = nullptr, g_title = nullptr, g_mono = nullptr;
HBRUSH g_back = nullptr, g_panel = nullptr, g_field = nullptr;
bool g_recapture = false;
bool g_embedded = false;  // dentro do launcher: sem botão Fechar, F8 e Esc não fecham
int g_tab = 0;
std::vector<HWND> g_controls[kTabs];
int g_build_tab = 0;
int g_capturing = -1;   // ação à espera de uma tecla
ULONGLONG g_copied_until = 0;
std::string g_shown_code;

std::wstring widen(const std::string& s) {
  std::wstring w(s.size(), L'\0');
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
  w.resize(n > 0 ? n : 0);
  return w;
}

std::string narrow(const std::wstring& w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
  return s;
}

void set_text(HWND h, int id, const wchar_t* fmt, double v) {
  wchar_t b[96];
  swprintf(b, 96, fmt, v);
  SetDlgItemTextW(h, id, b);
}

HWND add(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int ht, HFONT font = nullptr) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, x, y, w, ht, p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)(font ? font : g_font), TRUE);
  if (g_build_tab >= 0) g_controls[g_build_tab].push_back(c);
  return c;
}
HWND label(HWND p, const wchar_t* text, int x, int y, int w, int ht = 22, HFONT font = nullptr, int id = 0) {
  return add(p, L"STATIC", text, SS_NOPREFIX, id, x, y, w, ht, font);
}
HWND button(HWND p, const wchar_t* text, int id, int x, int y, int w, int ht) {
  return add(p, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, id, x, y, w, ht);
}
HWND slider(HWND p, int id, int x, int y, int w, int lo, int hi, int pos) {
  HWND s = add(p, TRACKBAR_CLASSW, L"", TBS_NOTICKS | WS_TABSTOP, id, x, y, w, 28);
  SendMessageW(s, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
  SendMessageW(s, TBM_SETPOS, TRUE, pos);
  return s;
}
HWND check(HWND p, const wchar_t* text, int id, int x, int y, int w, bool on) {
  HWND c = add(p, L"BUTTON", text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y, w, 24);
  SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  return c;
}

// Painéis por aba, desenhados em WM_PAINT.
struct Panel {
  int tab, x, y, w, h;
  const wchar_t* title;
};
std::vector<Panel> g_panels;
void panel(int tab, int x, int y, int w, int h, const wchar_t* title) { g_panels.push_back({tab, x, y, w, h, title}); }

void update_key_labels(HWND h) {
  const config::Config& c = config::get();
  for (int a = 0; a < keybinds::kActionCount; a++) {
    const std::wstring t = a == g_capturing ? L"Prima a nova tecla..." : widen(keybinds::name_of(c.keys[a]));
    SetDlgItemTextW(h, kKeyFirst + a, t.c_str());
    InvalidateRect(GetDlgItem(h, kKeyFirst + a), nullptr, FALSE);
  }
}

void update_preview(HWND h) {
  const config::Config& c = config::get();
  uint32_t cw = 0, ch = 0;
  window::client_size(cw, ch);
  wchar_t b[128];
  if (cw && ch)
    swprintf(b, 128, L"Render interno: %d × %d px   |   Janela: %u × %u px", (int)std::lround(1280 * c.resolution_scale),
             (int)std::lround(720 * c.resolution_scale), cw, ch);
  else
    swprintf(b, 128, L"Render interno: %d × %d px", (int)std::lround(1280 * c.resolution_scale), (int)std::lround(720 * c.resolution_scale));
  SetDlgItemTextW(h, kScalePreview, b);
}

void update_network(HWND h) {
  const net::MenuState s = net::menu_state();
  const wchar_t* state = s.phase == net::MenuState::kOff ? L"●  DESCONECTADO"
                         : s.phase == net::MenuState::kWaiting ? L"●  AGUARDANDO AMIGO"
                                                               : L"●  CONECTADO";
  std::wstring line = state;
  wchar_t b[96];
  if (s.phase == net::MenuState::kConnected) {
    if (s.peers > 1 || s.hosting) {
      swprintf(b, 96, L"  |  %d jogador(es)", s.peers);
      line += b;
    }
    if (s.ping_ms >= 0) {
      swprintf(b, 96, L"  |  Ping: %d ms", s.ping_ms);
      line += b;
    } else {
      line += L"  |  Ping: a medir...";
    }
  }
  SetDlgItemTextW(h, kNetState, line.c_str());
  SetDlgItemTextW(h, kNetDetail, widen(s.message).c_str());
  if (s.code != g_shown_code) {
    g_shown_code = s.code;
    SetDlgItemTextW(h, kInviteBox, widen(s.code).c_str());
  }
  SetDlgItemTextW(h, kCopyInvite, s.joined ? L"Copiar Código de Resposta (F9)" : L"Copiar Convite (F9)");
  EnableWindow(GetDlgItem(h, kCopyInvite), !s.code.empty());
  EnableWindow(GetDlgItem(h, kCreateRoom), s.phase == net::MenuState::kOff);
  EnableWindow(GetDlgItem(h, kDisconnect), s.phase != net::MenuState::kOff);
  if (g_copied_until && GetTickCount64() > g_copied_until) {
    g_copied_until = 0;
    SetDlgItemTextW(h, kCopyFeedback, L"");
  }
  InvalidateRect(GetDlgItem(h, kNetState), nullptr, TRUE);
}

void build_video(HWND h) {
  const config::Config& c = config::get();
  g_build_tab = 0;
  const int x = kContentLeft + 18;
  int y = kContentTop + 44;
  panel(0, kContentLeft, kContentTop, kContentW, 430, L"Imagem");
  label(h, L"Escala de resolução", x, y + 4, 200);
  add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kScale, x + 210, y, 200, 200);
  size_t sel = 0;
  for (size_t i = 0; i < 4; i++) {
    SendDlgItemMessageW(h, kScale, CB_ADDSTRING, 0, (LPARAM)kScaleNames[i]);
    if (std::fabs(kScales[i] - c.resolution_scale) < std::fabs(kScales[sel] - c.resolution_scale)) sel = i;
  }
  SendDlgItemMessageW(h, kScale, CB_SETCURSEL, sel, 0);
  y += 34;
  label(h, L"", x + 210, y, 560, 22, nullptr, kScalePreview);
  y += 42;
  check(h, L"Ecrã inteiro (Alt+Enter)", kFullscreen, x, y, 500, window::is_fullscreen());
  y += 42;
  check(h, L"Nitidez FSR 1.0 (ampliação EASU + nitidez RCAS)", kFsr, x, y, 500, c.sharpening);
  y += 36;
  label(h, L"", x, y + 4, 200, 22, nullptr, kStrengthText);
  slider(h, kStrength, x + 210, y, 360, 0, 100, (int)std::lround(c.sharpening_strength * 100));
  set_text(h, kStrengthText, L"Força do FSR: %.2f", c.sharpening_strength);
  y += 54;
  check(h, L"VSync (sincronizar com o monitor)", kVsync, x, y, 500, c.vsync);
  y += 42;
  label(h, L"Limite de FPS", x, y + 4, 200);
  add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFpsLimit, x + 210, y, 360, 200);
  int fps_sel = 0;
  for (int i = 0; i < 4; i++) {
    SendDlgItemMessageW(h, kFpsLimit, CB_ADDSTRING, 0, (LPARAM)kFpsNames[i]);
    if (kFpsChoices[i] == c.fps_limit) fps_sel = i;
  }
  SendDlgItemMessageW(h, kFpsLimit, CB_SETCURSEL, fps_sel, 0);
}

void build_audio(HWND h) {
  const config::Config& c = config::get();
  g_build_tab = 1;
  const int x = kContentLeft + 18;
  panel(1, kContentLeft, kContentTop, kContentW, 120, L"Som");
  int y = kContentTop + 52;
  label(h, L"", x, y + 4, 200, 22, nullptr, kVolumeText);
  slider(h, kVolume, x + 210, y, 500, 0, 100, (int)std::lround(c.volume * 100));
  set_text(h, kVolumeText, L"Volume geral: %.0f%%", c.volume * 100);
  const int top = kContentTop + 136;
  panel(1, kContentLeft, top, kContentW, 260, L"Idioma do jogo");
  y = top + 48;
  label(h, L"Falas e legendas", x, y + 4, 200);
  add(h, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kLanguage, x + 210, y, 500, 320);
  int count = 0, sel = 0;
  const language::Info* t = language::table(&count);
  for (int i = 0; i < count; i++) {
    SendDlgItemMessageW(h, kLanguage, CB_ADDSTRING, 0, (LPARAM)t[i].name);
    if (&t[i] == &language::current()) sel = i;
  }
  SendDlgItemMessageW(h, kLanguage, CB_SETCURSEL, sel, 0);
  y += 44;
  label(h, L"", x, y, 780, 66, nullptr, kLanguageNote);
  label(h, L"Este disco tem falas e legendas em inglês, alemão, francês, italiano e espanhol (2 variantes). Coreano e chinês só têm "
           L"texto. Português não existe no disco: ao escolhê-lo, o jogo fica em inglês.", x, y + 76, 780, 66);
}

void build_controls(HWND h) {
  const config::Config& c = config::get();
  g_build_tab = 2;
  const int x = kContentLeft + 18;
  panel(2, kContentLeft, kContentTop, kContentW, 96, L"Rato");
  int y = kContentTop + 44;
  label(h, L"", x, y + 4, 210, 22, nullptr, kMouseText);
  slider(h, kMouse, x + 214, y, 300, 10, 500, (int)std::lround(c.mouse_sensitivity * 100));
  set_text(h, kMouseText, L"Sensibilidade do rato: %.2f", c.mouse_sensitivity);
  check(h, L"Inverter eixo Y", kInvertY, x + 560, y + 2, 220, c.invert_y);
  const int top = kContentTop + 106;
  panel(2, kContentLeft, top, kContentW, 416, L"Teclas (clique e prima a nova tecla ou botão do rato; Esc cancela)");
  const int per_col = (keybinds::kActionCount + 1) / 2, row = 28;
  for (int a = 0; a < keybinds::kActionCount; a++) {
    const int col = a / per_col, r = a % per_col;
    const int cx = x + col * 400, cy = top + 38 + r * row;
    label(h, keybinds::info(a).label, cx, cy + 4, 228, 22);
    button(h, L"", kKeyFirst + a, cx + 230, cy, 150, 25);
  }
  const int by = top + 38 + per_col * row + 6;
  button(h, L"Restaurar Padrão", kRestoreKeys, x, by, 180, 28);
  label(h, L"", x + 196, by + 4, 590, 22, nullptr, kKeysNote);
  update_key_labels(h);
}

void build_multiplayer(HWND h) {
  g_build_tab = 3;
  const int w = (kContentW - 14) / 2, xa = kContentLeft, xb = kContentLeft + w + 14, top = kContentTop, ph = 280;
  panel(3, xa, top, w, ph, L"1. Criar Partida (Anfitrião)");
  panel(3, xb, top, w, ph, L"2. Entrar numa Partida (Convidado)");
  int y = top + 42;
  button(h, L"Gerar Sala Co-op", kCreateRoom, xa + 16, y, w - 32, 34);
  y += 44;
  add(h, L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP, kInviteBox, xa + 16, y, w - 32, 114, g_mono);
  y += 124;
  button(h, L"Copiar Convite (F9)", kCopyInvite, xa + 16, y, 240, 32);
  label(h, L"", xa + 268, y + 6, w - 284, 22, g_bold, kCopyFeedback);

  y = top + 42;
  label(h, L"Cole ou digite o código do amigo (gow://join/...). Se a ligação demorar, o anfitrião cola aqui o código de resposta "
           L"(gow://peer/...) do convidado.", xb + 16, y, w - 32, 62);
  y += 66;
  add(h, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_TABSTOP, kJoinBox, xb + 16, y, w - 32, 96, g_mono);
  y += 106;
  button(h, L"Conectar ao Amigo", kConnect, xb + 16, y, 240, 32);
  button(h, L"Desligar", kDisconnect, xb + w - 136, y, 120, 32);

  const int sy = top + ph + 12;
  panel(3, xa, sy, w, 186, L"Estado em tempo real");
  panel(3, xb, sy, w, 186, L"Como jogar");
  label(h, L"", xa + 16, sy + 40, w - 32, 24, g_bold, kNetState);
  label(h, L"", xa + 16, sy + 70, w - 32, 70, nullptr, kNetDetail);
  label(h, L"Passo 1: Conecte com o seu amigo aqui no F8.\n\nPasso 2: No menu do jogo, escolha Campaign ou Versus → System Link.\n\n"
           L"Passo 3: Crie ou entre na sala que aparecer na lista!", xb + 16, sy + 36, w - 32, 146);
}

void show_tab(HWND h, int tab) {
  g_tab = tab;
  for (int t = 0; t < kTabs; t++)
    for (HWND c : g_controls[t]) ShowWindow(c, t == tab ? SW_SHOW : SW_HIDE);
  for (int t = 0; t < kTabs; t++) InvalidateRect(GetDlgItem(h, kTabFirst + t), nullptr, TRUE);
  InvalidateRect(h, nullptr, TRUE);
}

void build(HWND h) {
  g_build_tab = -1;
  const int tw = kContentW / kTabs;
  for (int t = 0; t < kTabs; t++) button(h, kTabNames[t], kTabFirst + t, kContentLeft + t * tw, 50, tw - 4, 34);
  button(h, L"Fechar (F8)", kClose, kWidth - 158, kHeight - 42, 140, 30);
  for (auto& v : g_controls) v.clear();
  g_panels.clear();
  build_video(h);
  build_audio(h);
  build_controls(h);
  build_multiplayer(h);
  g_build_tab = -1;
  ShowWindow(GetDlgItem(h, kClose), g_embedded ? SW_HIDE : SW_SHOW);
  for (int t = 0; t < kTabs; t++) ShowWindow(GetDlgItem(h, kTabFirst + t), SW_SHOW);
  update_preview(h);
  g_shown_code.clear();
  update_network(h);
  show_tab(h, g_tab);
}

void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT r;
  GetClientRect(h, &r);
  FillRect(dc, &r, g_back);
  RECT bar{0, 0, r.right, 4};
  HBRUSH crimson = CreateSolidBrush(kCrimson);
  FillRect(dc, &bar, crimson);
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ old = SelectObject(dc, g_title);
  SetTextColor(dc, kCrimsonHot);
  RECT title{kContentLeft, 12, 460, 46};
  DrawTextW(dc, g_embedded ? L"CONFIGURAÇÕES" : L"GEARS OF WAR", -1, &title, DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, g_font);
  SetTextColor(dc, kDim);
  RECT sub{kWidth - 420, 12, kWidth - 18, 46};
  DrawTextW(dc, g_embedded ? L"Aplicadas em tempo real, também no F8" : L"Configurações  |  aplicadas em tempo real", -1, &sub, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
  HPEN pen = CreatePen(PS_SOLID, 1, kLine);
  HGDIOBJ old_pen = SelectObject(dc, pen);
  for (const Panel& p : g_panels) {
    if (p.tab != g_tab) continue;
    RECT pr{p.x, p.y, p.x + p.w, p.y + p.h};
    FillRect(dc, &pr, g_panel);
    MoveToEx(dc, p.x, p.y, nullptr);
    LineTo(dc, p.x + p.w, p.y);
    RECT accent{p.x, p.y, p.x + 4, p.y + p.h};
    FillRect(dc, &accent, crimson);
    SelectObject(dc, g_bold);
    SetTextColor(dc, kText);
    RECT t{p.x + 18, p.y + 8, p.x + p.w - 10, p.y + 34};
    DrawTextW(dc, p.title, -1, &t, DT_SINGLELINE | DT_VCENTER);
  }
  SelectObject(dc, old_pen);
  DeleteObject(pen);
  SelectObject(dc, old);
  DeleteObject(crimson);
  EndPaint(h, &ps);
}

void draw_button(const DRAWITEMSTRUCT* d) {
  const int id = (int)d->CtlID;
  const bool tab = id >= kTabFirst && id < kTabFirst + kTabs;
  const bool active_tab = tab && id - kTabFirst == g_tab;
  const bool primary = id == kCreateRoom || id == kConnect || id == kCopyInvite;
  const bool key = id >= kKeyFirst && id < kKeyFirst + (int)keybinds::kActionCount;
  const bool capturing = key && id - kKeyFirst == g_capturing;
  const bool pressed = d->itemState & ODS_SELECTED, disabled = d->itemState & ODS_DISABLED;
  COLORREF fill = kButton, text = kText;
  if (active_tab || capturing) fill = kCrimson;
  else if (tab) fill = kPanel, text = kDim;
  else if (key) fill = kField;
  else if (primary) fill = pressed ? kCrimsonHot : kCrimson;
  if (disabled) fill = RGB(44, 40, 41), text = RGB(112, 106, 105);
  if (pressed && !primary) fill = kCrimsonHot;
  HBRUSH b = CreateSolidBrush(fill);
  FillRect(d->hDC, &d->rcItem, b);
  DeleteObject(b);
  if (key && !capturing) {
    HBRUSH line = CreateSolidBrush(kLine);
    FrameRect(d->hDC, &d->rcItem, line);
    DeleteObject(line);
  }
  wchar_t t[96];
  GetWindowTextW(d->hwndItem, t, 96);
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, text);
  HGDIOBJ old = SelectObject(d->hDC, tab || primary ? g_bold : g_font);
  RECT r = d->rcItem;
  DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(d->hDC, old);
  if (d->itemState & ODS_FOCUS) {
    RECT f = d->rcItem;
    InflateRect(&f, -2, -2);
    DrawFocusRect(d->hDC, &f);
  }
}

void finish_capture(HWND h, uint16_t vk) {
  config::Config& c = config::edit();
  const int action = g_capturing;
  g_capturing = -1;
  ReleaseCapture();
  if (vk == VK_ESCAPE) {
    SetDlgItemTextW(h, kKeysNote, L"");
  } else if (keybinds::reserved(vk)) {
    SetDlgItemTextW(h, kKeysNote, L"Essa tecla está reservada (Esc, F8, F9, F10). Escolha outra.");
  } else {
    std::wstring note;
    for (int a = 0; a < keybinds::kActionCount; a++)
      if (a != action && c.keys[a] == vk) {  // uma tecla só faz uma ação
        c.keys[a] = 0;
        note = L"\"" + std::wstring(keybinds::info(a).label) + L"\" ficou sem tecla.";
      }
    c.keys[action] = vk;
    config::save();
    SetDlgItemTextW(h, kKeysNote, note.c_str());
  }
  update_key_labels(h);
}

std::string dialog_text(HWND h, int id) {
  std::wstring w((size_t)GetWindowTextLengthW(GetDlgItem(h, id)) + 1, L'\0');
  const int n = GetDlgItemTextW(h, id, w.data(), (int)w.size());
  w.resize(n);
  return narrow(w);
}

void command(HWND h, int id, int code) {
  config::Config& c = config::edit();
  if (id >= kTabFirst && id < kTabFirst + kTabs && code == BN_CLICKED) {
    show_tab(h, id - kTabFirst);
  } else if (id >= kKeyFirst && id < kKeyFirst + (int)keybinds::kActionCount && code == BN_CLICKED) {
    g_capturing = id - kKeyFirst;
    SetFocus(h);
    SetCapture(h);
    SetDlgItemTextW(h, kKeysNote, L"");
    update_key_labels(h);
  } else if (id == kRestoreKeys && code == BN_CLICKED) {
    for (int a = 0; a < keybinds::kActionCount; a++) c.keys[a] = keybinds::info(a).default_vk;
    config::save();
    SetDlgItemTextW(h, kKeysNote, L"Teclas padrão restauradas.");
    update_key_labels(h);
  } else if (id == kScale && code == CBN_SELCHANGE) {
    const LRESULT i = SendDlgItemMessageW(h, kScale, CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < 4) c.resolution_scale = kScales[i];
    config::save();
    update_preview(h);
  } else if (id == kFpsLimit && code == CBN_SELCHANGE) {
    const LRESULT i = SendDlgItemMessageW(h, kFpsLimit, CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < 4) c.fps_limit = kFpsChoices[i];
    config::save();
  } else if (id == kLanguage && code == CBN_SELCHANGE) {
    int count = 0;
    const language::Info* t = language::table(&count);
    const LRESULT i = SendDlgItemMessageW(h, kLanguage, CB_GETCURSEL, 0, 0);
    if (i >= 0 && i < count) {
      c.language = t[i].key;
      config::save();
      SetDlgItemTextW(h, kLanguageNote, &t[i] == &language::current() ? L"" : L"Guardado. Reinicie o jogo para aplicar o novo idioma.");
    }
  } else if (id == kFullscreen && code == BN_CLICKED) {
    const bool on = IsDlgButtonChecked(h, kFullscreen) == BST_CHECKED;
    if (on != window::is_fullscreen()) window::toggle_fullscreen_now();
    c.fullscreen = on;
    config::save();
    update_preview(h);
  } else if (id == kFsr && code == BN_CLICKED) {
    c.sharpening = IsDlgButtonChecked(h, kFsr) == BST_CHECKED;
    config::save();
  } else if (id == kVsync && code == BN_CLICKED) {
    c.vsync = IsDlgButtonChecked(h, kVsync) == BST_CHECKED;
    config::save();
  } else if (id == kInvertY && code == BN_CLICKED) {
    c.invert_y = IsDlgButtonChecked(h, kInvertY) == BST_CHECKED;
    config::save();
  } else if (id == kCreateRoom && code == BN_CLICKED) {
    net::create_room();
    update_network(h);
  } else if (id == kCopyInvite && code == BN_CLICKED) {
    net::on_copy_key();
    SetDlgItemTextW(h, kCopyFeedback, L"Copiado!");
    g_copied_until = GetTickCount64() + 2000;
    update_network(h);
  } else if (id == kConnect && code == BN_CLICKED) {
    net::connect_code(dialog_text(h, kJoinBox));
    update_network(h);
  } else if (id == kDisconnect && code == BN_CLICKED) {
    net::disconnect();
    update_network(h);
  } else if (id == kClose && code == BN_CLICKED) {
    toggle(GetWindow(h, GW_OWNER));
  }
}

void centre_over_owner(HWND h) {
  RECT o, r;
  GetWindowRect(GetWindow(h, GW_OWNER), &o);
  GetWindowRect(h, &r);
  const int w = r.right - r.left, ht = r.bottom - r.top;
  SetWindowPos(h, nullptr, std::max<int>(0, (o.left + o.right - w) / 2), std::max<int>(0, (o.top + o.bottom - ht) / 2), 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK menu_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      build(h);
      SetTimer(h, kTimer, 500, nullptr);
      return 0;
    case WM_TIMER:
      update_network(h);
      update_preview(h);
      return 0;
    case WM_DRAWITEM:
      draw_button(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      SetTextColor((HDC)wp, kText);
      SetBkColor((HDC)wp, kField);
      return (LRESULT)g_field;
    case WM_CTLCOLORSTATIC: {
      const int id = GetDlgCtrlID((HWND)lp);
      if (id == kInviteBox) {
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kField);
        return (LRESULT)g_field;
      }
      COLORREF text = kText;
      if (id == kNetState) {
        const auto phase = net::menu_state().phase;
        text = phase == net::MenuState::kConnected ? kGreen : phase == net::MenuState::kWaiting ? kAmber : kDim;
      } else if (id == kCopyFeedback) {
        text = kGreen;
      } else if (id == kLanguageNote || id == kKeysNote) {
        text = kAmber;
      } else if (id == kScalePreview) {
        text = kDim;
      }
      SetTextColor((HDC)wp, text);
      SetBkColor((HDC)wp, kPanel);
      return (LRESULT)g_panel;
    }
    case WM_CTLCOLORBTN:
      SetTextColor((HDC)wp, kText);
      SetBkColor((HDC)wp, kPanel);
      return (LRESULT)g_panel;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint(h);
      return 0;
    case WM_HSCROLL: {
      config::Config& c = config::edit();
      const int id = GetDlgCtrlID((HWND)lp);
      const int pos = (int)SendMessageW((HWND)lp, TBM_GETPOS, 0, 0);
      if (id == kStrength) {
        c.sharpening_strength = pos / 100.0f;
        set_text(h, kStrengthText, L"Força do FSR: %.2f", c.sharpening_strength);
      } else if (id == kMouse) {
        c.mouse_sensitivity = pos / 100.0f;
        set_text(h, kMouseText, L"Sensibilidade do rato: %.2f", c.mouse_sensitivity);
      } else if (id == kVolume) {
        c.volume = pos / 100.0f;
        set_text(h, kVolumeText, L"Volume geral: %.0f%%", c.volume * 100);
      }
      if (LOWORD(wp) == SB_ENDSCROLL) config::save();
      return 0;
    }
    case WM_COMMAND:
      command(h, LOWORD(wp), HIWORD(wp));
      return 0;
    case WM_CLOSE:
      toggle(GetWindow(h, GW_OWNER));
      return 0;
    case WM_DESTROY:
      KillTimer(h, kTimer);
      g_capturing = -1;
      g_menu = nullptr;
      g_embedded = false;
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
  g_back = CreateSolidBrush(kBack);
  g_panel = CreateSolidBrush(kPanel);
  g_field = CreateSolidBrush(kField);
  auto font = [](int size, int weight, const wchar_t* face) {
    return CreateFontW(size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
  };
  g_font = font(-14, FW_NORMAL, L"Segoe UI");
  g_bold = font(-15, FW_SEMIBOLD, L"Segoe UI");
  g_title = font(-24, FW_BOLD, L"Segoe UI");
  g_mono = font(-13, FW_NORMAL, L"Consolas");
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
  RECT r{0, 0, kWidth, kHeight};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOOLWINDOW);
  g_menu = CreateWindowExW(WS_EX_TOOLWINDOW, L"GowMenu", L"Gears of War - Menu (F8)", style, 0, 0, r.right - r.left, r.bottom - r.top,
                           owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!g_menu) return;
  centre_over_owner(g_menu);
  ShowWindow(g_menu, SW_SHOW);
  SetForegroundWindow(g_menu);
  SetFocus(GetDlgItem(g_menu, kTabFirst + g_tab));
}

void* create_embedded(void* parent_ptr, int x, int y) {
  init_once();
  g_embedded = true;
  g_menu = CreateWindowExW(WS_EX_CONTROLPARENT, L"GowMenu", L"", WS_CHILD | WS_VISIBLE, x, y, kWidth, kHeight - 40,
                           static_cast<HWND>(parent_ptr), nullptr, GetModuleHandleW(nullptr), nullptr);
  return g_menu;
}

bool pre_translate(void* msg_ptr) {
  MSG* m = static_cast<MSG*>(msg_ptr);
  if (!g_menu) return false;
  const bool mine = m->hwnd == g_menu || IsChild(g_menu, m->hwnd);
  if (g_capturing >= 0) {
    uint16_t vk = 0;
    switch (m->message) {
      case WM_KEYDOWN:
      case WM_SYSKEYDOWN: vk = (uint16_t)m->wParam; break;
      case WM_LBUTTONDOWN: vk = VK_LBUTTON; break;
      case WM_RBUTTONDOWN: vk = VK_RBUTTON; break;
      case WM_MBUTTONDOWN: vk = VK_MBUTTON; break;
      case WM_XBUTTONDOWN: vk = HIWORD(m->wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
    }
    if (vk) {
      finish_capture(g_menu, vk);
      return true;
    }
    if (m->message == WM_KEYUP || m->message == WM_SYSKEYUP || m->message == WM_LBUTTONUP || m->message == WM_RBUTTONUP) return true;
  }
  if (m->message == WM_KEYDOWN && mine && !g_embedded && (m->wParam == VK_F8 || m->wParam == VK_ESCAPE)) {
    if (!(m->lParam & (1 << 30))) toggle(GetWindow(g_menu, GW_OWNER));
    return true;
  }
  if (m->message == WM_KEYDOWN && mine && m->wParam == VK_F9) {
    if (!(m->lParam & (1 << 30))) command(g_menu, kCopyInvite, BN_CLICKED);
    return true;
  }
  return IsDialogMessageW(g_menu, m) != 0;
}

}  // namespace window::menu
