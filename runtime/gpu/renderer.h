// renderer.h — renderização no host (D3D12) dos comandos PM4 do Xenos, chamada pela thread da GPU
// (command_processor.cpp). Sem janela/D3D12 (GOW_NO_WINDOW=1, ou falha a iniciar) tudo vira no-op.
#pragma once

#include <cstdint>
#include <vector>

namespace gpu {

// Registradores atuais (command_processor.cpp): 0..0x3FFF no MMIO, 0x4000+ (constantes) no host.
uint32_t register_value(uint32_t index);
const uint32_t* constant_registers();  // registradores 0x4000..0xFFFF (índice - 0x4000)
// Por bloco de constantes, o relógio global na última escrita que mudou um valor: 32 blocos de 16 vec4 ALU, 32 slots de fetch de
// textura (6 dwords), todo o fetch, bool/loop. Uma cópia feita no instante T (constant_clock) continua válida se nenhum bloco
// que cobre tiver geração > T.
constexpr uint32_t kConstGenFetchSlot = 32, kConstGenFetch = 64, kConstGenBool = 65, kConstGenBlocks = 66;
const uint64_t* constant_generations();
uint64_t constant_clock();
// Tempo acumulado (µs) do processador de comandos parado: anel vazio / em WAIT_REG_MEM.
uint64_t idle_micros();
uint64_t wait_micros();
uint64_t busy_micros();
// Predicated tiling (SET_BIN_MASK/SELECT) atual; só a thread da GPU.
uint64_t bin_select();
uint64_t bin_mask();
bool packet_predicated();  // o pacote em execução (desenho, resolve) tem o bit de predicação  // a executar lotes do anel (inclui desenhos, swaps e esperas)

namespace render {

bool init();
bool active();
// PM4_IM_LOAD / IM_LOAD_IMMEDIATE: microcódigo (ordem do host) do vertex (pixel = false) ou pixel shader.
void load_shader(bool pixel, std::vector<uint32_t> ucode);
// O mesmo, com o hash (hash_words) já calculado; não faz nada se for o shader já carregado.
void load_shader(bool pixel, const std::vector<uint32_t>& ucode, uint64_t hash);
// PM4_DRAW_INDX(_2): VGT_DRAW_INITIATOR, base/tamanho do DMA de índices (VGT_DMA_BASE/SIZE) e, para índices
// imediatos, os dwords que seguem no pacote.
void draw(uint32_t initiator, uint32_t dma_base, uint32_t dma_size, const uint32_t* immediate, uint32_t immediate_count);
// PM4_XE_SWAP (VdSwap): frontbuffer físico, largura, altura e a constante de fetch de textura do frontbuffer.
void swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t fetch[6]);
// Fase atual (diagnóstico do watchdog).
const char* phase();
// Grava a cache de PSOs do driver (user_data/shader_cache/pipelines.bin) se tiver entradas novas. Chamado antes de sair.
void save_caches();

}  // namespace render
}  // namespace gpu
