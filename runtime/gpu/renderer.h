// renderer.h — renderização no host (D3D12) dos comandos PM4 do Xenos, chamada pela thread da GPU
// (command_processor.cpp). Sem janela/D3D12 (GOW_NO_WINDOW=1, ou falha a iniciar) tudo vira no-op.
#pragma once

#include <cstdint>
#include <vector>

namespace gpu {

// Registradores atuais (command_processor.cpp): 0..0x3FFF no MMIO, 0x4000+ (constantes) no host.
uint32_t register_value(uint32_t index);
const uint32_t* constant_registers();  // registradores 0x4000..0xFFFF (índice - 0x4000)

namespace render {

bool init();
bool active();
// PM4_IM_LOAD / IM_LOAD_IMMEDIATE: microcódigo (ordem do host) do vertex (pixel = false) ou pixel shader.
void load_shader(bool pixel, std::vector<uint32_t> ucode);
// PM4_DRAW_INDX(_2): VGT_DRAW_INITIATOR, base/tamanho do DMA de índices (VGT_DMA_BASE/SIZE) e, para índices
// imediatos, os dwords que seguem no pacote.
void draw(uint32_t initiator, uint32_t dma_base, uint32_t dma_size, const uint32_t* immediate, uint32_t immediate_count);
// PM4_XE_SWAP (VdSwap): frontbuffer físico, largura, altura e a constante de fetch de textura do frontbuffer.
void swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t fetch[6]);
// Fase atual (diagnóstico do watchdog).
const char* phase();

}  // namespace render
}  // namespace gpu
