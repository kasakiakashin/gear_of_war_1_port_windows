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
    "; 1 = se a cena nao aguentar o limite (60 ou 120), desce para metade em vez de oscilar (menos solavancos).\n"
    "AdaptiveFps=1\n"
    "[Graphics]\n"
    "; Ultimo preset aplicado: LowSpec, Original, Modern, Ultra (ou Custom). So informativo: os valores abaixo mandam.\n"
    "Preset=Custom\n"
    "; Filtro anisotropico (0 = como no Xbox 360, 2, 4, 8, 16). So em texturas 2D com mips e filtro linear.\n"
    "AnisotropicFiltering=0\n"
    "; 1 = render targets HDR em FP16 completo, sem o limite de 31.875 do formato 7e3 do Xenos.\n"
    "Fp16RenderTargets=0\n"
    "; Curva filmica suave e vibrancia (0.0 a 1.0) sobre a imagem final.\n"
    "Tonemap=0\n"
    "Vibrance=0.00\n"
    "; Efeitos de espaco de ecra sobre a cena HDR antes do pos-processamento e do HUD (0 = desligado, 1 = ligado) e intensidade.\n"
    "GTAO=0\n"
    "GTAOIntensity=0.60\n"
    "SSRTGI=0\n"
    "SSRTGIIntensity=0.50\n"
    "SSR=0\n"
    "SSRIntensity=0.40\n"
    "; 1 = efeitos de ecra a meia resolucao com upsample bilateral (mais leve).\n"
    "ScreenSpaceHalfRes=1\n"
    "; DXR 1.1 (RayQuery): oclusao ambiente com raios sobre a geometria da cena. So em GPUs com DXR 1.1.\n"
    "RayTracing=0\n"
    "; 1 = resolves sem copia (a textura e o proprio render target, com copy-on-write). Mais lento neste jogo; ver README.\n"
    "ZeroCopyResolves=0\n"
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
    else if (key == "AdaptiveFps") c.adaptive_fps = f != 0.0f;
    else if (key == "InvertY") c.invert_y = f != 0.0f;
    else if (key == "ResolutionScale") c.resolution_scale = std::clamp(f, 0.5f, 4.0f);
    else if (key == "Fullscreen") c.fullscreen = f != 0.0f;
    else if (key == "Sharpening_FSR1_CAS") c.sharpening = f != 0.0f;
    else if (key == "SharpeningStrength") c.sharpening_strength = std::clamp(f, 0.0f, 1.0f);
    else if (key == "MouseSensitivity") c.mouse_sensitivity = std::clamp(f, 0.05f, 20.0f);
    else if (key == "Preset") c.preset = val;
    else if (key == "AnisotropicFiltering") {
      const int a = (int)f;
      c.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : 0;
    } else if (key == "Fp16RenderTargets") c.fp16_targets = f != 0.0f;
    else if (key == "Tonemap") c.tonemap = f != 0.0f;
    else if (key == "Vibrance") c.vibrance = std::clamp(f, 0.0f, 1.0f);
    else if (key == "GTAO") c.gtao = f != 0.0f;
    else if (key == "GTAOIntensity") c.gtao_intensity = std::clamp(f, 0.0f, 1.0f);
    else if (key == "SSRTGI") c.ssrtgi = f != 0.0f;
    else if (key == "SSRTGIIntensity") c.ssrtgi_intensity = std::clamp(f, 0.0f, 1.0f);
    else if (key == "SSR") c.ssr = f != 0.0f;
    else if (key == "SSRIntensity") c.ssr_intensity = std::clamp(f, 0.0f, 1.0f);
    else if (key == "ScreenSpaceHalfRes") c.ss_half_res = f != 0.0f;
    else if (key == "RayTracing") c.raytracing = f != 0.0f;
    else if (key == "ZeroCopyResolves") c.zero_copy = f != 0.0f;
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
                                                        {"AdaptiveFps", c.adaptive_fps ? "1" : "0"},
                                                        {"InvertY", c.invert_y ? "1" : "0"},
                                                        {"ResolutionScale", num(c.resolution_scale)},
                                                        {"Fullscreen", c.fullscreen ? "1" : "0"},
                                                        {"Sharpening_FSR1_CAS", c.sharpening ? "1" : "0"},
                                                        {"SharpeningStrength", num(c.sharpening_strength)},
                                                        {"MouseSensitivity", num(c.mouse_sensitivity)},
                                                        {"Preset", c.preset},
                                                        {"AnisotropicFiltering", std::to_string(c.anisotropy)},
                                                        {"Fp16RenderTargets", c.fp16_targets ? "1" : "0"},
                                                        {"Tonemap", c.tonemap ? "1" : "0"},
                                                        {"Vibrance", num(c.vibrance)},
                                                        {"GTAO", c.gtao ? "1" : "0"},
                                                        {"GTAOIntensity", num(c.gtao_intensity)},
                                                        {"SSRTGI", c.ssrtgi ? "1" : "0"},
                                                        {"SSRTGIIntensity", num(c.ssrtgi_intensity)},
                                                        {"SSR", c.ssr ? "1" : "0"},
                                                        {"SSRIntensity", num(c.ssr_intensity)},
                                                        {"ScreenSpaceHalfRes", c.ss_half_res ? "1" : "0"},
                                                        {"RayTracing", c.raytracing ? "1" : "0"},
                                                        {"ZeroCopyResolves", c.zero_copy ? "1" : "0"}};
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

const char* preset_key(int p) {
  static const char* const k[kPresetCount] = {"LowSpec", "Original", "Modern", "Ultra"};
  return p >= 0 && p < kPresetCount ? k[p] : "Custom";
}

int preset_index(const std::string& key) {
  for (int p = 0; p < kPresetCount; p++)
    if (key == preset_key(p)) return p;
  return -1;
}

void apply_preset(Config& c, int p, bool raytracing_available) {
  struct Values {
    float scale;
    bool fsr;
    float fsr_strength;
    int fps, aniso;
    bool fp16, tonemap;
    float vibrance;
    bool gtao;
    float gtao_i;
    bool gi;
    float gi_i;
    bool ssr;
    float ssr_i;
    bool half, rt;
  };
  // PC Fraco: 540p ampliado pelo EASU para a janela, 30 FPS estáveis. Original: o que o Xbox 360 mostra (720p, 30 FPS).
  // PC Moderno: 1080p, 60 FPS, AF 8×, AO. Ultra: 1440p, AF 16×, AO + GI + reflexos a resolução completa, DXR se houver.
  static const Values v[kPresetCount] = {
      {0.75f, true, 0.60f, 30, 0, false, false, 0.0f, false, 0.6f, false, 0.5f, false, 0.4f, true, false},
      {1.0f, false, 0.50f, 30, 0, false, false, 0.0f, false, 0.6f, false, 0.5f, false, 0.4f, true, false},
      {1.5f, true, 0.50f, 60, 8, true, false, 0.15f, true, 0.6f, false, 0.5f, false, 0.4f, true, false},
      {2.0f, true, 0.50f, 60, 16, true, true, 0.25f, true, 0.8f, true, 0.5f, true, 0.5f, false, true},
  };
  if (p < 0 || p >= kPresetCount) return;
  const Values& x = v[p];
  c.resolution_scale = x.scale;
  c.sharpening = x.fsr;
  c.sharpening_strength = x.fsr_strength;
  c.fps_limit = x.fps;
  c.adaptive_fps = true;
  c.anisotropy = x.aniso;
  c.fp16_targets = x.fp16;
  c.tonemap = x.tonemap;
  c.vibrance = x.vibrance;
  c.gtao = x.gtao, c.gtao_intensity = x.gtao_i;
  c.ssrtgi = x.gi, c.ssrtgi_intensity = x.gi_i;
  c.ssr = x.ssr, c.ssr_intensity = x.ssr_i;
  c.ss_half_res = x.half;
  c.raytracing = x.rt && raytracing_available;
  c.zero_copy = false;
  c.preset = preset_key(p);
}

}  // namespace config
