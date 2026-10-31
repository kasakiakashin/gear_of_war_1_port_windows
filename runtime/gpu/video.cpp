// video.cpp — vídeo/GPU mínimos para o D3D do jogo passar da inicialização (sem renderização).
//
//  * Registradores da GPU Xenos: bloco MMIO de 64 KiB em 0x7FC80000 (registrador N em 0x7FC80000 + 4N),
//    confirmado por guest_memory como RAM simples (0 = GPU ociosa).
//  * Ring buffer de comandos: o D3D escreve pacotes PM4 e avança CP_RB_WPTR (registrador 0x1C5); o
//    processador de comandos (command_processor.cpp) consome-os, atualiza o RPTR e executa fences/interrupções.
#include <cstdio>

#include "gpu/gpu.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace {

// X_VIDEO_MODE (48 bytes BE): 1280×720 progressivo, widescreen, HD, 60 Hz, NTSC.
void write_video_mode(uint8_t* m, uint32_t p) {
  ppc_st32(m, p + 0x00, 1280);
  ppc_st32(m, p + 0x04, 720);
  ppc_st32(m, p + 0x08, 0);     // is_interlaced
  ppc_st32(m, p + 0x0C, 1);     // is_widescreen
  ppc_st32(m, p + 0x10, 1);     // is_hi_def
  ppc_stf32(m, p + 0x14, 60.0); // refresh_rate (float)
  ppc_st32(m, p + 0x18, 1);     // video_standard: NTSC-M
  ppc_st32(m, p + 0x1C, 0x4A);  // valores observados em hardware (Xenia)
  ppc_st32(m, p + 0x20, 0x01);
  for (uint32_t o = 0x24; o < 0x30; o += 4) ppc_st32(m, p + o, 0);
}

}  // namespace

XBOX_IMPORT(VdQueryVideoMode) { write_video_mode(mem_base, arg(ctx, 0)); }  // (X_VIDEO_MODE*)
XBOX_IMPORT(XGetVideoMode) { write_video_mode(mem_base, arg(ctx, 0)); }     // xam: mesmo formato
XBOX_IMPORT(VdQueryVideoFlags) { ret(ctx, 0x3); }  // widescreen | HD
XBOX_IMPORT(VdInitializeEngines) { ret(ctx, 1); }
XBOX_IMPORT(VdShutdownEngines) {}
XBOX_IMPORT(VdSetGraphicsInterruptCallback) {  // (callback, user_data)
  gpu::set_interrupt_callback(arg(ctx, 0), arg(ctx, 1));
}
XBOX_IMPORT(VdInitializeRingBuffer) {  // (PVOID physical_base, ULONG size_log2): tamanho = 1 << (log2 + 3) bytes
  std::fprintf(stderr, "[gpu] ring buffer: físico 0x%08X, %u bytes\n", arg(ctx, 0), 1u << (arg(ctx, 1) + 3));
  gpu::start_ring_buffer(arg(ctx, 0), arg(ctx, 1));
}
XBOX_IMPORT(VdEnableRingBufferRPtrWriteBack) {  // (PVOID physical_ptr, ULONG block_log2)
  gpu::set_rptr_writeback(arg(ctx, 0));
}
XBOX_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}
XBOX_IMPORT(VdCallGraphicsNotificationRoutines) { ret(ctx, 0); }
XBOX_IMPORT(VdIsHSIOTrainingSucceeded) { ret(ctx, 1); }
XBOX_IMPORT(VdRetrainEDRAM) { ret(ctx, 0); }
XBOX_IMPORT(VdRetrainEDRAMWorker) { ret(ctx, 0); }
XBOX_IMPORT(VdEnableDisableClockGating) { ret(ctx, 0); }
XBOX_IMPORT(VdPersistDisplay) { ret(ctx, 0); }
XBOX_IMPORT(VdGetCurrentDisplayGamma) {  // (DWORD* type, float* power)
  out32(mem_base, arg(ctx, 0), 1);
  if (arg(ctx, 1)) ppc_stf32(mem_base, arg(ctx, 1), 2.22222233f);
}
XBOX_IMPORT(VdGetSystemCommandBuffer) {  // (PVOID* p0, DWORD* p1): valores sentinela, como no Xenia
  out32(mem_base, arg(ctx, 0), 0xBEEF0000u);
  out32(mem_base, arg(ctx, 1), 0xBEEF0001u);
}

// VdSwap(PDWORD buffer, PVOID fetch, ...): o D3D reserva 64 dwords no ring para o kernel escrever os comandos de
// swap e passa a constante de fetch de textura (6 dwords BE) do frontbuffer. Como no Xenia: um pacote tipo 3
// PM4_XE_SWAP (0x64) com 'SWAP', endereço físico, largura e altura do frontbuffer + a constante (dwords 4..9).
XBOX_IMPORT(VdSwap) {
  uint32_t buf = arg(ctx, 0), fetch = arg(ctx, 1);
  uint32_t f[6] = {};
  if (fetch)
    for (int i = 0; i < 6; i++) f[i] = ppc_ld32(mem_base, fetch + 4 * i);
  uint32_t base = f[1] & 0xFFFFF000u, width = (f[2] & 0x1FFF) + 1, height = ((f[2] >> 13) & 0x1FFF) + 1;
  for (uint32_t i = 0; i < 64; i++) ppc_st32(mem_base, buf + 4 * i, 0);
  ppc_st32(mem_base, buf, (3u << 30) | (62u << 16) | (0x64u << 8));
  ppc_st32(mem_base, buf + 4, 0x53574150);  // 'SWAP'
  ppc_st32(mem_base, buf + 8, base);
  ppc_st32(mem_base, buf + 12, fetch ? width : 0);
  ppc_st32(mem_base, buf + 16, fetch ? height : 0);
  for (int i = 0; i < 6; i++) ppc_st32(mem_base, buf + 20 + 4 * i, f[i]);
  ret(ctx, 0);
}
