#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>

#include "launcher.h"

#include <cstdio>
#include <string>

#include "game_source.h"
#include "gpu/window.h"
#include "shader_prep.h"

namespace launcher {
namespace {

enum Id { kIsoText = 10, kIsoButton, kPrepText, kPrepButton, kPlay };
constexpr int kWidth = 860, kTop = 214, kHeight = kTop + 620;  // painel do F8 embutido: kHeight - 40 do menu
constexpr COLORREF kBack = RGB(16, 15, 16), kPanel = RGB(32, 29, 30), kCrimson = RGB(165, 18, 28), kCrimsonHot = RGB(205, 32, 42),
                   kText = RGB(236, 232, 230), kDim = RGB(162, 154, 152), kGreen = RGB(92, 200, 110), kAmber = RGB(235, 176, 60),
                   kTrack = RGB(52, 46, 47), kButton = RGB(58, 52, 53);
constexpr UINT_PTR kTimer = 1;

HWND g_wnd = nullptr;
HFONT g_font = nullptr, g_bold = nullptr, g_title = nullptr, g_play = nullptr;
HBRUSH g_back = nullptr, g_panel = nullptr;
bool g_play_pressed = false;
RECT g_bar{24, 150, kWidth - 24, 168};

std::wstring widen(const std::string& s) {
  std::wstring w(s.size(), L'\0');
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
  w.resize(n > 0 ? n : 0);
  return w;
}

HWND add(HWND p, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, HFONT f) {
  HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, p, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)f, TRUE);
  return c;
}

float fraction(const shader_prep::Progress& p) {
  if (p.stage == shader_prep::Progress::kDone) return 1.0f;
  if (p.stage == shader_prep::Progress::kScanning)
    return p.packages_total ? 0.25f * p.packages_done / p.packages_total : 0.0f;
  if (p.stage == shader_prep::Progress::kCompiling)
    return p.to_compile ? 0.25f + 0.75f * p.compiled / p.to_compile : 1.0f;
  return 0.0f;
}

void refresh() {
  const bool found = game_source::locate(false);
  SetDlgItemTextW(g_wnd, kIsoText, game_source::status().c_str());
  EnableWindow(GetDlgItem(g_wnd, kPlay), found);
  const shader_prep::Progress p = shader_prep::progress();
  wchar_t t[256];
  switch (p.stage) {
    case shader_prep::Progress::kScanning:
      swprintf(t, 256, L"A procurar shaders nos pacotes do jogo: %u / %u  (%u encontrados)", p.packages_done, p.packages_total, p.found);
      break;
    case shader_prep::Progress::kCompiling:
      swprintf(t, 256, L"A otimizar shaders para DirectX 12: %u / %u  (%u%%)", p.compiled, p.to_compile,
               p.to_compile ? 100 * p.compiled / p.to_compile : 100);
      break;
    case shader_prep::Progress::kDone:
      swprintf(t, 256, L"Shaders prontos. O jogo abre sem compilar nada.");
      break;
    case shader_prep::Progress::kFailed:
      swprintf(t, 256, L"Não foi possível preparar os shaders: %ls", widen(p.message).c_str());
      break;
    default:
      if (!found) swprintf(t, 256, L"Os shaders são preparados depois de escolher a ISO.");
      else if (shader_prep::up_to_date()) swprintf(t, 256, L"Shaders prontos. O jogo abre sem compilar nada.");
      else swprintf(t, 256, L"Shaders por preparar.");
  }
  SetDlgItemTextW(g_wnd, kPrepText, t);
  const bool busy = shader_prep::running();
  SetDlgItemTextW(g_wnd, kPlay, busy ? L"INICIAR JOGO  (a preparação continua no jogo)" : L"INICIAR JOGO");
  ShowWindow(GetDlgItem(g_wnd, kPrepButton), !busy && found && !shader_prep::up_to_date() ? SW_SHOW : SW_HIDE);
  InvalidateRect(g_wnd, &g_bar, FALSE);
}

void maybe_start_prep() {
  if (game_source::locate(false) && !shader_prep::up_to_date() && !shader_prep::running()) shader_prep::start();
}

void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT r;
  GetClientRect(h, &r);
  r.bottom = kTop;
  FillRect(dc, &r, g_back);
  HBRUSH crimson = CreateSolidBrush(kCrimson), track = CreateSolidBrush(kTrack);
  RECT bar{0, 0, r.right, 4};
  FillRect(dc, &bar, crimson);
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ old = SelectObject(dc, g_title);
  SetTextColor(dc, kCrimsonHot);
  RECT title{24, 14, 520, 52};
  DrawTextW(dc, L"GEARS OF WAR", -1, &title, DT_SINGLELINE | DT_VCENTER);
  SelectObject(dc, g_bold);
  SetTextColor(dc, kDim);
  RECT sub{kWidth - 330, 14, kWidth - 24, 52};
  DrawTextW(dc, L"PC Launcher", -1, &sub, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
  // Barra de progresso dos shaders
  FillRect(dc, &g_bar, track);
  const shader_prep::Progress p = shader_prep::progress();
  float f = fraction(p);
  if (p.stage == shader_prep::Progress::kIdle && shader_prep::up_to_date()) f = 1.0f;
  RECT fill = g_bar;
  fill.right = g_bar.left + (LONG)((g_bar.right - g_bar.left) * f);
  HBRUSH done = CreateSolidBrush(f >= 1.0f ? RGB(60, 140, 75) : kCrimson);
  FillRect(dc, &fill, done);
  DeleteObject(done);
  SelectObject(dc, old);
  DeleteObject(crimson);
  DeleteObject(track);
  EndPaint(h, &ps);
}

void draw_button(const DRAWITEMSTRUCT* d) {
  const bool play = d->CtlID == kPlay, disabled = d->itemState & ODS_DISABLED, pressed = d->itemState & ODS_SELECTED;
  COLORREF fill = play ? (pressed ? kCrimsonHot : kCrimson) : (pressed ? kCrimsonHot : kButton);
  if (disabled) fill = RGB(44, 40, 41);
  HBRUSH b = CreateSolidBrush(fill);
  FillRect(d->hDC, &d->rcItem, b);
  DeleteObject(b);
  wchar_t t[96];
  GetWindowTextW(d->hwndItem, t, 96);
  SetBkMode(d->hDC, TRANSPARENT);
  SetTextColor(d->hDC, disabled ? RGB(112, 106, 105) : kText);
  HGDIOBJ old = SelectObject(d->hDC, play ? g_play : g_bold);
  RECT r = d->rcItem;
  DrawTextW(d->hDC, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(d->hDC, old);
  if (d->itemState & ODS_FOCUS) {
    RECT f = d->rcItem;
    InflateRect(&f, -3, -3);
    DrawFocusRect(d->hDC, &f);
  }
}

LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      g_wnd = h;
      add(h, L"STATIC", L"", SS_NOPREFIX | SS_ENDELLIPSIS, kIsoText, 24, 66, kWidth - 250, 24, g_bold);
      add(h, L"BUTTON", L"Selecionar ISO", BS_OWNERDRAW | WS_TABSTOP, kIsoButton, kWidth - 200, 62, 176, 30, g_bold);
      add(h, L"STATIC", L"", SS_NOPREFIX, kPrepText, 24, 118, kWidth - 250, 24, g_font);
      add(h, L"BUTTON", L"Preparar shaders", BS_OWNERDRAW | WS_TABSTOP, kPrepButton, kWidth - 200, 112, 176, 30, g_bold);
      add(h, L"BUTTON", L"INICIAR JOGO", BS_OWNERDRAW | WS_TABSTOP, kPlay, 24, 176, kWidth - 48, 30, g_play);
      window::menu::create_embedded(h, 0, kTop);
      SetTimer(h, kTimer, 200, nullptr);
      maybe_start_prep();
      refresh();
      return 0;
    }
    case WM_TIMER:
      refresh();
      return 0;
    case WM_DRAWITEM:
      draw_button(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
      return TRUE;
    case WM_CTLCOLORSTATIC: {
      const int id = GetDlgCtrlID((HWND)lp);
      const shader_prep::Progress p = shader_prep::progress();
      COLORREF c = kText;
      if (id == kIsoText) c = game_source::locate(false) ? kGreen : kAmber;
      if (id == kPrepText) c = p.stage == shader_prep::Progress::kFailed ? kAmber : kDim;
      SetTextColor((HDC)wp, c);
      SetBkColor((HDC)wp, kBack);
      return (LRESULT)g_back;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      paint(h);
      return 0;
    case WM_COMMAND:
      if (HIWORD(wp) == BN_CLICKED) {
        if (LOWORD(wp) == kIsoButton) {
          if (game_source::choose_iso()) maybe_start_prep();
          refresh();
        } else if (LOWORD(wp) == kPrepButton) {
          shader_prep::start();
          refresh();
        } else if (LOWORD(wp) == kPlay && game_source::locate(false)) {
          g_play_pressed = true;
          DestroyWindow(h);
        }
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      KillTimer(h, kTimer);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool run() {
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES};
  InitCommonControlsEx(&icc);
  SetProcessDPIAware();
  g_back = CreateSolidBrush(kBack);
  g_panel = CreateSolidBrush(kPanel);
  auto font = [](int size, int weight) {
    return CreateFontW(size, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
  };
  g_font = font(-14, FW_NORMAL);
  g_bold = font(-15, FW_SEMIBOLD);
  g_title = font(-30, FW_BOLD);
  g_play = font(-18, FW_BOLD);
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"GowLauncher";
  RegisterClassExW(&wc);
  const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
  RECT r{0, 0, kWidth, kHeight};
  AdjustWindowRectEx(&r, style, FALSE, WS_EX_CONTROLPARENT);
  const int w = r.right - r.left, ht = r.bottom - r.top;
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, wc.lpszClassName, L"Gears of War - PC Launcher", style,
                           work.left + (work.right - work.left - w) / 2, work.top + std::max<int>(0, (work.bottom - work.top - ht) / 2), w,
                           ht, nullptr, nullptr, wc.hInstance, nullptr);
  if (!h) return true;  // sem launcher, joga-se na mesma
  ShowWindow(h, SW_SHOW);
  SetFocus(GetDlgItem(h, kPlay));
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (window::menu::pre_translate(&msg)) continue;
    if (g_wnd && IsDialogMessageW(g_wnd, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  g_wnd = nullptr;
  if (!g_play_pressed) shader_prep::request_stop();
  return g_play_pressed;
}

}  // namespace launcher
