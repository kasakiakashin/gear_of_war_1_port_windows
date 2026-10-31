// xma_decoder.cpp — pacotes XMA2 → PCM com o descodificador "xma2" do FFmpeg carregado em runtime.
//
// Só se usam funções exportadas e o início estável das estruturas (iguais do FFmpeg 4 ao 7):
//   AVFrame:  data[8] em +0, nb_samples em +112, format em +116
//   AVPacket: data em +24, size em +32 (alocado com av_new_packet)
//   AVCodecParameters: codec_type +0, codec_id +4, extradata +16, extradata_size +24
// A taxa e os canais vão por AVOptions ("ar", "ch_layout" ou "ac"). O "xma2" do FFmpeg aceita pacotes inteiros de
// 2048 bytes (o cabeçalho de cada pacote diz onde começa o 1º quadro, e os quadros atravessam pacotes) e, com
// extradata de 34 bytes (XMA2WAVEFORMATEX), um fluxo por par de canais.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "apu/xma_decoder.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace apu {
namespace {

constexpr uint32_t kFrameSamples = 512;

struct Ffmpeg {
  const void* (*find_decoder_by_name)(const char*);
  void* (*alloc_context3)(const void*);
  int (*open2)(void*, const void*, void**);
  void (*free_context)(void**);
  void* (*parameters_alloc)();
  void (*parameters_free)(void**);
  int (*parameters_from_context)(void*, const void*);
  int (*parameters_to_context)(void*, const void*);
  void* (*packet_alloc)();
  int (*new_packet)(void*, int);
  void (*packet_unref)(void*);
  int (*send_packet)(void*, const void*);
  int (*receive_frame)(void*, void*);
  void (*flush_buffers)(void*);
  void* (*frame_alloc)();
  void (*frame_unref)(void*);
  int (*opt_set_int)(void*, const char*, int64_t, int);
  int (*opt_set)(void*, const char*, const char*, int);
  void* (*mallocz)(size_t);
  const void* codec = nullptr;
  void* packet = nullptr;
  void* frame = nullptr;
};
Ffmpeg g_ff;
bool g_ok = false;
std::atomic<uint64_t> g_frames{0};

HMODULE load_first(const std::string& dir, const char* prefix) {
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA((dir + "\\" + prefix + "*.dll").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return nullptr;
  FindClose(h);
  return LoadLibraryExA((dir + "\\" + fd.cFileName).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
}

template <typename T>
bool sym(HMODULE m, const char* name, T& out) {
  out = reinterpret_cast<T>(GetProcAddress(m, name));
  return out != nullptr;
}

bool load(const std::string& dir) {
  HMODULE util = load_first(dir, "avutil-");
  HMODULE codec = util ? load_first(dir, "avcodec-") : nullptr;
  if (!codec) return false;
  Ffmpeg& f = g_ff;
  bool ok = sym(codec, "avcodec_find_decoder_by_name", f.find_decoder_by_name) &&
            sym(codec, "avcodec_alloc_context3", f.alloc_context3) && sym(codec, "avcodec_open2", f.open2) &&
            sym(codec, "avcodec_free_context", f.free_context) &&
            sym(codec, "avcodec_parameters_alloc", f.parameters_alloc) &&
            sym(codec, "avcodec_parameters_free", f.parameters_free) &&
            sym(codec, "avcodec_parameters_from_context", f.parameters_from_context) &&
            sym(codec, "avcodec_parameters_to_context", f.parameters_to_context) &&
            sym(codec, "av_packet_alloc", f.packet_alloc) && sym(codec, "av_new_packet", f.new_packet) &&
            sym(codec, "av_packet_unref", f.packet_unref) && sym(codec, "avcodec_send_packet", f.send_packet) &&
            sym(codec, "avcodec_receive_frame", f.receive_frame) &&
            sym(codec, "avcodec_flush_buffers", f.flush_buffers) && sym(util, "av_frame_alloc", f.frame_alloc) &&
            sym(util, "av_frame_unref", f.frame_unref) && sym(util, "av_opt_set_int", f.opt_set_int) &&
            sym(util, "av_opt_set", f.opt_set) && sym(util, "av_mallocz", f.mallocz);
  if (!ok) return false;
  void (*log_level)(int) = nullptr;
  if (sym(util, "av_log_set_level", log_level)) log_level(std::getenv("GOW_TRACE_XMA") ? 32 : -8);  // AV_LOG_QUIET
  f.codec = f.find_decoder_by_name("xma2");
  if (!f.codec) return false;
  f.packet = f.packet_alloc();
  f.frame = f.frame_alloc();
  return f.packet && f.frame;
}

void* open_codec(uint32_t channels, uint32_t rate) {
  Ffmpeg& f = g_ff;
  void* ctx = f.alloc_context3(f.codec);
  if (!ctx) return nullptr;
  void* par = f.parameters_alloc();
  f.parameters_from_context(par, ctx);
  auto* p = static_cast<uint8_t*>(par);
  auto* extra = static_cast<uint8_t*>(f.mallocz(34 + 64));
  extra[0] = 1;  // XMA2WAVEFORMATEX: NumStreams (LE), ChannelMask…
  extra[2] = channels == 2 ? 3 : 4;
  std::memcpy(p + 16, &extra, sizeof(extra));
  int size = 34;
  std::memcpy(p + 24, &size, sizeof(size));
  f.parameters_to_context(ctx, par);  // copia o extradata
  f.parameters_free(&par);
  f.opt_set_int(ctx, "ar", rate, 0);
  if (f.opt_set(ctx, "ch_layout", channels == 2 ? "stereo" : "mono", 0) < 0) f.opt_set_int(ctx, "ac", channels, 0);
  if (f.open2(ctx, f.codec, nullptr) < 0) {
    f.free_context(&ctx);
    return nullptr;
  }
  return ctx;
}

int16_t to_s16(float v) {
  v *= 32767.0f;
  return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
}

void receive(XmaStream& s) {
  Ffmpeg& f = g_ff;
  auto* fr = static_cast<uint8_t*>(f.frame);
  while (f.receive_frame(s.codec, f.frame) >= 0) {
    int n = *reinterpret_cast<int*>(fr + 112), fmt = *reinterpret_cast<int*>(fr + 116);
    float* ch[2];
    std::memcpy(ch, fr, sizeof(ch));
    size_t base = s.pcm.size();
    s.pcm.resize(base + (size_t)n * s.channels);
    for (int i = 0; i < n; i++) {
      for (uint32_t c = 0; c < s.channels; c++) {
        float v = fmt == 8 ? ch[c][i] : fmt == 3 ? ch[0][i * s.channels + c] : 0.0f;  // FLTP / FLT
        s.pcm[base + (size_t)i * s.channels + c] = to_s16(v);
      }
    }
    g_frames.fetch_add((uint64_t)n / kFrameSamples, std::memory_order_relaxed);
    f.frame_unref(f.frame);
  }
}

}  // namespace

bool xma_decoder_init() {
  static bool done = false;
  if (done) return g_ok;
  done = true;
  // Procura: GOW_FFMPEG_DIR, depois ffmpeg\ junto do executável (empacotada pelo CMake). LoadLibraryEx precisa de
  // caminho absoluto para resolver as dependências na mesma pasta.
  std::string dir;
  if (const char* e = std::getenv("GOW_XMA"); !e || e[0] != '0') {
    std::vector<std::string> dirs;
    if (const char* d = std::getenv("GOW_FFMPEG_DIR")) dirs.push_back(d);
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string base = exe;
    base.resize(base.find_last_of("\\/") + 1);
    dirs.push_back(base + "ffmpeg");
    for (const std::string& d : dirs)
      if (load(d)) {
        dir = d;
        g_ok = true;
        break;
      }
  }
  if (g_ok) std::fprintf(stderr, "[xma] descodificador xma2 do FFmpeg carregado de %s\n", dir.c_str());
  else std::fprintf(stderr, "[xma] FFmpeg (xma2) indisponível: XMA sai em silêncio (falta ffmpeg\\ junto do gow.exe, ou GOW_FFMPEG_DIR)\n");
  return g_ok;
}

void xma_reset(XmaStream& s) {
  if (s.codec && g_ok) g_ff.flush_buffers(s.codec);
  s.pcm.clear();
  s.pcm_pos = 0;
}

void xma_flush(XmaStream& s) {
  if (s.codec && g_ok) g_ff.flush_buffers(s.codec);
}

void xma_decode_packet(XmaStream& s, const uint8_t* packet, uint32_t channels, uint32_t rate) {
  if (s.pcm_pos && s.pcm_pos == s.pcm.size()) {
    s.pcm.clear();
    s.pcm_pos = 0;
  }
  if (!g_ok) {
    uint32_t frames = packet[0] >> 2;  // quadros que começam no pacote
    s.channels = channels;
    s.pcm.resize(s.pcm.size() + (size_t)frames * kFrameSamples * channels, 0);
    g_frames.fetch_add(frames, std::memory_order_relaxed);
    return;
  }
  Ffmpeg& f = g_ff;
  if (!s.codec || s.channels != channels || s.rate != rate) {
    if (s.codec) f.free_context(&s.codec);
    s.codec = open_codec(channels, rate);
    s.channels = channels;
    s.rate = rate;
    if (!s.codec) {
      std::fprintf(stderr, "[xma] avcodec_open2 falhou (%u canais, %u Hz)\n", channels, rate);
      return;
    }
  }
  if (f.new_packet(f.packet, 2048) < 0) return;
  uint8_t* data;
  std::memcpy(&data, static_cast<uint8_t*>(f.packet) + 24, sizeof(data));
  std::memcpy(data, packet, 2048);
  int r = f.send_packet(s.codec, f.packet);
  f.packet_unref(f.packet);
  if (r < 0) return;  // pacote inválido: ignora
  receive(s);
}

uint64_t xma_frames() { return g_frames.load(); }

}  // namespace apu
