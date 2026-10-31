#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace config {
namespace {

constexpr const char* kPath = "gow_config.ini";

const char kDefaults[] =
    "; gow_config.ini - configuracao do port (apague o ficheiro para repor os valores).\n"
    "[Game]\n"
    "; Caminho da ISO do jogo. Vazio = procura um .iso na pasta do gow.exe e, se nao houver, pergunta ao abrir.\n"
    "IsoPath=\n"
    "; Idioma: English, German, French, Spanish, SpanishLatin, Italian, Korean, Chinese. O disco nao tem Portuguese,\n"
    "; Japanese, SimplifiedChinese, Polish nem Russian: com esses o jogo fica em ingles. Aplicado ao arrancar.\n"
    "Language=English\n"
    "[Audio]\n"
    "; Volume geral, de 0.0 a 1.0.\n"
    "Volume=1.00\n"
    "[Video]\n"
    "; Resolucao interna real da geometria 3D: 1.0 = 720p, 1.5 = 1080p, 2.0 = 1440p, 3.0 = 4K (a janela abre desse\n"
    "; tamanho, limitada ao ecra). Mais custo de GPU.\n"
    "ResolutionScale=1.0\n"
    "; 0 = janela, 1 = ecra inteiro sem bordas (Alt+Enter alterna durante o jogo).\n"
    "Fullscreen=0\n"
    "; FSR 1.0 no passe final: EASU quando a saida e maior que a resolucao interna, e RCAS. 0 = desligado, 1 = ligado.\n"
    "Sharpening_FSR1_CAS=0\n"
    "; Intensidade do RCAS, de 0.0 a 1.0.\n"
    "SharpeningStrength=0.5\n"
    "; 1 = sincroniza com o monitor.\n"
    "VSync=0\n"
    "; Limite de FPS: 30 (como no Xbox 360), 60, 120 ou 0 = ilimitado. O tempo do jogo continua real.\n"
    "FpsLimit=60\n"
    "[Input]\n"
    "; Sensibilidade do rato (mira).\n"
    "MouseSensitivity=1.0\n"
    "; 1 = eixo Y do rato invertido.\n"
    "InvertY=0\n"
    "; Teclas (ou Mouse1..Mouse5, Space, Enter, Shift, Ctrl, Alt, Up, F1...; None = sem tecla). Esc, F8, F9 e F10 sao fixas.\n"
    "Key_MoveForward=W\n"
    "Key_MoveBack=S\n"
    "Key_MoveLeft=A\n"
    "Key_MoveRight=D\n"
    "Key_Fire=Mouse1\n"
    "Key_Aim=Mouse2\n"
    "Key_CoverRun=Space\n"
    "Key_Melee=E\n"
    "Key_Reload=R\n"
    "Key_Look=Q\n"
    "Key_Interact=C\n"
    "Key_Tactical=Shift\n"
    "Key_Zoom=Mouse3\n"
    "Key_Crouch=Ctrl\n"
    "Key_Weapon1=1\n"
    "Key_Weapon2=2\n"
    "Key_Weapon3=3\n"
    "Key_Weapon4=4\n"
    "Key_LookUp=I\n"
    "Key_LookDown=K\n"
    "Key_LookLeft=J\n"
    "Key_LookRight=L\n"
    "Key_Start=Enter\n"
    "Key_Back=Backspace\n"
    "[Network]\n"
    "; Nome no jogo (vazio = Player).\n"
    "PlayerName=\n"
    "; Tunel P2P para System Link / Co-op pela internet. Partilhar um convite revela o IP publico a quem o receber.\n"
    "; TunnelHost=1 cria a sessao e grava o convite em gow_invite.txt (F9 na janela faz o mesmo e copia-o).\n"
    "TunnelHost=0\n"
    "; Quem entra: JoinTunnel=gow://join/... (ou copie o convite e use o botao Colar no menu F8).\n"
    "JoinTunnel=\n"
    "; Porta UDP real (0 = escolhida pelo sistema; fixe-a para reencaminhar no router).\n"
    "TunnelPort=0\n"
    "; Servidor STUN usado para descobrir o endereco publico (0 = so LAN).\n"
    "StunServer=stun.l.google.com:19302\n";

std::string trim(std::string s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

Config load() {
  Config c;
  std::ifstream in(kPath);
  if (!in) {
    std::ofstream(kPath) << kDefaults;
    std::fprintf(stderr, "[config] %s criado com os valores por omissão\n", kPath);
    return c;
  }
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq)), val = trim(line.substr(eq + 1));
    const float f = (float)std::atof(val.c_str());
    if (key == "IsoPath") {
      if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
      c.iso_path = val;
    } else if (key.rfind("Key_", 0) == 0) {
      bool known = false;
      for (int a = 0; a < keybinds::kActionCount; a++)
        if (key.compare(4, std::string::npos, keybinds::info(a).key) == 0) c.keys[a] = keybinds::vk_of(val), known = true;
      if (!known) std::fprintf(stderr, "[config] chave desconhecida: %s\n", key.c_str());
    } else if (key == "Language") c.language = val;
    else if (key == "Volume") c.volume = std::clamp(f, 0.0f, 1.0f);
    else if (key == "VSync") c.vsync = f != 0.0f;
    else if (key == "FpsLimit") c.fps_limit = std::clamp((int)f, 0, 1000);
    else if (key == "InvertY") c.invert_y = f != 0.0f;
    else if (key == "ResolutionScale") c.resolution_scale = std::clamp(f, 0.5f, 4.0f);
    else if (key == "Fullscreen") c.fullscreen = f != 0.0f;
    else if (key == "Sharpening_FSR1_CAS") c.sharpening = f != 0.0f;
    else if (key == "SharpeningStrength") c.sharpening_strength = std::clamp(f, 0.0f, 1.0f);
    else if (key == "MouseSensitivity") c.mouse_sensitivity = std::clamp(f, 0.05f, 20.0f);
    else if (key == "PlayerName") c.player_name = val;
    else if (key == "TunnelHost") c.tunnel_host = f != 0.0f;
    else if (key == "JoinTunnel") c.join_tunnel = val;
    else if (key == "TunnelPort") c.tunnel_port = std::clamp((int)f, 0, 65535);
    else if (key == "StunServer") c.stun_server = val;
    else std::fprintf(stderr, "[config] chave desconhecida: %s\n", key.c_str());
  }
  // Para correr duas instâncias com o mesmo gow_config.ini.
  if (const char* e = std::getenv("GOW_TUNNEL_HOST")) c.tunnel_host = e[0] == '1';
  if (const char* e = std::getenv("GOW_JOIN_TUNNEL")) c.join_tunnel = e;
  std::fprintf(stderr, "[config] escala %.2f, ecrã inteiro %d, CAS %d (%.2f), rato %.2f\n", c.resolution_scale, c.fullscreen,
               c.sharpening, c.sharpening_strength, c.mouse_sensitivity);
  return c;
}

}  // namespace

Config& edit() {
  static Config c = load();
  return c;
}

const Config& get() { return edit(); }

void save() {
  const Config& c = get();
  char b[32];
  auto num = [&](float v) {
    std::snprintf(b, sizeof b, "%.2f", v);
    return std::string(b);
  };
  std::vector<std::pair<std::string, std::string>> values = {{"IsoPath", c.iso_path},
                                                        {"Language", c.language},
                                                        {"Volume", num(c.volume)},
                                                        {"VSync", c.vsync ? "1" : "0"},
                                                        {"FpsLimit", std::to_string(c.fps_limit)},
                                                        {"InvertY", c.invert_y ? "1" : "0"},
                                                        {"ResolutionScale", num(c.resolution_scale)},
                                                        {"Fullscreen", c.fullscreen ? "1" : "0"},
                                                        {"Sharpening_FSR1_CAS", c.sharpening ? "1" : "0"},
                                                        {"SharpeningStrength", num(c.sharpening_strength)},
                                                        {"MouseSensitivity", num(c.mouse_sensitivity)}};
  for (int a = 0; a < keybinds::kActionCount; a++)
    values.push_back({std::string("Key_") + keybinds::info(a).key, keybinds::name_of(c.keys[a])});
  std::vector<std::string> lines;
  {
    std::ifstream in(kPath);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
  }
  std::string out;
  const int kValues = (int)values.size();
  std::vector<char> seen(values.size(), 0);
  for (const std::string& l : lines) {
    const std::string t = trim(l);
    size_t eq = t.find('=');
    bool replaced = false;
    if (eq != std::string::npos && t[0] != ';' && t[0] != '#') {
      const std::string key = trim(t.substr(0, eq));
      for (int i = 0; i < kValues; i++)
        if (key == values[i].first) {
          out += key + "=" + values[i].second + "\n";
          seen[i] = replaced = true;
        }
    }
    if (!replaced) out += l + "\n";
  }
  for (int i = 0; i < kValues; i++)
    if (!seen[i]) out += values[i].first + "=" + values[i].second + "\n";
  std::ofstream(kPath) << out;
}

}  // namespace config
