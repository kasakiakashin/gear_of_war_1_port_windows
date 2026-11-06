// apu.h — áudio: driver de render do XAudio (audio.cpp) e descodificador XMA (xma.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace apu {

// Aloca a tabela de contextos XMA, publica-a no registrador ContextArrayAddress e arranca a thread do
// descodificador. Chamar depois de gm::init() e antes do código convidado.
void xma_stats(uint64_t& packets, uint64_t& skips, uint64_t& starved, uint64_t& loops);
void xma_init();
void xma_mmio_store(uint32_t ea, uint32_t v);  // stwbrx em 0x7FEA0000 (ppc_mmio_st32r)

// Saída de som do host (audio_out.cpp, WASAPI). output_init arranca o dispositivo (GOW_AUDIO=0 desliga).
void output_init();
bool output_active();
size_t output_queued();                                // amostras estéreo à espera de tocar
void output_submit_frame(const uint8_t* be_planar6);   // 256 amostras × 6 canais float BE planar
uint64_t output_underruns();

}  // namespace apu
