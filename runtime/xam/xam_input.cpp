// xam_input.cpp — XamInputGetCapabilities/GetState/SetState -> XInput do PC.
//
// Os bits de botões e o formato do XINPUT_GAMEPAD do Xbox 360 são os mesmos do XInput do PC; só a ordem dos
// bytes muda (a memória convidada é BE). O XInput é carregado em runtime (xinput1_4 / 1_3 / 9_1_0).
//
// O utilizador 0 tem sempre um comando ligado: se não houver comando no host, é um comando virtual em repouso.
// GOW_INPUT_SCRIPT="5:start,8:a,9.5:a+dpad_down,20:ls_up*90" carrega botões (ou analógicos ls_/rs_up|down|left|right) a partir de t segundos (desde o 1º GetState)
// durante GOW_INPUT_HOLD_FRAMES frames (6 por omissão) — para testes automáticos sem comando físico.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "gpu/gpu.h"
#include "kernel/printf_test.h"
#include "gpu/window.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"
#include "xam/xam.h"

using namespace kernel;
using namespace xam;

namespace {

struct HostGamepad {
  WORD buttons;
  BYTE left_trigger, right_trigger;
  SHORT lx, ly, rx, ry;
};
struct HostState {
  DWORD packet;
  HostGamepad pad;
};
struct HostVibration {
  WORD left, right;
};
using GetStateFn = DWORD(WINAPI*)(DWORD, HostState*);
using SetStateFn = DWORD(WINAPI*)(DWORD, HostVibration*);

struct XInput {
  GetStateFn get_state = nullptr;
  SetStateFn set_state = nullptr;
  XInput() {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
      HMODULE h = LoadLibraryA(dll);
      if (!h) continue;
      get_state = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(h, "XInputGetState")));
      set_state = reinterpret_cast<SetStateFn>(reinterpret_cast<void*>(GetProcAddress(h, "XInputSetState")));
      std::fprintf(stderr, "[input] %s carregado\n", dll);
      return;
    }
    std::fprintf(stderr, "[input] XInput indisponível: só o comando virtual\n");
  }
};

const XInput& xinput() {
  static XInput x;
  return x;
}

// XInputGetState de um comando desligado é lento (ms) e o jogo consulta em ciclo apertado: guarda o último
// estado por 1 ms (ligado) ou 1 s (desligado).
bool host_state(uint32_t user, HostState& s) {
  struct Cache {
    std::chrono::steady_clock::time_point next{};
    HostState state{};
    bool connected = false;
  };
  static std::mutex mu;
  static Cache cache[kUserCount];
  std::memset(&s, 0, sizeof(s));
  if (!xinput().get_state || user >= kUserCount) return false;
  std::lock_guard<std::mutex> lock(mu);
  Cache& c = cache[user];
  auto now = std::chrono::steady_clock::now();
  if (now >= c.next) {
    c.connected = xinput().get_state(user, &c.state) == ERROR_SUCCESS;
    if (!c.connected) std::memset(&c.state, 0, sizeof(c.state));
    c.next = now + (c.connected ? std::chrono::milliseconds(1) : std::chrono::milliseconds(1000));
  }
  s = c.state;
  return c.connected;
}

// ---- Roteiro de botões ----
struct Press {
  double at;
  uint16_t buttons;
  uint8_t lt, rt;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
  uint32_t frames = 0;  // 0 = GOW_INPUT_HOLD_FRAMES
};

uint16_t button_bit(const std::string& n) {
  static const struct { const char* name; uint16_t bit; } kButtons[] = {
      {"dpad_up", 0x0001}, {"dpad_down", 0x0002}, {"dpad_left", 0x0004}, {"dpad_right", 0x0008},
      {"start", 0x0010},   {"back", 0x0020},      {"ls", 0x0040},        {"rs", 0x0080},
      {"lb", 0x0100},      {"rb", 0x0200},        {"guide", 0x0400},     {"a", 0x1000},
      {"b", 0x2000},       {"x", 0x4000},         {"y", 0x8000}};
  for (auto& b : kButtons)
    if (n == b.name) return b.bit;
  return 0;
}

std::vector<Press> parse_script() {
  std::vector<Press> out;
  const char* e = std::getenv("GOW_INPUT_SCRIPT");
  if (!e) return out;
  std::string s = e;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    std::string item = s.substr(i, j - i);
    size_t colon = item.find(':');
    if (colon != std::string::npos) {
      Press p{std::atof(item.substr(0, colon).c_str()), 0, 0, 0};
      std::string names = item.substr(colon + 1);
      size_t star = names.find('*');  // "botões*frames": duração própria
      if (star != std::string::npos) p.frames = (uint32_t)std::atoi(names.c_str() + star + 1), names.resize(star);
      size_t k = 0;
      while (k <= names.size()) {
        size_t plus = names.find('+', k);
        if (plus == std::string::npos) plus = names.size();
        std::string n = names.substr(k, plus - k);
        if (n == "lt") p.lt = 0xFF;
        else if (n == "ls_up") p.ly = 32767;
        else if (n == "ls_down") p.ly = -32768;
        else if (n == "ls_left") p.lx = -32768;
        else if (n == "ls_right") p.lx = 32767;
        else if (n == "rs_up") p.ry = 32767;
        else if (n == "rs_down") p.ry = -32768;
        else if (n == "rs_left") p.rx = -32768;
        else if (n == "rs_right") p.rx = 32767;
        else if (n == "rt") p.rt = 0xFF;
        else if (uint16_t b = button_bit(n)) p.buttons |= b;
        else std::fprintf(stderr, "[input] GOW_INPUT_SCRIPT: botão desconhecido \"%s\"\n", n.c_str());
        k = plus + 1;
      }
      out.push_back(p);
    }
    i = j + 1;
  }
  std::fprintf(stderr, "[input] roteiro com %zu toques\n", out.size());
  return out;
}

void apply_sticks(HostGamepad& pad, const Press& p) {
  if (p.lx || p.ly) pad.lx = p.lx, pad.ly = p.ly;
  if (p.rx || p.ry) pad.rx = p.rx, pad.ry = p.ry;
}

// PacketNumber monotónico partilhado pelo roteiro e pelo teclado: sobe a cada mudança de estado (como num comando
// real; a UE3 deteta transições comparando estados entre ticks).
std::atomic<uint32_t> g_packet{0};

// Botões do roteiro ativos agora (utilizador 0). Cada toque começa na 1ª consulta a partir de t segundos e fica
// premido durante GOW_INPUT_HOLD_FRAMES frames apresentados (VdSwap; 6 por omissão), depois solto pelo menos 10
// frames antes do seguinte, para cruzar ticks completos do jogo.
void scripted(HostGamepad& pad) {
  static const std::vector<Press> script = parse_script();
  if (script.empty()) return;
  static const auto t0 = std::chrono::steady_clock::now();
  static const uint32_t hold_frames = [] {
    const char* e = std::getenv("GOW_INPUT_HOLD_FRAMES");
    return e && std::atoi(e) > 0 ? (uint32_t)std::atoi(e) : 6u;
  }();
  static std::mutex mu;
  static size_t next = 0;           // próximo toque por começar
  static bool pressed = false;
  static uint32_t start_frame = 0, release_frame = 0;
  std::lock_guard<std::mutex> lock(mu);
  double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  uint32_t frame = gpu::swap_count();
  if (pressed) {
    const Press& p = script[next - 1];
    if (frame - start_frame >= (p.frames ? p.frames : hold_frames)) {
      pressed = false;
      release_frame = frame;
      g_packet++;
    } else {
      pad.buttons |= p.buttons;
      pad.left_trigger |= p.lt;
      pad.right_trigger |= p.rt;
      apply_sticks(pad, p);
    }
    return;
  }
  if (next < script.size() && t >= script[next].at && frame - release_frame >= 10) {
    const Press& p = script[next++];
    pressed = true;
    start_frame = frame;
    g_packet++;
    pad.buttons |= p.buttons;
    pad.left_trigger |= p.lt;
    pad.right_trigger |= p.rt;
    apply_sticks(pad, p);
    std::fprintf(stderr, "[input] roteiro t=%.2fs frame %u: botões 0x%04X (%u frames)\n", t, frame, p.buttons,
                 hold_frames);
  }
}

// Teclado/rato da janela do jogo (gpu/window.h) somados ao comando do utilizador 0. O PacketNumber muda sempre
// que o estado do teclado muda, como num comando real.
void keyboard(HostGamepad& pad) {
  window::PadState k;
  static uint64_t last = 0;
  if (!window::keyboard_pad(k)) {
    if (last) g_packet++, last = 0;
    return;
  }
  uint64_t sig = (uint64_t)k.buttons | (uint64_t)k.left_trigger << 16 | (uint64_t)k.right_trigger << 24 |
                 (uint64_t)(uint16_t)k.lx << 32 | (uint64_t)(uint16_t)k.ly << 48;
  sig ^= ((uint64_t)(uint16_t)k.rx << 17) ^ ((uint64_t)(uint16_t)k.ry << 41);  // o rato também muda o estado
  if (sig != last) {
    g_packet++, last = sig;
    static const bool trace = std::getenv("GOW_TRACE_INPUT") != nullptr;
    if (trace)
      std::fprintf(stderr, "[input] teclado/rato: botões 0x%04X LT %u RT %u L (%d, %d) R (%d, %d)\n", k.buttons, k.left_trigger,
                   k.right_trigger, k.lx, k.ly, k.rx, k.ry);
  }
  pad.buttons |= k.buttons;
  pad.left_trigger |= k.left_trigger;
  pad.right_trigger |= k.right_trigger;
  if (k.lx || k.ly) pad.lx = k.lx, pad.ly = k.ly;
  if (k.rx || k.ry) pad.rx = k.rx, pad.ry = k.ry;
}

bool connected(uint32_t user) {
  HostState s;
  return user == 0 || host_state(user, s);
}

uint32_t user_of(uint32_t user_index) { return user_index == 0xFF ? 0 : user_index; }

}  // namespace

// XamInputGetCapabilities(DWORD UserIndex, DWORD Flags, X_INPUT_CAPABILITIES* Caps)
// X_INPUT_CAPABILITIES (0x14): u8 Type, u8 SubType, u16 Flags, X_INPUT_GAMEPAD (0xC), X_INPUT_VIBRATION (4).
XBOX_IMPORT(XamInputGetCapabilities) {
  uint32_t user = user_of(arg(ctx, 0)), caps = arg(ctx, 2);
  if (!caps) return ret(ctx, kErrorInvalidParameter);
  if (user >= kUserCount || !connected(user)) return ret(ctx, kErrorDeviceNotConnected);
  ppc_st8(mem_base, caps + 0, 1);   // XINPUT_DEVTYPE_GAMEPAD
  ppc_st8(mem_base, caps + 1, 1);   // XINPUT_DEVSUBTYPE_GAMEPAD
  ppc_st16(mem_base, caps + 2, 0);
  ppc_st16(mem_base, caps + 4, 0xFFFF);  // todos os botões / eixos com resolução completa
  ppc_st8(mem_base, caps + 6, 0xFF);
  ppc_st8(mem_base, caps + 7, 0xFF);
  for (uint32_t o = 8; o < 0x10; o += 2) ppc_st16(mem_base, caps + o, 0xFFFF);
  ppc_st16(mem_base, caps + 0x10, 0xFFFF);
  ppc_st16(mem_base, caps + 0x12, 0xFFFF);
  ret(ctx, kErrorSuccess);
}

// XamInputGetState(DWORD UserIndex, DWORD Flags, X_INPUT_STATE* State)
// X_INPUT_STATE (0x10): u32 PacketNumber, X_INPUT_GAMEPAD {u16 Buttons; u8 LT, RT; s16 LX, LY, RX, RY}.
XBOX_IMPORT(XamInputGetState) {
  kernel::run_guest_printf_test(ctx);  // GOW_TEST_PRINTF=1 (uma vez, com a CRT do jogo já iniciada)
  uint32_t user = user_of(arg(ctx, 0)), st = arg(ctx, 2);
  static const bool trace = std::getenv("GOW_TRACE_INPUT") != nullptr;
  if (trace) {
    // Uma linha por milhão de chamadas do mesmo sítio, e todas as chamadas de sítios raros (até 2000 linhas).
    static std::atomic<uint64_t> calls{0};
    static std::atomic<int> lines{0};
    uint64_t n = calls.fetch_add(1);
    bool hot = ctx.lr == 0x8221B42Cu;
    if ((hot ? (n & 0xFFFFF) == 0 : lines.load() < 2000) && lines.fetch_add(1) < 2000)
      std::fprintf(stderr, "[input] GetState #%llu utilizador 0x%X flags 0x%X lr=0x%08X t%u\n",
                   (unsigned long long)n, arg(ctx, 0), arg(ctx, 1), (uint32_t)ctx.lr,
                   kernel::current_thread() ? kernel::current_thread()->thread_id : 0);
  }
  if (!st) return ret(ctx, kErrorInvalidParameter);
  if (user >= kUserCount) return ret(ctx, kErrorDeviceNotConnected);
  HostState s;
  bool host = host_state(user, s);
  if (!host && user != 0) return ret(ctx, kErrorDeviceNotConnected);
  if (user == 0) {
    scripted(s.pad);
    keyboard(s.pad);
  }
  ppc_st32(mem_base, st + 0x0, s.packet + (user == 0 ? g_packet.load() : 0));
  ppc_st16(mem_base, st + 0x4, s.pad.buttons);
  if (trace && s.pad.buttons) {
    static std::atomic<int> pressed_lines{0};
    if (pressed_lines.fetch_add(1) < 300)
      std::fprintf(stderr, "[input] lido: utilizador %u botões 0x%04X pacote %u lr=0x%08X t%u frame %u\n", user,
                   s.pad.buttons, s.packet + (user == 0 ? g_packet.load() : 0), (uint32_t)ctx.lr,
                   kernel::current_thread() ? kernel::current_thread()->thread_id : 0, gpu::swap_count());
  }
  ppc_st8(mem_base, st + 0x6, s.pad.left_trigger);
  ppc_st8(mem_base, st + 0x7, s.pad.right_trigger);
  ppc_st16(mem_base, st + 0x8, (uint16_t)s.pad.lx);
  ppc_st16(mem_base, st + 0xA, (uint16_t)s.pad.ly);
  ppc_st16(mem_base, st + 0xC, (uint16_t)s.pad.rx);
  ppc_st16(mem_base, st + 0xE, (uint16_t)s.pad.ry);
  ret(ctx, kErrorSuccess);
}

// XamInputSetState(DWORD UserIndex, DWORD Flags, X_INPUT_VIBRATION* Vibration)
XBOX_IMPORT(XamInputSetState) {
  uint32_t user = user_of(arg(ctx, 0)), vib = arg(ctx, 2);
  if (user >= kUserCount || !connected(user)) return ret(ctx, kErrorDeviceNotConnected);
  if (vib && xinput().set_state) {
    HostVibration v{ppc_ld16(mem_base, vib), ppc_ld16(mem_base, vib + 2)};
    xinput().set_state(user, &v);
  }
  ret(ctx, kErrorSuccess);
}
