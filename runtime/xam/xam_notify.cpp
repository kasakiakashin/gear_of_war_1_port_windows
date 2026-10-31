// xam_notify.cpp — XOVERLAPPED, listeners de notificação (XamNotifyCreateListener/XNotifyGetNext) e XMsg*.
//
// Um listener é um evento manual do convidado (o jogo espera-o com WaitForSingleObject) + uma fila no host;
// o evento fica sinalizado enquanto a fila não estiver vazia. Como no Xenia, o 1º listener de sistema recebe
// as notificações de arranque XN_SYS_UI (1, 0) e XN_SYS_SIGNINCHANGED (×2).
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include "kernel/apc.h"
#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "xam/xam.h"

using namespace kernel;

namespace xam {
namespace {

struct Listener {
  uint64_t mask;
  uint32_t event;  // objeto (evento manual) na memória convidada
  std::deque<std::pair<uint32_t, uint32_t>> queue;
};

std::mutex g_mu;
std::map<uint32_t, Listener> g_listeners;  // handle -> listener
bool g_startup_notified = false;
const bool g_trace = [] {
  const char* e = std::getenv("GOW_TRACE_XAM");
  return e && *e && *e != '0';
}();

void enqueue(Listener& l, uint32_t id, uint32_t param) {
  if (!(l.mask & (1ull << ((id >> 25) & 0x3F)))) return;
  l.queue.emplace_back(id, param);
  set_event(l.event);
}

}  // namespace

void complete_overlapped(uint8_t* m, uint32_t ov, uint32_t result, uint32_t length) {
  if (!ov) return;
  ppc_st32(m, ov + 0x00, result);
  ppc_st32(m, ov + 0x04, length);
  ppc_st32(m, ov + 0x18, result ? 0x80070000u | (result & 0xFFFF) : 0);  // HRESULT_FROM_WIN32
  // pCompletionRoutine(dwErrorCode, dwNumberOfBytesTransferred, pOverlapped): APC para a thread que pediu.
  if (uint32_t routine = ppc_ld32(m, ov + 0x10)) queue_user_apc(nullptr, routine, result, length, ov);
  if (uint32_t ev = ppc_ld32(m, ov + 0x0C))
    if (uint32_t obj = object_of(ev)) set_event(obj);
}

uint32_t finish(uint8_t* m, uint32_t ov, uint32_t result, uint32_t length) {
  if (!ov) return result;
  complete_overlapped(m, ov, result, length);
  return kErrorIoPending;
}

void broadcast(uint32_t id, uint32_t param) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_trace) std::fprintf(stderr, "[xam] notificação 0x%08X(0x%X)\n", id, param);
  for (auto& [h, l] : g_listeners) enqueue(l, id, param);
}

void system_ui_shown() {
  broadcast(kXnSysUi, 1);
  broadcast(kXnSysUi, 0);
}

}  // namespace xam

using namespace xam;

// XamNotifyCreateListener(QWORD Mask, DWORD MaxVersion) -> HANDLE (0 em falha). A máscara usa os 64 bits de r3.
XBOX_IMPORT(XamNotifyCreateListener) {
  uint64_t mask = ctx.r[3];
  uint32_t obj = 0, handle = create_event_handle(true, false, &obj);
  std::lock_guard<std::mutex> lock(g_mu);
  Listener& l = g_listeners[handle] = Listener{mask, obj, {}};
  if (!g_startup_notified && (mask & 1)) {
    g_startup_notified = true;
    enqueue(l, kXnSysUi, 1);
    enqueue(l, kXnSysUi, 0);
    enqueue(l, kXnSysSigninChanged, 1);
    enqueue(l, kXnSysSigninChanged, 1);
  }
  std::fprintf(stderr, "[xam] listener 0x%08X (máscara 0x%llX)\n", handle, (unsigned long long)mask);
  ret(ctx, handle);
}

// XNotifyGetNext(HANDLE, DWORD MatchId, PDWORD Id, PULONG_PTR Param) -> BOOL
XBOX_IMPORT(XNotifyGetNext) {
  uint32_t handle = arg(ctx, 0), match = arg(ctx, 1), id_ptr = arg(ctx, 2), param_ptr = arg(ctx, 3);
  out32(mem_base, id_ptr, 0);
  out32(mem_base, param_ptr, 0);
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_listeners.find(handle);
  if (it == g_listeners.end()) return ret(ctx, 0);
  Listener& l = it->second;
  auto q = l.queue.begin();
  if (match)
    while (q != l.queue.end() && q->first != match) ++q;
  if (q == l.queue.end()) return ret(ctx, 0);
  out32(mem_base, id_ptr, q->first);
  out32(mem_base, param_ptr, q->second);
  if (g_trace) std::fprintf(stderr, "[xam] XNotifyGetNext -> 0x%08X(0x%X)\n", q->first, q->second);
  l.queue.erase(q);
  if (l.queue.empty()) reset_event(l.event);
  ret(ctx, 1);
}

XBOX_IMPORT(XNotifyPositionUI) {}

// ---- XMsg: mensagens para "apps" do sistema. ----
namespace {

// Apps do XAM (como no Xenia): 0xFA XMP (leitor de música), 0xFB XGI (contextos, propriedades, sessões),
// 0xFC XLiveBase (logon, amigos).
constexpr uint32_t kAppXmp = 0xFA, kAppXgi = 0xFB, kAppXLiveBase = 0xFC;
constexpr uint32_t kHresultFail = 0x80004005u;

uint32_t dispatch_message(uint8_t* m, uint32_t app, uint32_t msg, uint32_t buffer, uint32_t length) {
  static std::mutex mu;
  static std::set<uint64_t> seen;
  {
    std::lock_guard<std::mutex> lock(mu);
    if (seen.insert((uint64_t)app << 32 | msg).second)
      std::fprintf(stderr, "[xam] XMsg app 0x%X mensagem 0x%08X buffer 0x%08X (%u bytes)\n", app, msg, buffer,
                   length);
  }
  switch (app) {
    case kAppXgi:  // contextos/propriedades do utilizador e sessões: aceites e ignorados (sem Live)
      return kErrorSuccess;
    case kAppXLiveBase:
      if (std::getenv("GOW_TRACE_XAM")) {
        for (uint32_t p : {buffer, length})
          if (p >= 0x10000 && p < 0x80000000u)
            std::fprintf(stderr, "[xam]   XLiveBase 0x%08X [0x%08X]: %08X %08X %08X %08X %08X %08X\n", msg, p,
                         ppc_ld32(m, p), ppc_ld32(m, p + 4), ppc_ld32(m, p + 8), ppc_ld32(m, p + 12),
                         ppc_ld32(m, p + 16), ppc_ld32(m, p + 20));
      }
      if (msg == 0x00058004) {  // XLiveBaseGetLogonId (chamada no arranque): escreve um BOOL
        out32(m, buffer, 1);
        return kErrorSuccess;
      }
      if (msg == 0x00058020 && length >= 0x10000 && length < 0x80000000u) {
        // Enumerador de amigos: argumentos em param2 = {?, início, quantidade, PHANDLE, ...}. Offline: vazio.
        uint32_t handle_ptr = ppc_ld32(m, length + 12);
        if (handle_ptr) {
          out32(m, handle_ptr, create_empty_enumerator(0x10, 1));
          return kErrorSuccess;
        }
      }
      return kHresultFail;  // restantes mensagens de amigos/presença: indisponíveis offline
    case kAppXmp:
      break;
    default:
      return kErrorNotFound;
  }
  if (!buffer) return kErrorSuccess;
  switch (msg) {
    case 0x00070009:  // XMPGetStatus {client, PDWORD state}: parado
      out32(m, ppc_ld32(m, buffer + 4), 0);
      break;
    case 0x0007000B:  // XMPGetVolume {client, float* volume}
      if (uint32_t p = ppc_ld32(m, buffer + 4)) ppc_stf32(m, p, 1.0f);
      break;
    case 0x0007001B:  // XMPGetPlaybackController {client, PDWORD controller, PDWORD locked}: o título controla
      out32(m, ppc_ld32(m, buffer + 4), 0);
      out32(m, ppc_ld32(m, buffer + 8), 0);
      break;
    default:  // play/stop/pausa/set*/playlists: sem música do utilizador, nada a fazer
      break;
  }
  return kErrorSuccess;
}

}  // namespace

// XMsgInProcessCall(App, Message, Param1, Param2)
XBOX_IMPORT(XMsgInProcessCall) {
  ret(ctx, dispatch_message(mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3)));
}
// XMsgStartIORequest(App, Message, PXOVERLAPPED, Buffer, BufferLength)
XBOX_IMPORT(XMsgStartIORequest) {
  uint32_t r = dispatch_message(mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 3), arg(ctx, 4));
  ret(ctx, finish(mem_base, arg(ctx, 2), r));
}
// XMsgStartIORequestEx(App, Message, PXOVERLAPPED, Buffer, BufferLength, Unknown)
XBOX_IMPORT(XMsgStartIORequestEx) {
  uint32_t r = dispatch_message(mem_base, arg(ctx, 0), arg(ctx, 1), arg(ctx, 3), arg(ctx, 4));
  ret(ctx, finish(mem_base, arg(ctx, 2), r));
}
// XMsgCancelIORequest(PXOVERLAPPED, BOOL Wait): tudo já completou.
XBOX_IMPORT(XMsgCancelIORequest) { ret(ctx, kErrorSuccess); }
