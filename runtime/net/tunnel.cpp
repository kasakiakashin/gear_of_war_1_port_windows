// tunnel.cpp — túnel UDP com descoberta por STUN e hole punching, sem servidor próprio.
//
// Um único socket UDP real serve o STUN e o tráfego, para o mapeamento do NAT ser o mesmo. O convite leva o token da
// sessão e os endereços público (STUN) e de LAN do anfitrião; o convidado envia HELLO para ambos até receber WELCOME.
// Se o NAT do anfitrião filtrar por endereço de origem, o HELLO não passa sem o anfitrião enviar antes para o convidado:
// o convidado copia o seu código de resposta e o anfitrião cola-o (F9 ou botões do menu F8), o que começa a enviar PUNCH para lá.
//
// Pacote: "GOWT", tipo (u8), token (u64), corpo; inteiros em big-endian. Tudo o que vem da rede é validado aqui: token
// certo, origem conhecida, IP virtual de origem igual ao atribuído, comprimentos.
#include "net/tunnel.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include "config.h"

namespace net {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMagic = 0x474F5754;  // "GOWT"
constexpr uint32_t kStunCookie = 0x2112A442;
constexpr size_t kHeader = 13;
constexpr uint32_t kMaxPayload = 8192;
constexpr int kMaxPeers = 7;
enum Type : uint8_t { kHello = 1, kWelcome = 2, kPing = 3, kData = 4, kPunch = 5 };
enum Role { kNone, kHost, kJoin };

struct Endpoint {
  uint32_t ip = 0;  // ordem natural (a.b.c.d = a<<24…)
  uint16_t port = 0;
  bool valid() const { return ip && port; }
  bool operator==(const Endpoint& o) const { return ip == o.ip && port == o.port; }
};

struct Peer {
  Endpoint ep;
  uint32_t vip;
  Clock::time_point last;
};

struct Punch {
  Endpoint ep;
  Clock::time_point until;
};

std::mutex g_mu;  // estado abaixo
SOCKET g_sock = INVALID_SOCKET;
Role g_role = kNone;
uint64_t g_token = 0;
Endpoint g_public, g_lan, g_stun_a, g_stun_b, g_public_b;
uint8_t g_stun_txid[12];
Clock::time_point g_stun_deadline;
bool g_stun_done = false, g_copy_pending = false;
std::vector<Peer> g_peers;    // anfitrião: convidados aceites
std::vector<Punch> g_punch;   // anfitrião: destinos a abrir
Endpoint g_host_a, g_host_b;  // convidado: endereços do convite
Endpoint g_host;              // convidado: o que respondeu
Clock::time_point g_host_last;
std::atomic<uint32_t> g_vip{kHostVip};
std::string g_message;  // resultado da última ação do menu

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(x >> 8);
  v.push_back((uint8_t)x);
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  put16(v, x >> 16);
  put16(v, (uint16_t)x);
}
void put64(std::vector<uint8_t>& v, uint64_t x) {
  put32(v, (uint32_t)(x >> 32));
  put32(v, (uint32_t)x);
}
uint16_t get16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t* p) { return (uint32_t)get16(p) << 16 | get16(p + 2); }
uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }

std::string ip_str(const Endpoint& e) {
  char b[32];
  std::snprintf(b, sizeof b, "%u.%u.%u.%u:%u", e.ip >> 24, e.ip >> 16 & 255, e.ip >> 8 & 255, e.ip & 255, e.port);
  return b;
}

void send_raw(const Endpoint& to, const void* data, size_t n) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(to.ip);
  a.sin_port = htons(to.port);
  sendto(g_sock, (const char*)data, (int)n, 0, (sockaddr*)&a, sizeof a);
}

std::vector<uint8_t> packet(Type t) {
  std::vector<uint8_t> v;
  v.reserve(64);
  put32(v, kMagic);
  v.push_back(t);
  put64(v, g_token);
  return v;
}

// ---- Códigos gow:// (base64 URL, sem preenchimento) ----

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string b64_encode(const std::vector<uint8_t>& d) {
  std::string s;
  uint32_t acc = 0;
  int bits = 0;
  for (uint8_t b : d) {
    acc = acc << 8 | b;
    bits += 8;
    while (bits >= 6) s += kB64[acc >> (bits -= 6) & 63];
  }
  if (bits) s += kB64[acc << (6 - bits) & 63];
  return s;
}

std::vector<uint8_t> b64_decode(const std::string& s) {
  std::vector<uint8_t> d;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    const char* p = c ? std::strchr(kB64, c) : nullptr;
    if (!p) break;
    acc = acc << 6 | (uint32_t)(p - kB64);
    bits += 6;
    if (bits >= 8) d.push_back((uint8_t)(acc >> (bits -= 8)));
  }
  return d;
}

constexpr uint8_t kCodeVersion = 1;

std::string make_code() {
  std::vector<uint8_t> v;
  v.push_back(kCodeVersion);
  put64(v, g_token);
  put32(v, g_public.ip);
  put16(v, g_public.port);
  if (g_role == kHost) {
    put32(v, g_lan.ip);
    put16(v, g_lan.port);
    return "gow://join/" + b64_encode(v);
  }
  return "gow://peer/" + b64_encode(v);
}

std::string trim_code(std::string s) {
  size_t a = s.find("gow://");
  if (a == std::string::npos) return {};
  s = s.substr(a);
  size_t b = s.find_first_of(" \t\r\n\"'");
  return b == std::string::npos ? s : s.substr(0, b);
}

// ---- Área de transferência ----

void clipboard_set(const std::string& text) {
  if (!OpenClipboard(nullptr)) return;
  EmptyClipboard();
  if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, text.size() + 1)) {
    std::memcpy(GlobalLock(h), text.c_str(), text.size() + 1);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_TEXT, h)) GlobalFree(h);
  }
  CloseClipboard();
}

std::string clipboard_get() {
  std::string out;
  if (!OpenClipboard(nullptr)) return out;
  if (HANDLE h = GetClipboardData(CF_TEXT)) {
    if (const char* p = (const char*)GlobalLock(h)) {
      out.assign(p, strnlen(p, 1024));
      GlobalUnlock(h);
    }
  }
  CloseClipboard();
  return out;
}

// ---- STUN ----

Endpoint resolve(const std::string& host_port) {
  size_t c = host_port.rfind(':');
  std::string host = host_port.substr(0, c), port = c == std::string::npos ? "19302" : host_port.substr(c + 1);
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  Endpoint e;
  if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0 && res) {
    auto* a = reinterpret_cast<sockaddr_in*>(res->ai_addr);
    e.ip = ntohl(a->sin_addr.s_addr);
    e.port = ntohs(a->sin_port);
    freeaddrinfo(res);
  }
  return e;
}

void stun_send() {
  std::vector<uint8_t> v;
  put16(v, 0x0001);  // Binding Request
  put16(v, 0);
  put32(v, kStunCookie);
  v.insert(v.end(), g_stun_txid, g_stun_txid + 12);
  if (g_stun_a.valid()) send_raw(g_stun_a, v.data(), v.size());
  if (g_stun_b.valid()) send_raw(g_stun_b, v.data(), v.size());
}

// Resposta do STUN: XOR-MAPPED-ADDRESS (ou MAPPED-ADDRESS).
void stun_receive(const uint8_t* p, size_t n, const Endpoint& from) {
  if (n < 20 || get16(p) != 0x0101 || std::memcmp(p + 8, g_stun_txid, 12) != 0) return;
  if (!(from == g_stun_a) && !(from == g_stun_b)) return;
  size_t end = std::min<size_t>(n, 20u + get16(p + 2));
  Endpoint found;
  for (size_t o = 20; o + 4 <= end;) {
    uint16_t type = get16(p + o), len = get16(p + o + 2);
    if (o + 4 + len > end) break;
    if ((type == 0x0020 || type == 0x0001) && len >= 8 && p[o + 5] == 1) {  // IPv4
      Endpoint e;
      e.port = get16(p + o + 6);
      e.ip = get32(p + o + 8);
      if (type == 0x0020) {
        e.port ^= kStunCookie >> 16;
        e.ip ^= kStunCookie;
      }
      found = e;
      if (type == 0x0020) break;
    }
    o += 4 + ((len + 3u) & ~3u);
  }
  if (!found.valid()) return;
  if (from == g_stun_a) g_public = found;
  else g_public_b = found;
}

void publish_code() {
  g_stun_done = true;
  if (!g_public.valid() && g_public_b.valid()) g_public = g_public_b;
  if (g_public.valid())
    std::fprintf(stderr, "[net] endereço público %s (LAN %s)\n", ip_str(g_public).c_str(), ip_str(g_lan).c_str());
  else
    std::fprintf(stderr, "[net] STUN sem resposta: só o endereço de LAN %s\n", ip_str(g_lan).c_str());
  if (g_public.valid() && g_public_b.valid() && !(g_public == g_public_b))
    std::fprintf(stderr, "[net] AVISO: NAT simétrico (%s vs %s): a ligação pela internet provavelmente falha sem VPN\n",
                 ip_str(g_public).c_str(), ip_str(g_public_b).c_str());
  const std::string code = make_code();
  std::fprintf(stderr, "[net] %s: %s\n", g_role == kHost ? "convite" : "código de resposta", code.c_str());
  std::ofstream("gow_invite.txt") << code << "\n";
  if (g_copy_pending) {
    g_copy_pending = false;
    clipboard_set(code);
    g_message = "Copiado para a área de transferência.";
    std::fprintf(stderr, "[net] copiado para a área de transferência\n");
  }
}

// ---- Tráfego ----

void forward_data(const std::vector<uint8_t>& pkt, uint32_t dst, uint32_t except) {
  for (const Peer& p : g_peers)
    if (p.vip != except && (dst == kBroadcastVip || dst == p.vip)) send_raw(p.ep, pkt.data(), pkt.size());
}

void handle_packet(const uint8_t* p, size_t n, const Endpoint& from) {
  if (n >= 8 && get32(p + 4) == kStunCookie) {
    std::lock_guard<std::mutex> lock(g_mu);
    stun_receive(p, n, from);
    return;
  }
  if (n < kHeader || get32(p) != kMagic) return;
  const uint8_t type = p[4];
  bool deliver_now = false;
  uint32_t src = 0, dst = 0;
  uint16_t sport = 0, dport = 0;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    if (get64(p + 5) != g_token) return;
    const uint8_t* b = p + kHeader;
    const size_t bn = n - kHeader;
    const auto now = Clock::now();
    if (g_role == kHost) {
      Peer* peer = nullptr;
      for (Peer& q : g_peers)
        if (q.ep == from) peer = &q;
      if (type == kHello) {
        if (!peer) {
          if ((int)g_peers.size() >= kMaxPeers) return;
          uint32_t vip = kHostVip + 1;
          for (bool used = true; used;) {
            used = false;
            for (const Peer& q : g_peers)
              if (q.vip == vip) used = true, vip++;
          }
          g_peers.push_back({from, vip, now});
          peer = &g_peers.back();
          std::fprintf(stderr, "[net] jogador ligado: %s = 10.77.0.%u\n", ip_str(from).c_str(), vip & 255);
        }
        peer->last = now;
        auto w = packet(kWelcome);
        put32(w, peer->vip);
        send_raw(from, w.data(), w.size());
        return;
      }
      if (!peer) return;
      peer->last = now;
      if (type != kData || bn < 12) return;
      src = get32(b), dst = get32(b + 4), sport = get16(b + 8), dport = get16(b + 10);
      if (src != peer->vip) return;
      if (dst != kHostVip) forward_data(std::vector<uint8_t>(p, p + n), dst, src);
      deliver_now = dst == kHostVip || dst == kBroadcastVip;
    } else if (g_role == kJoin) {
      if (type == kWelcome) {
        if (bn < 4 || !(from == g_host_a || from == g_host_b)) return;
        uint32_t vip = get32(b);
        if ((vip & 0xFFFFFF00u) != (kHostVip & 0xFFFFFF00u)) return;
        if (!g_host.valid()) std::fprintf(stderr, "[net] ligado ao anfitrião %s como 10.77.0.%u\n", ip_str(from).c_str(), vip & 255);
        g_host = from;
        g_host_last = now;
        g_vip = vip;
        return;
      }
      if (!g_host.valid() || !(from == g_host)) return;
      g_host_last = now;
      if (type != kData || bn < 12) return;
      src = get32(b), dst = get32(b + 4), sport = get16(b + 8), dport = get16(b + 10);
      deliver_now = dst == g_vip.load() || dst == kBroadcastVip;
    }
  }
  if (deliver_now && n - kHeader - 12 <= kMaxPayload)
    deliver(src, sport, dport, dst == kBroadcastVip, p + kHeader + 12, (uint32_t)(n - kHeader - 12));
}

void tick() {
  std::lock_guard<std::mutex> lock(g_mu);
  const auto now = Clock::now();
  if (!g_stun_done) {
    if (now >= g_stun_deadline || (g_public.valid() && (g_public_b.valid() || !g_stun_b.valid()))) publish_code();
    else stun_send();
  }
  const auto hello = packet(g_role == kHost ? kPing : kHello);
  if (g_role == kHost) {
    for (size_t i = 0; i < g_peers.size();) {
      if (now - g_peers[i].last > std::chrono::seconds(30)) {
        std::fprintf(stderr, "[net] jogador 10.77.0.%u sem resposta: removido\n", g_peers[i].vip & 255);
        g_peers.erase(g_peers.begin() + i);
      } else {
        send_raw(g_peers[i++].ep, hello.data(), hello.size());
      }
    }
    const auto punch = packet(kPunch);
    for (size_t i = 0; i < g_punch.size();) {
      if (now > g_punch[i].until) g_punch.erase(g_punch.begin() + i);
      else send_raw(g_punch[i++].ep, punch.data(), punch.size());
    }
  } else if (g_role == kJoin) {
    if (g_host.valid() && now - g_host_last > std::chrono::seconds(30)) {
      std::fprintf(stderr, "[net] anfitrião sem resposta: a tentar de novo\n");
      g_host = Endpoint{};
    }
    if (g_host.valid()) {
      send_raw(g_host, hello.data(), hello.size());
    } else {
      if (g_host_a.valid()) send_raw(g_host_a, hello.data(), hello.size());
      if (g_host_b.valid() && !(g_host_b == g_host_a)) send_raw(g_host_b, hello.data(), hello.size());
    }
  }
}

void tunnel_thread() {
  std::vector<uint8_t> buf(kMaxPayload + 64);
  auto next_tick = Clock::now();
  for (;;) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(g_sock, &rd);
    timeval tv{0, 100000};
    if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
      sockaddr_in a{};
      int alen = sizeof a;
      int n = recvfrom(g_sock, (char*)buf.data(), (int)buf.size(), 0, (sockaddr*)&a, &alen);
      if (n > 0) {
        Endpoint from;
        from.ip = ntohl(a.sin_addr.s_addr);
        from.port = ntohs(a.sin_port);
        handle_packet(buf.data(), (size_t)n, from);
      }
    }
    if (Clock::now() >= next_tick) {
      tick();
      next_tick = Clock::now() + std::chrono::milliseconds(500);
    }
  }
}

// Abre o socket, descobre os endereços e arranca a thread (uma vez). Chamada com g_mu tomado.
bool open_socket(uint16_t port) {
  if (g_sock != INVALID_SOCKET) return true;
  WSADATA wsa;
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_sock == INVALID_SOCKET) return false;
  // Sem isto, um ICMP "port unreachable" faz o recvfrom falhar com WSAECONNRESET.
  constexpr DWORD kSioUdpConnReset = 0x9800000C;
  BOOL off = FALSE;
  DWORD got = 0;
  WSAIoctl(g_sock, kSioUdpConnReset, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  if (bind(g_sock, (sockaddr*)&a, sizeof a) != 0) {
    std::fprintf(stderr, "[net] não consegui abrir a porta UDP %u (%d)\n", port, WSAGetLastError());
    closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    return false;
  }
  int alen = sizeof a;
  getsockname(g_sock, (sockaddr*)&a, &alen);
  g_lan.port = ntohs(a.sin_port);
  // IP da interface principal: connect em UDP só escolhe a rota, não envia nada.
  SOCKET probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_addr.s_addr = htonl(0x08080808);
  dst.sin_port = htons(53);
  if (connect(probe, (sockaddr*)&dst, sizeof dst) == 0) {
    sockaddr_in me{};
    alen = sizeof me;
    if (getsockname(probe, (sockaddr*)&me, &alen) == 0) g_lan.ip = ntohl(me.sin_addr.s_addr);
  }
  closesocket(probe);
  if (!g_lan.ip) g_lan.ip = 0x7F000001;

  std::random_device rd;
  for (uint8_t& b : g_stun_txid) b = (uint8_t)rd();
  const std::string server = config::get().stun_server;
  if (!server.empty() && server != "0") {
    g_stun_a = resolve(server);
    // 2º servidor do mesmo fornecedor: endereços públicos diferentes denunciam um NAT simétrico.
    if (server.rfind("stun.l.google.com", 0) == 0) g_stun_b = resolve("stun1.l.google.com:19302");
  }
  g_stun_deadline = Clock::now() + std::chrono::seconds(3);
  std::thread(tunnel_thread).detach();
  return true;
}

bool start_host() {
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_role != kNone) return true;
  if (!open_socket((uint16_t)config::get().tunnel_port)) return false;
  std::random_device rd;
  g_token = (uint64_t)rd() << 32 | rd();
  g_role = kHost;
  g_vip = kHostVip;
  std::fprintf(stderr, "[net] anfitrião do túnel na porta UDP %u\n", g_lan.port);
  return true;
}

bool start_join(const std::string& raw) {
  const std::string code = trim_code(raw);
  const std::string prefix = "gow://join/";
  if (code.rfind(prefix, 0) != 0) return false;
  const std::vector<uint8_t> d = b64_decode(code.substr(prefix.size()));
  if (d.size() != 21 || d[0] != kCodeVersion) {
    std::fprintf(stderr, "[net] convite inválido\n");
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_role != kNone) {
    std::fprintf(stderr, "[net] o túnel já está ativo\n");
    return false;
  }
  if (!open_socket((uint16_t)config::get().tunnel_port)) return false;
  g_token = get64(&d[1]);
  g_host_a.ip = get32(&d[9]), g_host_a.port = get16(&d[13]);
  g_host_b.ip = get32(&d[15]), g_host_b.port = get16(&d[19]);
  g_role = kJoin;
  g_vip = 0;
  std::fprintf(stderr, "[net] a ligar a %s / %s\n", ip_str(g_host_a).c_str(), ip_str(g_host_b).c_str());
  return true;
}

bool add_punch(const std::string& raw) {
  const std::string code = trim_code(raw);
  const std::string prefix = "gow://peer/";
  if (code.rfind(prefix, 0) != 0) return false;
  const std::vector<uint8_t> d = b64_decode(code.substr(prefix.size()));
  std::lock_guard<std::mutex> lock(g_mu);
  if (d.size() != 15 || d[0] != kCodeVersion || g_role != kHost || get64(&d[1]) != g_token) {
    std::fprintf(stderr, "[net] código de resposta inválido ou de outra sessão\n");
    return false;
  }
  Endpoint e;
  e.ip = get32(&d[9]), e.port = get16(&d[13]);
  if (!e.valid()) return false;
  g_punch.push_back({e, Clock::now() + std::chrono::seconds(60)});
  std::fprintf(stderr, "[net] a abrir caminho até %s\n", ip_str(e).c_str());
  return true;
}

}  // namespace

void start_from_config() {
  const config::Config& c = config::get();
  if (!c.join_tunnel.empty()) start_join(c.join_tunnel);
  else if (c.tunnel_host) start_host();
}

uint32_t local_vip() { return g_vip.load(); }

void send(uint32_t dst_vip, uint16_t src_port, uint16_t dst_port, const uint8_t* data, uint32_t len) {
  if (len > kMaxPayload) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_role == kNone || g_sock == INVALID_SOCKET) return;
  const uint32_t src = g_vip.load();
  if (!src) return;
  std::vector<uint8_t> v = packet(kData);
  put32(v, src);
  put32(v, dst_vip);
  put16(v, src_port);
  put16(v, dst_port);
  v.insert(v.end(), data, data + len);
  if (g_role == kHost) forward_data(v, dst_vip, kHostVip);
  else if (g_host.valid()) send_raw(g_host, v.data(), v.size());
}

void on_copy_key() {
  if (!start_host()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_stun_done) {
    const std::string code = make_code();
    clipboard_set(code);
    g_message = "Copiado para a área de transferência.";
    std::fprintf(stderr, "[net] copiado: %s\n", code.c_str());
  } else {
    g_copy_pending = true;  // copia quando o STUN acabar
    g_message = "A descobrir o endereço público...";
  }
}

void on_paste_key() {
  const std::string text = clipboard_get();
  bool ok = false;
  const char* done = "";
  if (text.find("gow://join/") != std::string::npos) ok = start_join(text), done = "A ligar ao anfitrião...";
  else if (text.find("gow://peer/") != std::string::npos) ok = add_punch(text), done = "A abrir caminho até o convidado...";
  else std::fprintf(stderr, "[net] a área de transferência não tem um código gow://\n");
  std::lock_guard<std::mutex> lock(g_mu);
  g_message = ok ? done : "A área de transferência não tem um código gow:// válido para esta sessão.";
}

bool is_joined() {
  std::lock_guard<std::mutex> lock(g_mu);
  return g_role == kJoin;
}

std::string menu_status() {
  std::lock_guard<std::mutex> lock(g_mu);
  std::string s = "Estado: ";
  char b[96];
  if (g_role == kHost) {
    std::snprintf(b, sizeof b, "anfitrião, %zu jogador(es) ligado(s).", g_peers.size());
    s += b;
  } else if (g_role == kJoin) {
    s += g_host.valid() ? "ligado ao anfitrião." : "a ligar... Se demorar, copie o código de resposta e envie-o ao anfitrião.";
  } else {
    s += "sem ligação.";
  }
  if (!g_message.empty()) s += "\n" + g_message;
  return s;
}

std::string status() {
  std::lock_guard<std::mutex> lock(g_mu);
  char b[96];
  if (g_role == kHost)
    std::snprintf(b, sizeof b, " | Anfitriao: %zu ligado(s), F9 copia o convite", g_peers.size());
  else if (g_role == kJoin)
    std::snprintf(b, sizeof b, "%s", g_host.valid() ? " | Ligado ao anfitriao" : " | A ligar... F9 copia o codigo de resposta");
  else
    return {};
  return b;
}

}  // namespace net
