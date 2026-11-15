// config.h — gow_config.ini (na pasta de trabalho), criado com os valores por omissão se não existir.
#pragma once

#include <cstdint>
#include <string>

#include "keybinds.h"

namespace config {

struct Config {
  std::string iso_path;  // imagem de disco do jogo (UTF-8); vazio = procurar
  std::string language = "English";  // ver language.cpp; aplicado ao arrancar
  float volume = 1.0f;     // 0..1
  bool vsync = false;      // sincroniza a apresentação com o monitor
  int fps_limit = 60;      // 30 = ritmo do Xbox 360; 0 = ilimitado
  bool adaptive_fps = true;  // limite ≥ 60 que a cena não aguenta: 30 FPS estáveis em vez de solavancos
  bool invert_y = false;   // rato: eixo Y invertido
  uint16_t keys[keybinds::kActionCount];  // VK por ação
  Config() {
    for (int a = 0; a < keybinds::kActionCount; a++) keys[a] = keybinds::info(a).default_vk;
  }
  float resolution_scale = 1.0f;  // resolução interna 3D: 1.0 = 720p, 1.5 = 1080p, 2.0 = 1440p, 3.0 = 4K
  bool fullscreen = false;        // sem bordas, na resolução do monitor; Alt+Enter alterna
  bool sharpening = false;        // FSR 1.0 no passe final (EASU se a saída for maior, RCAS)
  float sharpening_strength = 0.5f;  // 0..1
  float mouse_sensitivity = 1.0f;
  // Gráficos avançados (aba "Avançado & RT" do F8 e do launcher). Tudo desligado = imagem da v0.9.1.
  std::string preset = "Custom";    // último preset aplicado (LowSpec, Original, Modern, Ultra) ou Custom
  int anisotropy = 0;               // 0 = como o jogo; 2/4/8/16 = anisotrópico nos samplers 2D lineares com mips
  bool fp16_targets = false;        // RTs 7e3 sem o limite de 31,875 (gama completa do FP16)
  bool tonemap = false;             // curva fílmica suave sobre a imagem final (já em LDR)
  float vibrance = 0.0f;            // 0..1: saturação seletiva das cores pouco saturadas
  bool gtao = false;                // oclusão ambiente em espaço de ecrã
  float gtao_intensity = 0.6f;
  bool ssrtgi = false;              // iluminação global de 1 rebote em espaço de ecrã
  float ssrtgi_intensity = 0.5f;
  bool ssr = false;                 // reflexos em espaço de ecrã
  float ssr_intensity = 0.4f;
  bool ss_half_res = true;          // efeitos de ecrã a meia resolução com upsample bilateral
  bool raytracing = false;          // DXR 1.1 (só indicador; ver d3d12_renderer.cpp)
  bool zero_copy = true;            // resolves cujo destino nunca é lido não são copiados
  std::string player_name;  // nome no jogo (vazio = "Player")
  bool tunnel_host = false;  // anfitrião do túnel P2P no arranque
  std::string join_tunnel;   // gow://join/… do anfitrião
  int tunnel_port = 0;       // porta UDP real (0 = escolhida pelo sistema)
  std::string stun_server = "stun.l.google.com:19302";  // "0" desliga (só LAN)
};

const Config& get();  // carrega na 1ª chamada
Config& edit();       // a mesma instância, para o menu F8 (leituras noutras threads: valores simples, sem sincronização)
void save();          // grava as opções do menu F8 e o IsoPath no gow_config.ini, mantendo o resto do ficheiro

// Presets da aba Vídeo: definem escala, FSR, FPS e os gráficos avançados (não tocam no ecrã inteiro, VSync, som nem teclas).
enum Preset { kPresetLowSpec, kPresetOriginal, kPresetModern, kPresetUltra, kPresetCount };
const char* preset_key(int p);  // "LowSpec", "Original", "Modern", "Ultra"
void apply_preset(Config& c, int p, bool raytracing_available);
int preset_index(const std::string& key);  // -1 = Custom

}  // namespace config
