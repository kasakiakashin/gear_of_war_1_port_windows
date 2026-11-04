// audio.cpp — XAudio (driver de render do kernel): ritmo da callback e entrega dos quadros à saída do host.
//
// O XAudio2/XACT do jogo regista uma callback com XAudioRegisterRenderDriverClient({callback, arg}, PDWORD driver).
// No hardware, o sistema chama-a a cada quadro de áudio (256 amostras a 48 kHz = 5,33 ms) e o motor de áudio do
// jogo mistura as vozes e entrega o quadro com XAudioSubmitRenderDriverFrame. É esse ritmo que faz as vozes
// avançarem e terminarem: sem ele, sons (e o que espera por eles — cenas da UI, falas) ficam parados.
// Aqui uma thread convidada dedicada chama a callback; os quadros entregues vão para a saída WASAPI (audio_out.cpp).
// Com a saída ativa o ritmo é o da placa de som: chama-se a callback sempre que a fila tem menos do que a latência
// pedida (~43 ms), mas nunca em rajada: o WASAPI retira ~10 ms de uma vez, e duas misturas seguidas não dariam tempo
// ao XMA para entregar mais amostras (a voz ficava sem dados, um estalo no início do quadro). Sem saída (GOW_AUDIO=0 ou
// sem dispositivo) usa-se um temporizador de 5,33 ms.
//
// XMA: ver xma.cpp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "apu/apu.h"
#include "apu/xma_decoder.h"
#include "guest_memory.h"
#include "kernel/kernel.h"
#include "kernel/thread.h"

using namespace kernel;

namespace {

constexpr uint32_t kDriverMagic = 0x41550000u;  // 'AU' | índice, como no Xenia
constexpr int kMaxClients = 8;
constexpr double kFrameSeconds = 256.0 / 48000.0;

struct Client {
  std::atomic<uint32_t> callback{0}, arg{0};
  std::atomic<bool> active{false};
};
Client g_clients[kMaxClients];
std::atomic<uint64_t> g_frames_submitted{0};
std::once_flag g_thread_once;

DWORD WINAPI audio_thread(LPVOID) {
  auto* ctx = new PPCContext;
  GuestThread* t = create_guest_thread(*ctx, 0x10000);
  std::fprintf(stderr, "[audio] thread do driver de render = thread convidada %u\n", t->thread_id);
  PPCContext saved = *ctx;
  auto next = std::chrono::steady_clock::now();
  const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      std::chrono::duration<double>(kFrameSeconds));
  uint64_t ticks = 0;
  static const size_t target_queued = [] {
    const char* e = std::getenv("GOW_AUDIO_QUEUE");  // em quadros de 256 amostras
    return (size_t)256 * (e ? (size_t)std::atoi(e) : 8);  // 43 ms: folga para frames lentos e para o XMA
  }();
  const auto min_gap = std::chrono::microseconds(3500);
  auto last_call = std::chrono::steady_clock::now();
  for (;;) {
    if (apu::output_active()) {
      for (;;) {
        const size_t queued = apu::output_queued();
        const bool spaced = std::chrono::steady_clock::now() - last_call >= min_gap;
        if (queued < target_queued / 2 || (queued < target_queued && spaced)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      last_call = std::chrono::steady_clock::now();
    } else {
      next += period;
      std::this_thread::sleep_until(next);
      auto now = std::chrono::steady_clock::now();
      if (now - next > std::chrono::milliseconds(100)) next = now;  // atrasado demais: não tenta recuperar
    }
    if (++ticks % 3750 == 0)  // ~20 s
    {
      uint64_t packets, skips, starved, loops;
      apu::xma_stats(packets, skips, starved, loops);
      std::fprintf(stderr, "[audio] %llu quadros entregues, %llu quadros XMA descodificados, %llu buracos na saída, %llu pacotes rejeitados; XMA: %llu pacotes, %llu com salto, %llu à espera de entrada, %llu voltas de loop\n",
                   (unsigned long long)g_frames_submitted.load(), (unsigned long long)apu::xma_frames(),
                   (unsigned long long)apu::output_underruns(), (unsigned long long)apu::xma_rejected(), (unsigned long long)packets, (unsigned long long)skips,
                   (unsigned long long)starved, (unsigned long long)loops);
    }
    for (Client& c : g_clients) {
      if (!c.active.load()) continue;
      *ctx = saved;
      ctx->r[3] = c.arg.load();
      run_guest(t, *ctx, c.callback.load());
    }
  }
}

}  // namespace

// XAudioRegisterRenderDriverClient(PXAUDIO_CALLBACK {callback, context}, PDWORD Driver) -> NTSTATUS
XBOX_IMPORT(XAudioRegisterRenderDriverClient) {
  uint32_t cb_ptr = arg(ctx, 0), driver_ptr = arg(ctx, 1);
  if (!cb_ptr || !driver_ptr) return ret_status(ctx, kStatusInvalidParameter);
  uint32_t cb = ppc_ld32(mem_base, cb_ptr), cb_arg = ppc_ld32(mem_base, cb_ptr + 4);
  for (int i = 0; i < kMaxClients; i++) {
    if (g_clients[i].active.load()) continue;
    g_clients[i].callback = cb;
    g_clients[i].arg = cb_arg;
    g_clients[i].active = true;
    ppc_st32(mem_base, driver_ptr, kDriverMagic | (uint32_t)i);
    std::fprintf(stderr, "[audio] cliente %d: callback 0x%08X(0x%08X), quadro de %.2f ms\n", i, cb, cb_arg,
                 kFrameSeconds * 1000);
    std::call_once(g_thread_once, [] {
      // Prioridade alta: a mistura do XAudio não pode esperar pelas threads do jogo nem pelo FXC (a fila tem ~40 ms). As critical
      // sections do convidado bloqueiam num evento (não giram), por isso subir a prioridade não prende o dono do lock.
      HANDLE h = CreateThread(nullptr, 16u << 20, audio_thread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
      if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
        CloseHandle(h);
      }
    });
    return ret_status(ctx, kStatusSuccess);
  }
  ret_status(ctx, kStatusNoMemory);
}

// XAudioUnregisterRenderDriverClient(DWORD Driver)
XBOX_IMPORT(XAudioUnregisterRenderDriverClient) {
  uint32_t i = arg(ctx, 0) & 0xFFFF;
  if ((arg(ctx, 0) & 0xFFFF0000u) == kDriverMagic && i < kMaxClients) g_clients[i].active = false;
}

// XAudioSubmitRenderDriverFrame(DWORD Driver, PFLOAT Samples): 256 amostras × 6 canais (float BE, planar).
XBOX_IMPORT(XAudioSubmitRenderDriverFrame) {
  g_frames_submitted.fetch_add(1, std::memory_order_relaxed);
  if (arg(ctx, 1)) apu::output_submit_frame(PPC_HOST(mem_base, arg(ctx, 1)));
  ret_status(ctx, kStatusSuccess);
}

// XAudioGetVoiceCategoryVolumeChangeMask(DWORD Driver?, PDWORD Mask): nenhuma categoria mudou.
XBOX_IMPORT(XAudioGetVoiceCategoryVolumeChangeMask) {
  out32(mem_base, arg(ctx, 1), 0);
  ret_status(ctx, kStatusSuccess);
}

// XAudioGetVoiceCategoryVolume(DWORD Category, PFLOAT Volume): volume total.
XBOX_IMPORT(XAudioGetVoiceCategoryVolume) {
  if (arg(ctx, 1)) ppc_stf32(mem_base, arg(ctx, 1), 1.0f);
  ret_status(ctx, kStatusSuccess);
}
