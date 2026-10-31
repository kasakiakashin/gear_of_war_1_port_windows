// xma_decoder.h — descodificação de pacotes XMA2 (2048 bytes) para PCM de 16 bits.
//
// Usa o descodificador "xma2" do FFmpeg (libavcodec) carregado em runtime, sem headers nem link: pasta ffmpeg\ junto do
// gow.exe (copiada pelo CMake) ou GOW_FFMPEG_DIR. Sem FFmpeg, cada pacote produz silêncio com a duração certa (número de
// quadros do cabeçalho × 512 amostras), o que mantém o ritmo das vozes.
#pragma once

#include <cstdint>
#include <vector>

namespace apu {

struct XmaStream {
  void* codec = nullptr;  // AVCodecContext*
  uint32_t channels = 0, rate = 0;
  std::vector<int16_t> pcm;  // PCM descodificado ainda não escrito no anel de saída (intercalado, nativo)
  size_t pcm_pos = 0;
};

bool xma_decoder_init();  // carrega o FFmpeg (uma vez); false = modo silencioso
void xma_flush(XmaStream& s);  // só o estado do descodificador; o PCM já descodificado fica
void xma_reset(XmaStream& s);  // descarta estado e PCM pendente (Clear / contexto novo)
// Descodifica um pacote e acrescenta o PCM a s.pcm. channels 1/2, rate em Hz.
void xma_decode_packet(XmaStream& s, const uint8_t* packet, uint32_t channels, uint32_t rate);

uint64_t xma_frames();

}  // namespace apu
