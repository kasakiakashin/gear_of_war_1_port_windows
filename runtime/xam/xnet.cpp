// xnet.cpp — rede (NetDll_* do xam.xex): sockets UDP virtuais sobre a "LAN" 10.77.0.0/24 de net/tunnel.cpp.
//
// O 1º argumento de todas as NetDll_* é o "caller" (1 = título); os argumentos reais começam em r4. Nenhum socket do
// convidado é um socket do host: portas e IPs vivem na memória convidada em big-endian (= ordem de rede), logo os valores
// lidos com ppc_ld16/ppc_ld32 já estão na ordem natural. Sem túnel ativo o cabo aparece ligado e não há mais ninguém na
// rede; TCP não é suportado (o UE3 usa só UDP/VDP).
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "kernel/kernel.h"
#include "kernel/sync.h"
#include "net/tunnel.h"
#include "xam/xam.h"

using namespace kernel;
using namespace xam;

namespace {

constexpr uint32_t kWsaEBadf = 10009, kWsaEInval = 10022, kWsaEWouldBlock = 10035, kWsaENotSock = 10038,
                   kWsaEMsgSize = 10040, kWsaEAddrInUse = 10048, kWsaENetDown = 10050, kWsaEShutdown = 10058,
                   kWsaETimedOut = 10060, kWsaEConnRefused = 10061, kWsaENotConn = 10057, kWsaEAfNoSupport = 10047;
constexpr uint32_t kSocketError = 0xFFFFFFFFu, kInvalidSocket = 0xFFFFFFFFu;
constexpr uint32_t kXnAddrNone = 1, kXnAddrEthernet = 2, kXnAddrStatic = 4;  // XNET_GET_XNADDR_*
constexpr uint32_t kFionbio = 0x8004667E, kFionread = 0x4004667F;
constexpr uint32_t kSolSocket = 0xFFFF, kSoReuseAddr = 4, kSoSndBuf = 0x1001, kSoRcvBuf = 0x1002, kSoRcvTimeo = 0x1006,
                   kSoType = 0x1008;
constexpr uint32_t kSockStream = 1;
constexpr uint32_t kLoopback = 0x7F000001;
constexpr size_t kMaxQueue = 512;
constexpr uint32_t kMaxDatagram = 8192;

thread_local uint32_t t_last_error = 0;

uint32_t fail(uint32_t err) {
  t_last_error = err;
  return kSocketError;
}

struct Datagram {
  uint32_t src;
  uint16_t port;
  std::vector<uint8_t> data;
};

struct Socket {
  uint32_t type = 0;
  uint16_t port = 0;  // 0 = ainda sem porta
  bool nonblocking = false, reuse = false, closed = false;
  uint32_t recv_timeout_ms = 0, snd_buf = 65536, rcv_buf = 65536;
  uint32_t peer_ip = 0;  // connect()
  uint16_t peer_port = 0;
  std::deque<Datagram> queue;
  std::condition_variable cv;
};

std::mutex g_mu;
std::map<uint32_t, std::shared_ptr<Socket>> g_sockets;
uint32_t g_next_handle = 0x4000;
uint16_t g_next_port = 49152;

std::shared_ptr<Socket> find_socket(uint32_t h) {
  auto it = g_sockets.find(h);
  return it == g_sockets.end() ? nullptr : it->second;
}

bool port_in_use(uint16_t port) {
  for (auto& [h, s] : g_sockets)
    if (s->port == port) return true;
  return false;
}

// Porta efémera na 1ª utilização sem bind (com g_mu tomado).
void ensure_port(Socket& s) {
  if (s.port) return;
  uint16_t port;
  do {
    port = g_next_port;
    g_next_port = g_next_port == 65535 ? 49152 : g_next_port + 1;
  } while (port_in_use(port));
  s.port = port;
}

void queue_datagram(uint32_t src, uint16_t src_port, uint16_t dst_port, bool broadcast, const uint8_t* data, uint32_t len) {
  std::lock_guard<std::mutex> lock(g_mu);
  for (auto& [h, s] : g_sockets) {
    if (s->port != dst_port || s->type == kSockStream || s->queue.size() >= kMaxQueue) continue;
    s->queue.push_back({src, src_port, std::vector<uint8_t>(data, data + len)});
    s->cv.notify_all();
    if (!broadcast) break;
  }
}

// Envio comum a sendto/send: a própria máquina entrega direto, o resto vai pelo túnel.
void route(uint16_t src_port, uint32_t dst, uint16_t dst_port, const uint8_t* data, uint32_t len) {
  const uint32_t me = net::local_vip();
  if (dst == me || dst == kLoopback) return queue_datagram(me ? me : kLoopback, src_port, dst_port, false, data, len);
  const bool broadcast = dst == 0xFFFFFFFFu || (dst & 0xFF) == 0xFF;
  net::send(broadcast ? net::kBroadcastVip : dst, src_port, dst_port, data, len);
}

uint32_t do_send(Socket& s, uint32_t dst, uint16_t dst_port, uint8_t* m, uint32_t buf, uint32_t len) {
  if (len > kMaxDatagram) return fail(kWsaEMsgSize);
  route(s.port, dst, dst_port, reinterpret_cast<const uint8_t*>(PPC_HOST(m, buf)), len);
  return len;
}

uint32_t do_recv(const std::shared_ptr<Socket>& s, uint8_t* m, uint32_t buf, uint32_t len, uint32_t from, uint32_t fromlen) {
  std::unique_lock<std::mutex> lock(g_mu);
  ensure_port(*s);
  if (s->queue.empty()) {
    if (s->nonblocking) return fail(kWsaEWouldBlock);
    auto ready = [&] { return s->closed || !s->queue.empty(); };
    if (s->recv_timeout_ms) {
      if (!s->cv.wait_for(lock, std::chrono::milliseconds(s->recv_timeout_ms), ready)) return fail(kWsaETimedOut);
    } else {
      s->cv.wait(lock, ready);
    }
    if (s->closed) return fail(kWsaEShutdown);
  }
  Datagram d = std::move(s->queue.front());
  s->queue.pop_front();
  lock.unlock();
  const uint32_t n = std::min<uint32_t>(len, (uint32_t)d.data.size());
  if (n) std::memcpy(PPC_HOST(m, buf), d.data.data(), n);
  if (from) {
    ppc_st16(m, from + 0, 2);  // AF_INET
    ppc_st16(m, from + 2, d.port);
    ppc_st32(m, from + 4, d.src);
    ppc_st32(m, from + 8, 0);
    ppc_st32(m, from + 12, 0);
  }
  if (fromlen) ppc_st32(m, fromlen, 16);
  if (n < d.data.size()) return fail(kWsaEMsgSize);
  return n;
}

void fill_random(uint8_t* m, uint32_t p, uint32_t n) {
  static std::mt19937 rng(std::random_device{}());
  for (uint32_t i = 0; i < n; i++) ppc_st8(m, p + i, (uint8_t)rng());
}

}  // namespace

void net::deliver(uint32_t src_vip, uint16_t src_port, uint16_t dst_port, bool broadcast, const uint8_t* data, uint32_t len) {
  queue_datagram(src_vip, src_port, dst_port, broadcast, data, len);
}

// NetDll_XNetStartup(caller, XNetStartupParams*) / NetDll_XNetCleanup(caller)
XBOX_IMPORT(NetDll_XNetStartup) { ret(ctx, 0); }
XBOX_IMPORT(NetDll_XNetCleanup) { ret(ctx, 0); }

// NetDll_WSAStartup(caller, WORD Version, WSADATA*): WSADATA (0x190) {u16 wVersion, u16 wHighVersion,
// char szDescription[257], char szSystemStatus[129], u16 iMaxSockets, u16 iMaxUdpDg, ptr lpVendorInfo}
XBOX_IMPORT(NetDll_WSAStartup) {
  uint32_t version = arg(ctx, 1) & 0xFFFF, data = arg(ctx, 2);
  if (data) {
    for (uint32_t o = 0; o < 0x190; o += 4) ppc_st32(mem_base, data + o, 0);
    ppc_st16(mem_base, data + 0, (uint16_t)version);
    ppc_st16(mem_base, data + 2, 0x0202);
    const char desc[] = "WinSock 2.0";
    for (uint32_t i = 0; i < sizeof(desc); i++) ppc_st8(mem_base, data + 4 + i, (uint8_t)desc[i]);
  }
  ret(ctx, 0);
}
XBOX_IMPORT(NetDll_WSACleanup) { ret(ctx, 0); }
XBOX_IMPORT(NetDll_WSAGetLastError) { ret(ctx, t_last_error); }
// NetDll_WSACreateEvent(): evento manual não sinalizado (WSAEVENT = HANDLE).
XBOX_IMPORT(NetDll_WSACreateEvent) { ret(ctx, create_event_handle(true, false)); }

// NetDll_socket(caller, af, type, protocol): UDP (17) ou VDP (254) em datagramas; TCP cria-se, mas não liga.
XBOX_IMPORT(NetDll_socket) {
  const uint32_t af = arg(ctx, 1), type = arg(ctx, 2), proto = arg(ctx, 3);
  std::fprintf(stderr, "[xnet] socket(af %u, tipo %u, proto %u)\n", af, type, proto);
  if (af != 2) {
    t_last_error = kWsaEAfNoSupport;
    return ret_status(ctx, kInvalidSocket);
  }
  auto s = std::make_shared<Socket>();
  s->type = type;
  std::lock_guard<std::mutex> lock(g_mu);
  const uint32_t h = g_next_handle += 4;
  g_sockets[h] = s;
  ret(ctx, h);
}

// NetDll_closesocket(caller, s): acorda quem estiver bloqueado em recv.
XBOX_IMPORT(NetDll_closesocket) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_sockets.find(arg(ctx, 1));
  if (it == g_sockets.end()) return ret_status(ctx, fail(kWsaENotSock));
  it->second->closed = true;
  it->second->cv.notify_all();
  g_sockets.erase(it);
  ret(ctx, 0);
}

// NetDll_ioctlsocket(caller, s, cmd, u_long* argp)
XBOX_IMPORT(NetDll_ioctlsocket) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t cmd = arg(ctx, 2), argp = arg(ctx, 3);
  if (cmd == kFionbio) {
    s->nonblocking = ppc_ld32(mem_base, argp) != 0;
  } else if (cmd == kFionread) {
    ppc_st32(mem_base, argp, s->queue.empty() ? 0 : (uint32_t)s->queue.front().data.size());
  } else {
    return ret_status(ctx, fail(kWsaEInval));
  }
  ret(ctx, 0);
}

// NetDll_setsockopt(caller, s, level, optname, optval, optlen): SO_BROADCAST, etc. aceitam-se sem efeito.
XBOX_IMPORT(NetDll_setsockopt) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t level = arg(ctx, 2), opt = arg(ctx, 3), val = arg(ctx, 4), len = arg(ctx, 5);
  if (level == kSolSocket && val && len >= 4) {
    const uint32_t v = ppc_ld32(mem_base, val);
    if (opt == kSoReuseAddr) s->reuse = v != 0;
    else if (opt == kSoRcvTimeo) s->recv_timeout_ms = v;
    else if (opt == kSoSndBuf) s->snd_buf = v;
    else if (opt == kSoRcvBuf) s->rcv_buf = v;
  }
  ret(ctx, 0);
}

// NetDll_getsockopt(caller, s, level, optname, optval, optlen*)
XBOX_IMPORT(NetDll_getsockopt) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t opt = arg(ctx, 3), val = arg(ctx, 4), lenp = arg(ctx, 5);
  uint32_t v = 0;
  if (opt == kSoSndBuf) v = s->snd_buf;
  else if (opt == kSoRcvBuf) v = s->rcv_buf;
  else if (opt == kSoRcvTimeo) v = s->recv_timeout_ms;
  else if (opt == kSoType) v = s->type;
  else if (opt == kSoReuseAddr) v = s->reuse;
  if (val) ppc_st32(mem_base, val, v);
  if (lenp) ppc_st32(mem_base, lenp, 4);
  ret(ctx, 0);
}

// NetDll_getsockname(caller, s, sockaddr*, namelen*)
XBOX_IMPORT(NetDll_getsockname) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t a = arg(ctx, 2), lenp = arg(ctx, 3);
  ppc_st16(mem_base, a + 0, 2);
  ppc_st16(mem_base, a + 2, s->port);
  ppc_st32(mem_base, a + 4, net::local_vip());
  ppc_st32(mem_base, a + 8, 0);
  ppc_st32(mem_base, a + 12, 0);
  if (lenp) ppc_st32(mem_base, lenp, 16);
  ret(ctx, 0);
}

// NetDll_bind(caller, s, sockaddr_in*, namelen)
XBOX_IMPORT(NetDll_bind) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint16_t port = ppc_ld16(mem_base, arg(ctx, 2) + 2);
  if (port) {
    if (port_in_use(port) && !s->reuse) return ret_status(ctx, fail(kWsaEAddrInUse));
    s->port = port;
  } else {
    ensure_port(*s);
  }
  std::fprintf(stderr, "[xnet] bind(0x%X) porta %u\n", arg(ctx, 1), s->port);
  ret(ctx, 0);
}

// NetDll_connect(caller, s, sockaddr_in*, namelen): em UDP só fixa o destino.
XBOX_IMPORT(NetDll_connect) {
  std::lock_guard<std::mutex> lock(g_mu);
  auto s = find_socket(arg(ctx, 1));
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t a = arg(ctx, 2);
  const uint32_t ip = ppc_ld32(mem_base, a + 4);
  std::fprintf(stderr, "[xnet] connect(0x%X) %u.%u.%u.%u:%u\n", arg(ctx, 1), ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255,
               ppc_ld16(mem_base, a + 2));
  if (s->type == kSockStream) return ret_status(ctx, fail(kWsaEConnRefused));
  ensure_port(*s);
  s->peer_ip = ip;
  s->peer_port = ppc_ld16(mem_base, a + 2);
  ret(ctx, 0);
}

// NetDll_recv(caller, s, buf, len, flags)
XBOX_IMPORT(NetDll_recv) {
  std::shared_ptr<Socket> s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = find_socket(arg(ctx, 1));
  }
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  ret_status(ctx, do_recv(s, mem_base, arg(ctx, 2), arg(ctx, 3), 0, 0));
}

// NetDll_recvfrom(caller, s, buf, len, flags, from, fromlen*)
XBOX_IMPORT(NetDll_recvfrom) {
  std::shared_ptr<Socket> s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = find_socket(arg(ctx, 1));
  }
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  ret_status(ctx, do_recv(s, mem_base, arg(ctx, 2), arg(ctx, 3), arg(ctx, 5), arg(ctx, 6)));
}

// NetDll_send(caller, s, buf, len, flags)
XBOX_IMPORT(NetDll_send) {
  std::shared_ptr<Socket> s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = find_socket(arg(ctx, 1));
    if (s) ensure_port(*s);
  }
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  if (!s->peer_port) return ret_status(ctx, fail(kWsaENotConn));
  ret_status(ctx, do_send(*s, s->peer_ip, s->peer_port, mem_base, arg(ctx, 2), arg(ctx, 3)));
}

// NetDll_sendto(caller, s, buf, len, flags, to, tolen)
XBOX_IMPORT(NetDll_sendto) {
  std::shared_ptr<Socket> s;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    s = find_socket(arg(ctx, 1));
    if (s) ensure_port(*s);
  }
  if (!s) return ret_status(ctx, fail(kWsaENotSock));
  const uint32_t to = arg(ctx, 5);
  if (!to) return ret_status(ctx, fail(kWsaEBadf));
  const uint32_t ip = ppc_ld32(mem_base, to + 4);
  const uint16_t port = ppc_ld16(mem_base, to + 2);
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) < 8)
    std::fprintf(stderr, "[xnet] sendto %u.%u.%u.%u:%u da porta %u, %u bytes\n", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255,
                 port, s->port, arg(ctx, 3));
  ret_status(ctx, do_send(*s, ip, port, mem_base, arg(ctx, 2), arg(ctx, 3)));
}

// NetDll_inet_addr(PCSTR), a única sem "caller": "a.b.c.d" -> IPv4 em ordem de rede (BE = valor natural no convidado).
XBOX_IMPORT(NetDll_inet_addr) {
  const char* s = arg(ctx, 0) ? reinterpret_cast<const char*>(PPC_HOST(mem_base, arg(ctx, 0))) : "";
  unsigned a, b, c, d;
  char tail;
  if (std::sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return ret(ctx, 0xFFFFFFFFu);  // INADDR_NONE
  ret(ctx, a << 24 | b << 16 | c << 8 | d);
}

// NetDll_XNetRandom(caller, BYTE* Buffer, DWORD Length)
XBOX_IMPORT(NetDll_XNetRandom) {
  fill_random(mem_base, arg(ctx, 1), arg(ctx, 2));
  ret(ctx, 0);
}
// NetDll_XNetCreateKey(caller, XNKID* (8), XNKEY* (16)): chave de sessão aleatória (System Link).
XBOX_IMPORT(NetDll_XNetCreateKey) {
  fill_random(mem_base, arg(ctx, 1), 8);
  fill_random(mem_base, arg(ctx, 2), 16);
  if (arg(ctx, 1)) ppc_st8(mem_base, arg(ctx, 1), ppc_ld8(mem_base, arg(ctx, 1)) & 0x0F);  // bits altos 0: System Link
  ret(ctx, 0);
}
XBOX_IMPORT(NetDll_XNetRegisterKey) { ret(ctx, 0); }
XBOX_IMPORT(NetDll_XNetUnregisterKey) { ret(ctx, 0); }
// NetDll_XNetXnAddrToInAddr(caller, XNADDR*, XNKID*, IN_ADDR*): o endereço LAN do XNADDR é o IP virtual.
XBOX_IMPORT(NetDll_XNetXnAddrToInAddr) {
  const uint32_t xna = arg(ctx, 1), ina = arg(ctx, 3);
  if (!xna || !ina) return ret(ctx, kWsaEInval);
  ppc_st32(mem_base, ina, ppc_ld32(mem_base, xna));
  ret(ctx, 0);
}

// NetDll_XNetGetTitleXnAddr / GetDebugXnAddr(caller, XNADDR*): XNADDR (0x24) = {IN_ADDR ina, IN_ADDR inaOnline,
// u16 wPortOnline, u8 abEnet[6], u8 abOnline[20]}. Pendente (0) enquanto o anfitrião não atribuir o IP virtual.
XBOX_IMPORT(NetDll_XNetGetTitleXnAddr) {
  const uint32_t a = arg(ctx, 1), vip = net::local_vip();
  for (uint32_t o = 0; o < 0x24 && a; o += 4) ppc_st32(mem_base, a + o, 0);
  if (!vip) return ret(ctx, 0);
  if (a) {
    ppc_st32(mem_base, a, vip);
    const uint8_t mac[6] = {0x00, 0x1D, 0xD8, 0x4D, 0x00, (uint8_t)vip};
    for (uint32_t i = 0; i < 6; i++) ppc_st8(mem_base, a + 0x0A + i, mac[i]);
  }
  ret(ctx, kXnAddrEthernet | kXnAddrStatic);
}
XBOX_IMPORT(NetDll_XNetGetDebugXnAddr) { ret(ctx, kXnAddrNone); }
// NetDll_XNetGetEthernetLinkStatus(caller): ativo, 100 Mbps, full duplex.
XBOX_IMPORT(NetDll_XNetGetEthernetLinkStatus) { ret(ctx, 0x0B); }

// DNS e QoS precisam de rede.
XBOX_IMPORT(NetDll_XNetDnsLookup) {  // (caller, PCSTR Host, WSAEVENT, XNDNS** Out)
  out32(mem_base, arg(ctx, 3), 0);
  ret(ctx, kWsaENetDown);
}
XBOX_IMPORT(NetDll_XNetDnsRelease) { ret(ctx, 0); }
XBOX_IMPORT(NetDll_XNetQosListen) { ret(ctx, 0); }
XBOX_IMPORT(NetDll_XNetQosLookup) { ret(ctx, kWsaENetDown); }
XBOX_IMPORT(NetDll_XNetQosRelease) { ret(ctx, 0); }

// XNetLogonGetTitleID(): título em execução.
XBOX_IMPORT(XNetLogonGetTitleID) { ret(ctx, kTitleId); }
