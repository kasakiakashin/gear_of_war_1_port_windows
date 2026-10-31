// gpu.h — processador de comandos PM4 (command_processor.cpp) usado pelos imports Vd* (video.cpp).
#pragma once

#include <cstdint>

namespace gpu {

// VdInitializeRingBuffer: base física do ring primário, tamanho = 1 << (size_log2 + 3) bytes.
void start_ring_buffer(uint32_t physical_base, uint32_t size_log2);
// VdEnableRingBufferRPtrWriteBack: endereço físico onde o RPTR é espelhado.
void set_rptr_writeback(uint32_t physical);
// VdSetGraphicsInterruptCallback: cb(source, data) — 0 = VBlank, 1 = PM4_INTERRUPT.
void set_interrupt_callback(uint32_t cb, uint32_t data);
// Quadros apresentados (pacotes de swap processados).
uint32_t swap_count();
// Estado do processador de comandos (para o relatório do watchdog).
void dump_state();

}  // namespace gpu
