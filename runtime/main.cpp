// main.cpp — gow.exe: carrega o default.xex recompilado e executa o entry point do convidado.
//   gow.exe [base_image.bin | jogo.iso]          (sem argumentos: ver game_source.cpp)
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "gpu/gpu.h"
#include "apu/apu.h"
#include "config.h"
#include "game_source.h"
#include "net/tunnel.h"
#include "guest_memory.h"
#include "kernel/diag.h"
#include "kernel/thread.h"
#include "loader.h"
#include "ppc_context.h"
#include "xbox_import.h"

namespace {

constexpr uint32_t kMainStackSize = 0x40000;         // XEX DEFAULT_STACK_SIZE (256 KiB)
constexpr SIZE_T kHostStackSize = 256ull << 20;      // pilha host: recursão das rotinas traduzidas

// GOW_PEEK=0xA,0xB,...: imprime [a] e, se for um ponteiro válido, [[a]] (u32 BE).
void peek() {
  const char* list = std::getenv("GOW_PEEK");
  if (!list) return;
  for (const char* p = list; *p;) {
    char* end;
    uint32_t a = (uint32_t)std::strtoul(p, &end, 0);
    if (end == p) break;
    uint32_t v = ppc_ld32(gm::base(), a);
    bool ptr = v >= 0x10000u && v < 0xFFFF0000u && !IsBadReadPtr(PPC_HOST(gm::base(), v), 4);
    std::fprintf(stderr, "[peek] [0x%08X] = 0x%08X", a, v);
    if (ptr) std::fprintf(stderr, "  [[..]] = 0x%08X", ppc_ld32(gm::base(), v));
    std::fputc('\n', stderr);
    p = *end ? end + 1 : end;
  }
}

// GOW_MEMDUMP=0xBASE:0xSIZE[,...]: no watchdog grava a memória convidada em frames/mem_BASE.bin (páginas não
// confirmadas ficam a zero), para análise offline (ex.: tabela de FNames do UE3).
void memdump() {
  const char* list = std::getenv("GOW_MEMDUMP");
  if (!list) return;
  for (const char* p = list; *p;) {
    char* end;
    uint32_t base = (uint32_t)std::strtoul(p, &end, 0);
    if (*end != ':') break;
    uint32_t size = (uint32_t)std::strtoul(end + 1, &end, 0);
    char path[64];
    std::snprintf(path, sizeof(path), "frames/mem_%08X.bin", base);
    CreateDirectoryA("frames", nullptr);
    if (FILE* f = std::fopen(path, "wb")) {
      static uint8_t zero[4096];
      for (uint32_t o = 0; o < size; o += 4096) {
        const uint8_t* src = PPC_HOST(gm::base(), base + o);
        MEMORY_BASIC_INFORMATION mbi{};
        bool ok = VirtualQuery(src, &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_NOACCESS);
        std::fwrite(ok ? src : zero, 1, 4096, f);
      }
      std::fclose(f);
      std::fprintf(stderr, "[memdump] %s (%u bytes)\n", path, size);
    }
    p = *end ? end + 1 : end;
  }
}

DWORD WINAPI guest_main(LPVOID) {
  static PPCContext ctx;
  kernel::GuestThread* t = kernel::create_guest_thread(ctx, kMainStackSize);
  if (!t) {
    std::fprintf(stderr, "[gow] falha ao criar a thread principal do convidado\n");
    return 1;
  }
  std::fprintf(stderr, "[gow] a executar sub_%08X (r1=0x%08llX r13=0x%08llX)\n", g_ppc_entry_point,
               (unsigned long long)ctx.r[1], (unsigned long long)ctx.r[13]);
  uint32_t code = kernel::run_guest(t, ctx, g_ppc_entry_point);
  std::fprintf(stderr, "[gow] thread principal terminou: 0x%08X\n", code);
  kernel::finish_thread(t, code);
  return 0;
}

}  // namespace

// Resolução do temporizador do Windows: por omissão 15,6 ms, o que fazia o VBlank de 16,67 ms acordar a 15,6/31,2 ms
// (VBlanks perdidos → flips atrasados um VBlank → cutscenes a 20 fps) e transformava as esperas curtas (processador
// de comandos inativo 200 µs, ritmo do áudio 1 ms) em 15 ms. Pede 0,5 ms (ntdll) ou 1 ms (winmm).
void raise_timer_resolution() {
  using NtSetTimerResolutionFn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  ULONG cur = 0;
  if (HMODULE nt = GetModuleHandleA("ntdll.dll"))
    if (auto f = (NtSetTimerResolutionFn)GetProcAddress(nt, "NtSetTimerResolution"); f && f(5000, TRUE, &cur) >= 0) {
      std::fprintf(stderr, "[gow] resolução do temporizador: %.2f ms\n", cur / 10000.0);
      return;
    }
  if (HMODULE mm = LoadLibraryA("winmm.dll"))
    if (auto f = (UINT(WINAPI*)(UINT))GetProcAddress(mm, "timeBeginPeriod")) f(1);
}

// Sem GOW_VERBOSE=1 (ou um GOW_TRACE_*, GOW_WATCHDOG, GOW_HEARTBEAT), o log do jogo não vai a lado nenhum: nem consola nem
// ficheiro. Um relatório de crash abre gow_crash.log (diag::crash_report).
void setup_logging() {
  bool verbose = false;
  for (char** e = _environ; e && *e; e++)
    if (!std::strncmp(*e, "GOW_VERBOSE=1", 13) || !std::strncmp(*e, "GOW_TRACE", 9) || !std::strncmp(*e, "GOW_WATCHDOG=", 13) ||
        !std::strncmp(*e, "GOW_HEARTBEAT=", 14) || !std::strncmp(*e, "GOW_PROFILE_GPU=", 16))
      verbose = true;
  if (!verbose) {
    (void)std::freopen("NUL", "w", stderr);
    diag::set_silent();
    return;
  }
  // Aplicação GUI: sem stderr herdado (redirecionado por quem a lançou), usa a consola de onde foi aberta, se houver.
  HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
  if ((!err || err == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) (void)std::freopen("CONOUT$", "w", stderr);
}

int main(int argc, char** argv) {
  setup_logging();
  raise_timer_resolution();
  game_source::init_working_dir();
  config::get();  // cria o gow_config.ini se não existir
  std::vector<uint8_t> image;
  if (!game_source::prepare(&image)) return 1;
  net::start_from_config();
  if (!gm::init()) {
    std::fprintf(stderr, "[gow] falha ao reservar 4 GiB para o convidado\n");
    return 1;
  }
  bool imports_ok = xbox::init_imports();
  if (!loader::load_image(image) || !loader::bind_imports()) return 1;
  if (!imports_ok) std::fprintf(stderr, "[gow] aviso: tabela de imports incompleta\n");
  std::atexit(xbox::print_import_stats);
  apu::xma_init();
  apu::output_init();
  if (std::getenv("GOW_SAMPLE")) kernel::start_sampler();
  // GOW_HEARTBEAT=N: a cada N segundos, estado da GPU + GOW_PEEK (para ver se algo progride).
  if (const char* hb = std::getenv("GOW_HEARTBEAT")) {
    int secs = std::max(1, std::atoi(hb));
    std::thread([secs] {
      for (int t = secs;; t += secs) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        std::fprintf(stderr, "[heartbeat] %d s\n", t);
        gpu::dump_state();
        peek();
      }
    }).detach();
  }
  // GOW_WATCHDOG=N: após N segundos imprime o estado de todas as threads convidadas e encerra.
  if (const char* w = std::getenv("GOW_WATCHDOG")) {
    int secs = std::atoi(w);
    std::thread([secs] {
      std::this_thread::sleep_for(std::chrono::seconds(secs));
      std::fprintf(stderr, "[watchdog] %d s\n", secs);
      diag::crash_report();
      kernel::print_samples();
      gpu::dump_state();
      peek();
      memdump();
      std::_Exit(3);
    }).detach();
  }

  HANDLE t = CreateThread(nullptr, kHostStackSize, guest_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  if (!t) return 1;
  WaitForSingleObject(t, INFINITE);
  DWORD code = 1;
  GetExitCodeThread(t, &code);
  std::fprintf(stderr, "[gow] fim (código %lu, %llu páginas confirmadas sob demanda)\n", code,
               (unsigned long long)gm::demand_commits());
  return (int)code;
}
