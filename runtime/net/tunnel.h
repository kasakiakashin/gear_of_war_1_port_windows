// tunnel.h — túnel UDP P2P: faz parecer aos sockets virtuais do convidado (xam/xnet.cpp) que as máquinas estão na mesma
// LAN 10.77.0.0/24. Topologia em estrela: o anfitrião é 10.77.0.1 e encaminha o tráfego entre os outros.
//
// Só abre sockets reais depois de ativado (TunnelHost / JoinTunnel no gow_config.ini, ou F9 / F8 na janela).
#pragma once

#include <cstdint>
#include <string>

namespace net {

constexpr uint32_t kHostVip = 0x0A4D0001;       // 10.77.0.1
constexpr uint32_t kBroadcastVip = 0x0A4D00FF;  // 10.77.0.255

void start_from_config();  // chamado no arranque

// IP virtual desta máquina; 0 enquanto um convidado ainda não foi aceite pelo anfitrião.
uint32_t local_vip();

// Envia um datagrama do convidado para outra máquina (dst = IP virtual ou broadcast). Sem túnel ligado, perde-se.
void send(uint32_t dst_vip, uint16_t src_port, uint16_t dst_port, const uint8_t* data, uint32_t len);

// Implementado em xam/xnet.cpp: entrega um datagrama aos sockets virtuais ligados a dst_port.
void deliver(uint32_t src_vip, uint16_t src_port, uint16_t dst_port, bool broadcast, const uint8_t* data, uint32_t len);

// F9: copia o código desta máquina (anfitrião: convite; convidado: código de resposta). Sem túnel, torna-se anfitrião.
void on_copy_key();
// Colar: lê a área de transferência: gow://join/… liga-se; gow://peer/… (no anfitrião) abre caminho até esse convidado.
void on_paste_key();

// Estado para o menu F8 (textos em UTF-8).
struct MenuState {
  enum Phase { kOff, kWaiting, kConnected } phase = kOff;
  bool hosting = false, joined = false;
  int peers = 0;
  int ping_ms = -1;     // -1 = ainda sem medição
  std::string code;     // convite (anfitrião) ou código de resposta (convidado); vazio até o STUN acabar
  std::string message;  // resultado da última ação
};
MenuState menu_state();
void create_room();                         // torna esta máquina anfitriã, sem tocar na área de transferência
void connect_code(const std::string& text);  // gow://join/… liga-se; gow://peer/… (no anfitrião) abre caminho até esse convidado
void disconnect();
bool is_joined();

// Texto curto para o título da janela (vazio sem túnel).
std::string status();

}  // namespace net
