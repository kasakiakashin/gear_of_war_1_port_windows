// config.h — gow_config.ini (na pasta de trabalho), criado com os valores por omissão se não existir.
#pragma once

#include <string>

namespace config {

struct Config {
  std::string iso_path;  // imagem de disco do jogo (UTF-8); vazio = procurar
  float resolution_scale = 1.0f;  // resolução interna 3D: 1.0 = 720p, 1.5 = 1080p, 2.0 = 1440p, 3.0 = 4K
  bool fullscreen = false;        // sem bordas, na resolução do monitor; Alt+Enter alterna
  bool sharpening = false;        // FSR 1.0 no passe final (EASU se a saída for maior, RCAS)
  float sharpening_strength = 0.5f;  // 0..1
  float mouse_sensitivity = 1.0f;
  std::string player_name;  // nome no jogo (vazio = "Player")
  bool tunnel_host = false;  // anfitrião do túnel P2P no arranque
  std::string join_tunnel;   // gow://join/… do anfitrião
  int tunnel_port = 0;       // porta UDP real (0 = escolhida pelo sistema)
  std::string stun_server = "stun.l.google.com:19302";  // "0" desliga (só LAN)
};

const Config& get();  // carrega na 1ª chamada
Config& edit();       // a mesma instância, para o menu F8 (leituras noutras threads: valores simples, sem sincronização)
void save();          // grava o IsoPath e as opções de vídeo e rato no gow_config.ini, mantendo o resto do ficheiro

}  // namespace config
