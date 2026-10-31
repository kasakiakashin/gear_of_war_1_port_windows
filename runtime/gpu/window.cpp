#include "gpu/window.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>

#include "net/tunnel.h"
#include <mutex>
#include <string>
#include <thread>

#include "config.h"

namespace window {
namespace {

constexpr UINT kMsgToggleFullscreen = WM_APP + 1;

std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_closed{false}, g_focus{false};
std::atomic<uint32_t> g_client_w{0}, g_client_h{0};
std::mutex g_mu;
bool g_keys[256] = {};
bool g_captured = false;
float g_mouse_x = 0, g_mouse_y = 0;  // movimento pendente (píxeis): decai com o tempo na leitura
bool g_fullscreen = false;
RECT g_restore{};
DWORD g_restore_style = 0;

void update_title_hint(HWND h) {
  // O título principal é atualizado pelo renderer; aqui só a dica de captura enquanto o rato está solto.
  (void)h;
}

// Os botões do rato ficam em g_keys, nos seus VK, para poderem ser remapeados como qualquer tecla.
void clear_mouse_buttons() {
  for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2}) g_keys[vk] = false;
}

void set_capture(HWND h, bool on) {
  // Chamada com g_mu tomado.
  if (g_captured == on) return;
  g_captured = on;
  g_mouse_x = g_mouse_y = 0;
  if (on) {
    RECT r;
    GetClientRect(h, &r);
    POINT a{r.left, r.top}, b{r.right, r.bottom};
    ClientToScreen(h, &a);
    ClientToScreen(h, &b);
    RECT clip{a.x, a.y, b.x, b.y};
    ClipCursor(&clip);
    while (ShowCursor(FALSE) >= 0) {
    }
  } else {
    ClipCursor(nullptr);
    while (ShowCursor(TRUE) < 0) {
    }
    clear_mouse_buttons();
  }
  update_title_hint(h);
}

void toggle_fullscreen(HWND h) {
  g_fullscreen = !g_fullscreen;
  if (g_fullscreen) {
    GetWindowRect(h, &g_restore);
    g_restore_style = (DWORD)GetWindowLongPtrW(h, GWL_STYLE);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  } else {
    SetWindowLongPtrW(h, GWL_STYLE, g_restore_style);
    SetWindowPos(h, HWND_NOTOPMOST, g_restore.left, g_restore.top, g_restore.right - g_restore.left,
                 g_restore.bottom - g_restore.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_captured) {  // volta a prender o cursor à área nova
    g_captured = false;
    while (ShowCursor(TRUE) < 0) {
    }
    set_capture(h, true);
  }
}

void set_mouse_buttons(WPARAM wp) {
  const WORD s = GET_KEYSTATE_WPARAM(wp);
  g_keys[VK_LBUTTON] = s & MK_LBUTTON;
  g_keys[VK_RBUTTON] = s & MK_RBUTTON;
  g_keys[VK_MBUTTON] = s & MK_MBUTTON;
  g_keys[VK_XBUTTON1] = s & MK_XBUTTON1;
  g_keys[VK_XBUTTON2] = s & MK_XBUTTON2;
}

LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      g_closed = true;
      std::fprintf(stderr, "[window] janela fechada: a terminar\n");
      std::fflush(stderr);
      std::_Exit(0);
    case WM_SIZE:
      if (wp != SIZE_MINIMIZED) g_client_w = LOWORD(lp), g_client_h = HIWORD(lp);
      return 0;
    case WM_SETFOCUS:
      g_focus = true;
      return 0;
    case WM_KILLFOCUS: {
      g_focus = false;
      std::lock_guard<std::mutex> lock(g_mu);
      for (bool& k : g_keys) k = false;
      set_capture(h, false);
      return 0;
    }
    case kMsgToggleFullscreen:
      toggle_fullscreen(h);
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == VK_RETURN && (lp & (1 << 29))) {  // Alt+Enter
        if (!(lp & (1 << 30))) toggle_fullscreen(h);
        return 0;
      }
      if (wp == VK_F4) break;  // Alt+F4 fecha a janela
      [[fallthrough]];
    case WM_KEYDOWN: {
      if (wp == VK_F8 || wp == VK_F9) {  // menu / copiar o código do túnel
        if (!(lp & (1 << 30))) wp == VK_F8 ? menu::toggle(h) : net::on_copy_key();
        return 0;
      }
      std::lock_guard<std::mutex> lock(g_mu);
      if ((wp == VK_ESCAPE || wp == VK_F10) && g_captured) {
        set_capture(h, false);  // solta o rato; não chega ao jogo
        return 0;
      }
      g_keys[wp & 0xFF] = true;
      return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      std::lock_guard<std::mutex> lock(g_mu);
      g_keys[wp & 0xFF] = false;
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN: {
      std::lock_guard<std::mutex> lock(g_mu);
      if (menu::is_open()) return 0;
      if (!g_captured) {  // o clique que captura não dispara
        set_capture(h, true);
        return 0;
      }
      set_mouse_buttons(wp);
      return msg == WM_XBUTTONDOWN ? TRUE : 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP: {
      std::lock_guard<std::mutex> lock(g_mu);
      set_mouse_buttons(wp);
      return msg == WM_XBUTTONUP ? TRUE : 0;
    }
    case WM_INPUT: {
      RAWINPUT ri;
      UINT size = sizeof(ri);
      if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
          ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_captured) g_mouse_x += (float)ri.data.mouse.lLastX, g_mouse_y += (float)ri.data.mouse.lLastY;
      }
      break;
    }
  }
  return DefWindowProcW(h, msg, wp, lp);
}

void window_thread(int width, int height, std::mutex* mu, std::condition_variable* cv, bool* ready) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  wc.lpszClassName = L"GowWindow";
  RegisterClassExW(&wc);
  SetProcessDPIAware();
  const config::Config& cfg = config::get();
  int w = (int)std::lround(width * cfg.resolution_scale), h_px = (int)std::lround(height * cfg.resolution_scale);
  if (const char* e = std::getenv("GOW_WINDOW_SIZE")) std::sscanf(e, "%dx%d", &w, &h_px);  // saída ≠ resolução interna
  // Em janela, limitado à área de trabalho (mantendo 16:9).
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int max_w = work.right - work.left - 16, max_h = work.bottom - work.top - 48;
  if (max_w > 0 && max_h > 0 && (w > max_w || h_px > max_h)) {
    const float k = std::min((float)max_w / w, (float)max_h / h_px);
    w = (int)(w * k), h_px = (int)(h_px * k);
    std::fprintf(stderr, "[window] ResolutionScale maior que o ecrã: janela limitada a %d×%d\n", w, h_px);
  }
  RECT r{0, 0, w, h_px};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hw = CreateWindowExW(0, wc.lpszClassName, L"Gears of War (recompilado)", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
  if (hw) {
    RAWINPUTDEVICE rid{0x01, 0x02, 0, hw};  // rato (generic desktop / mouse)
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    // Testes automáticos não roubam o foco; para jogar, a janela abre ativa.
    ShowWindow(hw, std::getenv("GOW_INPUT_SCRIPT") ? SW_SHOWNOACTIVATE : SW_SHOW);
    RECT cr;
    GetClientRect(hw, &cr);
    g_client_w = (uint32_t)cr.right, g_client_h = (uint32_t)cr.bottom;
    g_hwnd = hw;
    if (cfg.fullscreen) {
      toggle_fullscreen(hw);
      GetClientRect(hw, &cr);
      g_client_w = (uint32_t)cr.right, g_client_h = (uint32_t)cr.bottom;
    }
  }
  {
    std::lock_guard<std::mutex> lock(*mu);
    *ready = true;
  }
  cv->notify_all();
  if (!hw) return;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (menu::pre_translate(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

int16_t axis(bool neg, bool pos) { return pos == neg ? 0 : pos ? 32767 : -32768; }

// Rato → analógico: velocidade do rato (píxeis pendentes com decaimento) com compensação da zona morta da UE3, para que
// movimentos lentos também mexam a mira.
int16_t mouse_axis(float pending) {
  constexpr float kGain = 900.0f, kDeadzone = 7000.0f;
  float v = pending * kGain * config::get().mouse_sensitivity;
  if (std::fabs(v) < 1.0f) return 0;
  v += v > 0 ? kDeadzone : -kDeadzone;
  return (int16_t)std::clamp(v, -32768.0f, 32767.0f);
}

}  // namespace

void* create(int width, int height) {
  if (const char* e = std::getenv("GOW_NO_WINDOW"); e && *e == '1') return nullptr;
  if (g_hwnd) return g_hwnd.load();
  std::mutex mu;
  std::condition_variable cv;
  bool ready = false;
  std::thread(window_thread, width, height, &mu, &cv, &ready).detach();
  std::unique_lock<std::mutex> lock(mu);
  cv.wait(lock, [&] { return ready; });
  if (!g_hwnd) std::fprintf(stderr, "[window] CreateWindowEx falhou (%lu)\n", GetLastError());
  return g_hwnd.load();
}

void* handle() { return g_hwnd.load(); }
bool closed() { return g_closed.load(); }

void client_size(uint32_t& w, uint32_t& h) {
  w = g_client_w.load();
  h = g_client_h.load();
}

void set_title(const char* title) {
  HWND h = g_hwnd.load();
  if (!h) return;
  wchar_t wide[256];
  if (MultiByteToWideChar(CP_UTF8, 0, title, -1, wide, 256) > 0) SetWindowTextW(h, wide);
}

bool is_fullscreen() { return g_fullscreen; }
void toggle_fullscreen_now() {
  if (HWND h = g_hwnd.load()) toggle_fullscreen(h);
}
bool mouse_captured() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_captured;
}
void capture_mouse(bool on) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (HWND h = g_hwnd.load()) set_capture(h, on);
}

bool keyboard_pad(PadState& out) {
  out = PadState{};
  if (!g_focus) return false;
  std::lock_guard<std::mutex> lock(g_mu);
  const config::Config& cfg = config::get();
  auto k = [](int vk) { return vk && g_keys[vk & 0xFF]; };
  auto act = [&](int a) { return k(cfg.keys[a]); };
  for (int a = 0; a < keybinds::kActionCount; a++)
    if (const uint16_t bit = keybinds::info(a).button; bit && act(a)) out.buttons |= bit;
  // Alternativas fixas (setas no direcional, F no corpo a corpo), se a tecla não tiver sido usada noutra ação.
  auto free_key = [&](int vk) {
    for (uint16_t b : cfg.keys)
      if (b == vk) return false;
    return true;
  };
  static const struct { int vk; uint16_t bit; } kAliases[] = {
      {VK_UP, 0x0001}, {VK_DOWN, 0x0002}, {VK_LEFT, 0x0004}, {VK_RIGHT, 0x0008}, {'F', 0x2000}};
  for (auto& m : kAliases)
    if (k(m.vk) && free_key(m.vk)) out.buttons |= m.bit;
  if (k(VK_ESCAPE)) out.buttons |= 0x2000;  // só chega aqui com o rato solto: voltar nos menus
  out.lx = axis(act(keybinds::kMoveLeft), act(keybinds::kMoveRight));
  out.ly = axis(act(keybinds::kMoveBack), act(keybinds::kMoveForward));
  out.rx = axis(act(keybinds::kLookLeft), act(keybinds::kLookRight));
  out.ry = axis(act(keybinds::kLookDown), act(keybinds::kLookUp));
  // Rato: o movimento pendente decai com uma constante de 40 ms; em regime, 1000 píxeis/s dão o analógico a fundo.
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  const float dt = std::chrono::duration<float>(now - last).count();
  last = now;
  if (!out.rx && !out.ry) {
    out.rx = mouse_axis(g_mouse_x);
    out.ry = mouse_axis(cfg.invert_y ? g_mouse_y : -g_mouse_y);
  }
  const float decay = std::exp(-dt / 0.04f);
  g_mouse_x *= decay;
  g_mouse_y *= decay;
  if (std::fabs(g_mouse_x) < 0.01f) g_mouse_x = 0;
  if (std::fabs(g_mouse_y) < 0.01f) g_mouse_y = 0;
  out.right_trigger = act(keybinds::kFire) ? 255 : 0;
  out.left_trigger = act(keybinds::kAim) ? 255 : 0;
  return out.buttons || out.lx || out.ly || out.rx || out.ry || out.left_trigger || out.right_trigger;
}

}  // namespace window
