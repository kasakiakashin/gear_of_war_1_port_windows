// d3d12_renderer.cpp — backend D3D12 do processador de comandos.
//
// d3d12.dll / dxgi.dll / d3dcompiler_47.dll são carregadas em runtime (nada a ligar). Modelo:
//  * EDRAM: um render target do host por (base em tiles, formato, pitch); MSAA ignorado (1×).
//  * Desenho: shaders Xenos traduzidos para HLSL (xenos_shader.cpp) e compilados com D3DCompile; vértices lidos no
//    VS a partir de buffers brutos (cópia da memória convidada por desenho) e de uma lista de índices já expandida
//    (strips/fans/quads/rect lists → listas).
//  * Resolve (RB_MODECONTROL = cópia): blit do render target para uma textura do host associada ao endereço de
//    destino, + limpezas de cor/profundidade. Texturas lidas depois nesse endereço usam a cópia do host.
//  * Swap: blit do frontbuffer (cópia de resolve, ou memória convidada descodificada) para o backbuffer.
//  * Um command list por frame; no swap executa, espera pela GPU e recomeça (simples e seguro).
#define WIN32_LEAN_AND_MEAN
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "net/tunnel.h"
#include "gpu/renderer.h"
#include "gpu/texture.h"
#include "gpu/window.h"
#include "gpu/xenos_shader.h"
#include "guest_memory.h"
#include "kernel/vfs.h"

namespace gpu::render {
namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720, kBackBuffers = 2;
// Dois frames em voo: cada anel (upload, SRV, samplers) está dividido em duas metades, uma por slot.
constexpr int kSlots = 2;
constexpr size_t kUploadSize = 256ull << 20;  // por slot
constexpr uint32_t kSrvHeapSize = 32768, kSamplerHeapSize = 1024;  // por slot
constexpr uint32_t kRtvHeapSize = 512, kDsvHeapSize = 128;

// Registradores usados
enum : uint32_t {
  kRbSurfaceInfo = 0x2000, kRbColorInfo = 0x2001, kRbDepthInfo = 0x2002, kRbColor1Info = 0x2003,
  kPaScWindowOffset = 0x2080, kPaScWindowScissorTl = 0x2081, kPaScWindowScissorBr = 0x2082,
  kVgtIndxOffset = 0x2102, kVgtMultiPrimIbResetIndx = 0x2103, kRbColorMask = 0x2104, kRbBlendRed = 0x2105,
  kRbStencilRefMask = 0x210D, kRbAlphaRef = 0x210E, kPaClVportXscale = 0x210F, kSqProgramCntl = 0x2180,
  kSqContextMisc = 0x2181, kRbDepthControl = 0x2200, kRbBlendControl0 = 0x2201, kRbColorControl = 0x2202,
  kPaSuPolyOffsetFrontScale = 0x2380, kPaSuScModeCntl = 0x2205, kPaClVteCntl = 0x2206, kRbModeControl = 0x2208, kRbBlendControl1 = 0x2209,
  kSqVsConst = 0x2307, kSqPsConst = 0x2308, kRbCopyControl = 0x2318, kRbCopyDestBase = 0x2319,
  kRbCopyDestPitch = 0x231A, kRbCopyDestInfo = 0x231B, kRbDepthClear = 0x231D, kRbColorClear = 0x231E,
  kConstAlu = 0x4000, kConstFetch = 0x4800, kConstBool = 0x4900, kConstLoop = 0x4908,
};

uint32_t reg(uint32_t i) { return register_value(i); }
float regf(uint32_t i) {
  uint32_t v = reg(i);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                        LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

struct Texture {
  uint32_t stable = 0;  // frames seguidos com o mesmo hash completo
  uint64_t quick = 0;   // hash do início da memória convidada
  ID3D12Resource* res = nullptr;
  D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
  uint32_t width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{}, dsv{};
  bool has_rtv = false, depth = false;
  uint64_t hash = 0, raw_hash = 0;  // hash do conteúdo descodificado / da memória convidada
  uint64_t frame = ~0ull;
  uint32_t guest_pitch = 0, guest_bpp = 0;  // destinos de resolve: layout da superfície no convidado
  uint32_t mips = 1;
  bool cube = false;  // 6 fatias vistas como TextureCube
  bool depth_copy = false;  // cópia R32_FLOAT de um resolve de profundidade (fetch k_24_8 / k_24_8_FLOAT)
  uint64_t serial = 0;                      // destinos de resolve: ordem da última escrita
};

struct RtKey {
  uint32_t base, format, pitch;
  bool depth;
  bool operator<(const RtKey& o) const {
    return std::tie(base, format, pitch, depth) < std::tie(o.base, o.format, o.pitch, o.depth);
  }
};

struct Compiled {
  TranslatedShader info;
  ID3DBlob* blob = nullptr;
  std::atomic<bool> pending{false};  // FXC a correr numa thread de fundo (o desenho é saltado até acabar)
};

struct Renderer {
  bool active = false;
  HMODULE d3d12 = nullptr, dxgi = nullptr, compiler = nullptr;
  PFN_D3D12_CREATE_DEVICE create_device = nullptr;
  PFN_D3D12_SERIALIZE_ROOT_SIGNATURE serialize_rs = nullptr;
  PFN_CreateDXGIFactory2 create_factory = nullptr;
  PFN_D3DCompile compile = nullptr;
  HRESULT(WINAPI* create_blob)(SIZE_T, ID3DBlob**) = nullptr;
  IDXGIFactory4* factory = nullptr;
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  IDXGISwapChain3* swapchain = nullptr;
  ID3D12Resource* backbuffers[kBackBuffers] = {};
  uint32_t bb_w = 0, bb_h = 0;  // tamanho do swapchain (segue a área cliente da janela)
  // ResolutionScale: RTs, profundidade e cópias de resolve têm tamanho lógico × scale; o convidado e os shaders veem só
  // coordenadas lógicas (1280×720). Viewport, tesoura, retângulos de resolve/clear convertem-se aqui.
  float scale = 1.0f;
  ID3D12CommandAllocator* allocator = nullptr;  // o do slot atual
  ID3D12CommandAllocator* allocators[kSlots] = {};
  uint64_t slot_fence[kSlots] = {};
  std::vector<ID3D12Resource*> slot_garbage[kSlots];
  int slot = 0;
  ID3D12GraphicsCommandList* list = nullptr;
  ID3D12Fence* fence = nullptr;
  HANDLE fence_event = nullptr;
  uint64_t fence_value = 0;
  ID3D12Resource* upload = nullptr;
  uint8_t* upload_ptr = nullptr;
  size_t upload_offset = 0;
  // Buffers de vértices já copiados neste submit, por (endereço físico, tamanho): os modelos repetem-se muito
  // entre desenhos. GOW_NO_VB_CACHE=1 copia sempre.
  struct VbCopy {
    D3D12_GPU_VIRTUAL_ADDRESS va;
    uint64_t hash;
  };
  std::unordered_map<uint64_t, VbCopy> vb_cache;
  uint64_t vb_stale = 0;
  struct IndexCopy {
    D3D12_GPU_VIRTUAL_ADDRESS va;
    uint32_t count;
    D3D12_PRIMITIVE_TOPOLOGY topo;
    D3D12_PRIMITIVE_TOPOLOGY_TYPE type;
  };
  std::unordered_map<uint64_t, IndexCopy> idx_cache;
  ID3D12RootSignature* bound_root = nullptr;
  ID3D12PipelineState* bound_pso = nullptr;
  std::unordered_map<uint64_t, uint32_t> srv_tables, smp_tables;  // tabelas de descritores por conteúdo, por submit
  std::chrono::steady_clock::time_point last_present;
  float stat_worst_ms = 0;
  uint32_t stat_slow_frames = 0;  // acima de 50 ms
  uint32_t stat_submits = 0, stat_creates = 0, stat_early[3] = {};  // submits a meio do frame: upload, SRV, samplers
  int shader_wait_ms = 0;  // tempo já gasto neste frame à espera do FXC
  ID3D12DescriptorHeap *srv_heap = nullptr, *sampler_heap = nullptr, *rtv_heap = nullptr, *dsv_heap = nullptr;
  uint32_t srv_inc = 0, sampler_inc = 0, rtv_inc = 0, dsv_inc = 0;
  uint32_t srv_next = 0, sampler_next = 0, rtv_next = 0, dsv_next = 0;
  ID3D12RootSignature *root = nullptr, *blit_root = nullptr;
  ID3DBlob *blit_vs = nullptr, *blit_ps = nullptr;
  std::map<uint32_t, ID3D12PipelineState*> blit_pso;  // por formato DXGI
  std::unordered_map<uint64_t, Compiled> shaders;
  std::unordered_map<uint64_t, ID3D12PipelineState*> psos;
  std::map<RtKey, Texture> rts;
  // Cópias de resolve por (endereço físico de destino, formato DXGI); a mais recente (serial) é a que vale.
  std::map<std::pair<uint32_t, uint32_t>, Texture> resolved;
  uint64_t resolve_serial = 0;
  std::vector<ID3D12Resource*> garbage;  // libertados só depois do próximo submit (podem estar em uso na GPU)
  // GOW_RESOLVE_TO_MEMORY=1: resolves copiados de volta para a memória convidada (tiled + endian) após o submit.
  bool resolve_to_memory = false;
  struct Writeback {
    ID3D12Resource* readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    uint32_t w, h, dest, pitch, bpp, endian;
  };
  std::vector<Writeback> writebacks;
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Texture> textures;
  Texture dummy, dummy_cube;
  Texture fsr_tmp;  // saída do EASU, entrada do RCAS
  std::map<uint32_t, Texture> cubes;  // cube maps por endereço base
  std::vector<uint32_t> vs_ucode, ps_ucode;
  uint64_t vs_hash = 0, ps_hash = 0;
  uint64_t frame = 0;
  std::set<uint64_t> dump_frames;
  bool dump_shaders = false, dump_resolves = false;
  bool trace = false;  // GOW_TRACE_RENDER=N: desenhos, resolves, texturas e swap do frame N
  uint64_t trace_frame = 0;
  uint32_t trace_lines = 0;
  // Estatísticas por frame
  uint32_t draws = 0, skipped = 0, resolves = 0, swaps = 0;
  std::set<std::string> warned;
};

Renderer g;
// Fase atual do renderer (diagnóstico do watchdog: onde a thread da GPU está presa).
std::atomic<const char*> g_phase{"ocioso"};
struct Phase {
  const char* prev;
  explicit Phase(const char* p) : prev(g_phase.exchange(p)) {}
  ~Phase() { g_phase = prev; }
};

bool traced_frame() { return g.frame == g.trace_frame; }
uint32_t scaled(float v) { return (uint32_t)std::lround(v * g.scale); }

// GOW_PROFILE_GPU=1: tempo do processador de comandos por fase, médio por frame, no relatório de 120 frames.
enum ProfCat {
  kProfDraw, kProfTexture, kProfShader, kProfPso, kProfVertex, kProfResolve, kProfSubmit, kProfIndex, kProfConst,
  kProfRecord, kProfDesc, kProfSetup, kProfCount
};
const char* const kProfNames[kProfCount] = {"desenho", "texturas", "shaders", "PSOs",       "vértices",
                                            "resolves", "submit",   "índices", "constantes", "gravação", "descritores", "estado"};
const bool g_profile = std::getenv("GOW_PROFILE_GPU") != nullptr;
double g_prof[kProfCount] = {};
LARGE_INTEGER g_prof_last_swap{};
double g_prof_frame_ms = 0;
double prof_ms(LONGLONG ticks) {
  static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
  return ticks * 1000.0 / freq.QuadPart;
}
struct Prof {
  ProfCat cat;
  LARGE_INTEGER t0;
  bool running = g_profile;
  explicit Prof(ProfCat c) : cat(c) { if (running) QueryPerformanceCounter(&t0); }
  void stop() {
    if (!running) return;
    running = false;
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    g_prof[cat] += prof_ms(t1.QuadPart - t0.QuadPart);
  }
  ~Prof() { stop(); }
};

void warn_once(const std::string& key, const char* fmt, ...) {
  if (!g.warned.insert(key).second) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);
}

// ---- Utilitários D3D12 ----
D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE t) {
  D3D12_HEAP_PROPERTIES p{};
  p.Type = t;
  return p;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t size) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}

D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle(ID3D12DescriptorHeap* h, uint32_t inc, uint32_t i) {
  D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
  c.ptr += (size_t)inc * i;
  return c;
}
D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle(ID3D12DescriptorHeap* h, uint32_t inc, uint32_t i) {
  D3D12_GPU_DESCRIPTOR_HANDLE c = h->GetGPUDescriptorHandleForHeapStart();
  c.ptr += (uint64_t)inc * i;
  return c;
}

void transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES to) {
  if (cur == to) return;
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = cur;
  b.Transition.StateAfter = to;
  g.list->ResourceBarrier(1, &b);
  cur = to;
}
void transition(Texture& t, D3D12_RESOURCE_STATES to) { transition(t.res, t.state, to); }

void wait_fence(uint64_t value) {
  if (g.fence->GetCompletedValue() < value) {
    g.fence->SetEventOnCompletion(value, g.fence_event);
    WaitForSingleObject(g.fence_event, INFINITE);
  }
}

void bind_heaps() {
  ID3D12DescriptorHeap* heaps[] = {g.srv_heap, g.sampler_heap};
  g.list->SetDescriptorHeaps(2, heaps);
}

// Resolves → memória convidada: cópia para readback agora, escrita (tiled 2D + troca de endian) depois do submit.
void flush_writebacks() {
  uint8_t* m = gm::base();
  for (auto& wb : g.writebacks) {
    void* p = nullptr;
    if (SUCCEEDED(wb.readback->Map(0, nullptr, &p))) {
      uint32_t bpp_log2 = wb.bpp == 1 ? 0 : wb.bpp == 2 ? 1 : wb.bpp == 4 ? 2 : wb.bpp == 8 ? 3 : 4;
      uint32_t pitch_aligned = (wb.pitch + 31) & ~31u;
      uint8_t* dst = m + 0xA0000000u;
      for (uint32_t y = 0; y < wb.h; y++) {
        const uint8_t* row = static_cast<const uint8_t*>(p) + (size_t)y * wb.fp.Footprint.RowPitch;
        for (uint32_t x = 0; x < wb.w; x++) {
          uint32_t off = wb.dest + tiled_offset_2d(x, y, pitch_aligned, bpp_log2);
          if (off + wb.bpp > 0x20000000u) continue;
          for (uint32_t k = 0; k < wb.bpp; k += 4) {
            uint32_t v;
            std::memcpy(&v, row + (size_t)x * wb.bpp + k, 4);
            switch (wb.endian) {
              case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
              case 2: v = __builtin_bswap32(v); break;
              case 3: v = (v << 16) | (v >> 16); break;
            }
            std::memcpy(dst + off + k, &v, 4);
          }
        }
      }
      wb.readback->Unmap(0, nullptr);
    }
    wb.readback->Release();
  }
  g.writebacks.clear();
}

void queue_writeback(Texture& t, uint32_t row, uint32_t w, uint32_t h, uint32_t dest, uint32_t pitch, uint32_t bpp,
                     uint32_t endian) {
  if (bpp < 4) return;  // trocas de endian em palavras de 32 bits
  D3D12_RESOURCE_DESC rd = t.res->GetDesc();
  rd.Width = w;
  rd.Height = h;
  rd.MipLevels = 1;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  g.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
  auto hp = heap(D3D12_HEAP_TYPE_READBACK);
  auto bd = buffer_desc(total);
  ID3D12Resource* rb = nullptr;
  if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb))))
    return;
  D3D12_RESOURCE_STATES before = t.state;
  transition(t, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = t.res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_BOX box{0, row, 0, w, row + h, 1};
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
  transition(t, before);
  g.writebacks.push_back({rb, fp, w, h, dest, pitch, bpp, endian});
}

// Executa o que foi gravado e reabre o command list no outro slot. Só se espera pela GPU até ao fim do submit
// anterior desse slot (o CPU grava o frame seguinte enquanto a GPU desenha este); wait_all espera por tudo
// (leituras de volta: capturas, resolves para a memória convidada).
void submit(bool wait_all = false) {
  Phase ph("submit (à espera da GPU)");
  Prof pf(kProfSubmit);
  g.list->Close();
  ID3D12CommandList* lists[] = {g.list};
  g.queue->ExecuteCommandLists(1, lists);
  g.queue->Signal(g.fence, ++g.fence_value);
  g.slot_fence[g.slot] = g.fence_value;
  g.slot_garbage[g.slot].swap(g.garbage);
  g.garbage.clear();
  ULONGLONG t0 = GetTickCount64();
  if (wait_all || !g.writebacks.empty()) wait_fence(g.fence_value);
  g.slot = (g.slot + 1) % kSlots;
  wait_fence(g.slot_fence[g.slot]);
  ULONGLONG dt = GetTickCount64() - t0;
  HRESULT removed = g.device->GetDeviceRemovedReason();
  if (dt > 500 || FAILED(removed))
    std::fprintf(stderr, "[d3d12] submit: GPU demorou %llu ms (device removed: 0x%08lX)\n", (unsigned long long)dt,
                 removed);
  flush_writebacks();
  for (ID3D12Resource* r : g.slot_garbage[g.slot]) r->Release();
  g.slot_garbage[g.slot].clear();
  g.allocator = g.allocators[g.slot];
  g.allocator->Reset();
  g.list->Reset(g.allocator, nullptr);
  g.upload_offset = 0;
  g.vb_cache.clear();
  g.idx_cache.clear();
  g.bound_root = nullptr;
  g.bound_pso = nullptr;
  g.srv_tables.clear();
  g.smp_tables.clear();
  g.stat_submits++;
  g.srv_next = 0;
  g.sampler_next = 0;
  bind_heaps();
}

// Reserva no anel de upload (alinhado); submete se não couber.
uint8_t* upload_alloc(size_t n, size_t align, D3D12_GPU_VIRTUAL_ADDRESS* va, size_t* offset = nullptr) {
  size_t off = (g.upload_offset + align - 1) & ~(align - 1);
  if (off + n > kUploadSize) {
    submit();
    off = 0;
  }
  if (n > kUploadSize) return nullptr;
  g.upload_offset = off + n;
  size_t abs = (size_t)g.slot * kUploadSize + off;
  if (va) *va = g.upload->GetGPUVirtualAddress() + abs;
  if (offset) *offset = abs;
  return g.upload_ptr + abs;
}

D3D12_GPU_VIRTUAL_ADDRESS upload_cb(const void* data, size_t n) {
  D3D12_GPU_VIRTUAL_ADDRESS va = 0;
  uint8_t* p = upload_alloc((n + 255) & ~size_t(255), 256, &va);
  if (p) std::memcpy(p, data, n);
  return va;
}

// Constantes ALU: reserva as 256 (o cbuffer declara-as todas), mas só copia as que o shader lê. Copiar 8 KiB por desenho para
// memória de upload custava vários ms por frame e enchia o anel.
D3D12_GPU_VIRTUAL_ADDRESS upload_alu(const uint32_t* src, const uint64_t used[4]) {
  D3D12_GPU_VIRTUAL_ADDRESS va = 0;
  uint8_t* p = upload_alloc(256 * 16, 256, &va);
  if (!p) return va;
  for (uint32_t i = 0; i < 256;) {
    if (!((used[i >> 6] >> (i & 63)) & 1)) {
      i++;
      continue;
    }
    uint32_t j = i + 1;
    while (j < 256 && ((used[j >> 6] >> (j & 63)) & 1)) j++;
    std::memcpy(p + i * 16, src + i * 4, (j - i) * 16);
    i = j;
  }
  return va;
}

uint32_t srv_alloc(uint32_t n) {
  if (g.srv_next + n > kSrvHeapSize) submit();
  uint32_t i = g.srv_next;
  g.srv_next += n;
  return (uint32_t)g.slot * kSrvHeapSize + i;
}
uint32_t sampler_alloc(uint32_t n) {
  if (g.sampler_next + n > kSamplerHeapSize) submit();
  uint32_t i = g.sampler_next;
  g.sampler_next += n;
  return (uint32_t)g.slot * kSamplerHeapSize + i;
}

// O FXC tem erros internos ("internal error: l-value expected") que dependem das passagens de otimização:
// tenta o nível 1 e, se falhar, sem otimização e com o nível 3.
ID3DBlob* compile_hlsl(const std::string& src, const char* target, const char* name, std::string* err) {
  static const UINT kFlags[] = {D3DCOMPILE_OPTIMIZATION_LEVEL1, D3DCOMPILE_SKIP_OPTIMIZATION,
                                D3DCOMPILE_OPTIMIZATION_LEVEL3};
  for (UINT flags : kFlags) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = g.compile(src.data(), src.size(), name, nullptr, nullptr, "main", target, flags, 0, &code, &errors);
    if (errors) {
      if (err && FAILED(hr)) err->assign((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
      errors->Release();
    }
    if (SUCCEEDED(hr)) {
      if (flags != kFlags[0]) std::fprintf(stderr, "[d3d12] %s compilou com flags 0x%X\n", name, flags);
      return code;
    }
    if (code) code->Release();
  }
  return nullptr;
}

// ---- Texturas do host ----
bool create_texture(Texture& t, uint32_t w, uint32_t h, DXGI_FORMAT f, bool render_target, bool depth,
                    uint32_t mips = 1, uint32_t array = 1) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = (UINT16)array;
  d.MipLevels = (UINT16)mips;
  d.Format = depth ? DXGI_FORMAT_R24G8_TYPELESS : f;
  d.SampleDesc.Count = 1;
  d.Flags = depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                  : render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
  D3D12_CLEAR_VALUE cv{};
  cv.Format = depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : f;
  if (depth) cv.DepthStencil.Depth = 1.0f;
  auto hp = heap(D3D12_HEAP_TYPE_DEFAULT);
  t.state = depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                  : render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST;
  g.stat_creates++;
  HRESULT hr = g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, t.state,
                                                 (render_target || depth) ? &cv : nullptr, IID_PPV_ARGS(&t.res));
  if (FAILED(hr)) {
    warn_once("tex" + std::to_string(f), "[d3d12] CreateCommittedResource %ux%u formato %u falhou (0x%08lX)\n", w, h,
              (unsigned)f, hr);
    t.res = nullptr;
    return false;
  }
  t.width = w;
  t.height = h;
  t.format = f;
  t.depth = depth;
  t.mips = mips;
  if (depth) {
    t.dsv = cpu_handle(g.dsv_heap, g.dsv_inc, g.dsv_next++ % kDsvHeapSize);
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g.device->CreateDepthStencilView(t.res, &dv, t.dsv);
  } else if (render_target) {
    t.rtv = cpu_handle(g.rtv_heap, g.rtv_inc, kBackBuffers + g.rtv_next++ % (kRtvHeapSize - kBackBuffers));
    g.device->CreateRenderTargetView(t.res, nullptr, t.rtv);
    t.has_rtv = true;
  }
  return true;
}

// swizzle: campo da constante de fetch (3 bits por componente: 0..3 = X..W, 4 = 0, 5 = 1), a mesma codificação do
// Shader4ComponentMapping do D3D12. Aplica-se por cima do mapeamento base do formato.
constexpr uint32_t kSwizzleIdentity = 0x688, kSwizzleSwapRB = 0x60A;  // XYZW / ZYXW
void write_srv(const Texture& t, uint32_t slot, uint32_t swizzle = kSwizzleIdentity) {
  D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
  sd.Format = t.depth ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS : t.format;
  sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  // O Xenos devolve o canal único de k_8/k_16/k_32_FLOAT/DXT5A em todos os componentes (RRRR) e os dois de
  // k_8_8/DXN como RGGG; o D3D devolveria (R, 0, 0, 1). As planas YUV do Bink dependem disto.
  switch (sd.Format) {
    case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_BC4_UNORM:
      sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
      break;
    case DXGI_FORMAT_R8G8_UNORM: case DXGI_FORMAT_R16G16_UNORM: case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT: case DXGI_FORMAT_BC5_UNORM:
      sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 1, 1);
      break;
    default:
      sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  }
  if (swizzle != kSwizzleIdentity) {
    uint32_t base = sd.Shader4ComponentMapping, m = D3D12_SHADER_COMPONENT_MAPPING_ALWAYS_SET_BIT_AVOIDING_ZEROMEM_MISTAKES;
    for (uint32_t c = 0; c < 4; c++) {
      uint32_t src = (swizzle >> (3 * c)) & 7;
      uint32_t v = src < 4 ? (base >> (3 * src)) & 7 : src == 4 ? 4u : src == 5 ? 5u : (base >> (3 * c)) & 7;
      m |= v << (3 * c);
    }
    sd.Shader4ComponentMapping = m;
  }
  if (t.cube) {
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    sd.TextureCube.MipLevels = t.mips;
  } else {
    sd.Texture2D.MipLevels = t.mips;
  }
  g.device->CreateShaderResourceView(t.res, &sd, cpu_handle(g.srv_heap, g.srv_inc, slot));
}

void upload_texture(Texture& t, const DecodedTexture& d, uint32_t level = 0) {
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  D3D12_RESOURCE_DESC rd = t.res->GetDesc();
  UINT rows = 0;
  UINT64 row_size = 0, total = 0;
  g.device->GetCopyableFootprints(&rd, level, 1, 0, &fp, &rows, &row_size, &total);
  size_t off = 0;
  uint8_t* p = upload_alloc((size_t)total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, nullptr, &off);
  if (!p) return;
  for (UINT y = 0; y < rows && y < d.rows; y++)
    std::memcpy(p + (size_t)y * fp.Footprint.RowPitch, &d.data[(size_t)y * d.row_pitch],
                std::min<size_t>(d.row_pitch, (size_t)row_size));
  fp.Offset = off;
  transition(t, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = t.res;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = level;
  src.pResource = g.upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = fp;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

// Cópia de resolve mais recente para um endereço físico (qualquer formato).
// want_depth (fetch k_24_8 / k_24_8_FLOAT): prefere a cópia com a profundidade em x. Os outros formatos nunca a veem.
Texture* resolved_at(uint32_t phys, bool want_depth = false) {
  Texture* best = nullptr;
  for (auto it = g.resolved.lower_bound({phys, 0}); it != g.resolved.end() && it->first.first == phys; ++it) {
    Texture& t = it->second;
    if (!t.res) continue;
    if (want_depth && t.depth_copy) return &t;
    if (!t.depth_copy && (!best || t.serial > best->serial)) best = &t;
  }
  return best;
}

// Cube map (dimensão 3): 6 faces 2D tiled por nível (decode_cube_mips), num array de 6 × mips.
Texture* cube_texture_for(const TextureFetch& f) {
  Prof pf(kProfTexture);
  Texture& t = g.cubes[f.base()];
  if (t.res && t.frame == g.frame) return &t;
  FormatInfo fi = texture_format_info(f.format());
  if (!fi.dxgi) return &g.dummy_cube;
  // Deteção de alterações: nível 0 das 6 faces (cube maps do jogo são estáticos ou mudam por inteiro).
  uint32_t pb = (std::max(f.pitch_texels(), f.width()) + fi.block_w - 1) / fi.block_w;
  uint32_t bh = (f.height() + fi.block_h - 1) / fi.block_h;
  uint32_t face = f.tiled() ? ((pb + 31) & ~31u) * ((bh + 31) & ~31u) * fi.bytes_per_block : pb * bh * fi.bytes_per_block;
  face = (face + 0xFFF) & ~0xFFFu;
  uint32_t phys = f.base() & 0x1FFFFFFFu;
  if ((uint64_t)phys + 6ull * face > 0x20000000u) return &g.dummy_cube;
  uint64_t raw = hash_bytes(gm::base() + 0xA0000000u + phys, (size_t)face * 6) ^ f.dword[1] ^ f.dword[2] ^ f.dword[5];
  if (t.res && t.raw_hash == raw) {
    t.frame = g.frame;
    return &t;
  }
  std::vector<DecodedTexture> faces;
  uint32_t mips = 1;
  if (!decode_cube_mips(gm::base(), f, faces, mips)) return &g.dummy_cube;
  if (t.res && t.mips != mips) g.garbage.push_back(t.res), t.res = nullptr;
  if (!t.res) {
    if (!create_texture(t, faces[0].width, faces[0].height, (DXGI_FORMAT)faces[0].dxgi, false, false, mips, 6))
      return &g.dummy_cube;
    t.cube = true;
  }
  for (uint32_t l = 0; l < mips; l++)
    for (uint32_t i = 0; i < 6; i++) upload_texture(t, faces[l * 6 + i], l + i * mips);  // subrecurso = mip + face × mips
  t.raw_hash = raw;
  t.frame = g.frame;
  return &t;
}

// Textura de uma constante de fetch: cópia de resolve (mesmo endereço) ou memória convidada descodificada.
Texture* texture_for(const TextureFetch& f) {
  Prof pf(kProfTexture);
  if (Texture* r = resolved_at(f.base(), f.format() == 22 || f.format() == 23)) {
    if (g.trace && traced_frame())
      std::fprintf(stderr, "[render] textura-resolve 0x%08X fmt fetch %u %ux%u swizzle 0x%03X (cópia DXGI %u)%c", f.base(),
                   f.format(), f.width(), f.height(), (f.dword[3] >> 1) & 0xFFF, (unsigned)r->format, 10);
    return r;
  }
  auto key = std::make_tuple(f.base(), f.dword[1] & 0xFFF, f.dword[2], f.dword[0] & 0xFFC00000u);
  Texture& t = g.textures[key];
  if (t.res && t.frame == g.frame) return &t;
  // Memória convidada igual à da última descodificação: nada a fazer (descodificar todas as texturas a cada frame
  // custava ~26 ms/frame no pátio da prisão). Uma textura sem mudanças há 30 frames só volta ao hash completo 1 frame em 16;
  // nos outros basta a amostra do início, que apanha outra textura posta no mesmo endereço.
  const uint64_t quick = hash_bytes(gm::base() + 0xA0000000u + (gpu_physical(f.base()) & 0x1FFFFFFFu), 256);
  if (t.res && t.stable >= 30 && t.quick == quick && (g.frame + (uintptr_t)&t / 64) % 16 && t.state != D3D12_RESOURCE_STATE_COPY_DEST &&
      !(g.trace && traced_frame())) {
    t.frame = g.frame;
    return &t;
  }
  uint64_t raw = texture_raw_hash(gm::base(), f);
  if (t.res && t.raw_hash == raw && t.state != D3D12_RESOURCE_STATE_COPY_DEST && !(g.trace && traced_frame())) {
    t.frame = g.frame;
    t.stable++;
    t.quick = quick;
    return &t;
  }
  t.stable = 0;
  static const uint32_t trace_tex_max = std::getenv("GOW_TRACE_MAX") ? (uint32_t)std::atoi(std::getenv("GOW_TRACE_MAX")) : 400;
  if (g.trace && traced_frame() && g.trace_lines < trace_tex_max) {
    g.trace_lines++;
    std::fprintf(stderr,
                 "[render] textura 0x%08X fmt %u %ux%u pitch %u tiled %d endian %u mips %u..%u packed %d mip 0x%08X "
                 "dim %u (%08X %08X %08X %08X %08X %08X)\n",
                 f.base(), f.format(), f.width(), f.height(), f.pitch_texels(), f.tiled(), f.endian(),
                 (f.dword[4] >> 2) & 15, (f.dword[4] >> 6) & 15, (f.dword[5] >> 11) & 1,
                 gpu_physical(f.dword[5] & 0xFFFFF000u), f.dimension(), f.dword[0], f.dword[1], f.dword[2], f.dword[3],
                 f.dword[4], f.dword[5]);
  }
  std::vector<DecodedTexture> levels;
  Phase ph("descodificação de textura");
  if (!decode_texture_mips(gm::base(), f, levels)) {
    warn_once("fmt" + std::to_string(f.format()), "[d3d12] formato de textura %u não suportado\n", f.format());
    return &g.dummy;
  }
  uint64_t h = 0;
  for (const DecodedTexture& lv : levels) h = h * 31 + hash_bytes(lv.data.data(), lv.data.size());
  const DecodedTexture& d = levels[0];
  if (t.res && t.mips != levels.size()) g.garbage.push_back(t.res), t.res = nullptr, t.hash = 0;
  if (!t.res && !create_texture(t, d.width, d.height, (DXGI_FORMAT)d.dxgi, false, false, (uint32_t)levels.size()))
    return &g.dummy;
  t.frame = g.frame;
  t.raw_hash = raw;
  t.quick = quick;
  if (t.hash != h || t.state == D3D12_RESOURCE_STATE_COPY_DEST) {
    t.hash = h;
    for (uint32_t l = 0; l < levels.size(); l++) upload_texture(t, levels[l], l);
  }
  return &t;
}

// ---- Render targets da EDRAM ----
Texture* render_target(uint32_t base, uint32_t xe_format, uint32_t pitch, bool depth, uint32_t min_height) {
  if (!pitch) pitch = kWidth;
  DXGI_FORMAT f = depth ? DXGI_FORMAT_D24_UNORM_S8_UINT : (DXGI_FORMAT)color_rt_dxgi(xe_format);
  RtKey key{base, (uint32_t)f, pitch, depth};
  Texture& t = g.rts[key];
  // Altura pedida pela tesoura, que o jogo deixa em 8191 quando não a usa: limitada ao que cabe nos 10 MiB da EDRAM. Sem isto
  // cada superfície podia ter 8192 linhas × escala: a 2× gastava centenas de MB, a 3× passava do limite do D3D12 (16384).
  const bool wide = f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_UNORM || f == DXGI_FORMAT_R32G32_FLOAT;
  const uint32_t edram_h = (10u << 20) / (pitch * (wide ? 8 : 4));
  const uint32_t logical_h = std::min<uint32_t>(std::max<uint32_t>((min_height + 15) & ~15u, kHeight), std::max<uint32_t>(edram_h, kHeight));
  constexpr uint32_t kMaxTexture = D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
  uint32_t h = std::min(scaled((float)logical_h), kMaxTexture);
  if (t.res && t.height >= h) return &t;
  if (t.res) g.garbage.push_back(t.res), t.res = nullptr;  // recriado maior (conteúdo perdido)
  if (!create_texture(t, std::min(scaled((float)std::min<uint32_t>(pitch, 8192)), kMaxTexture), h, f, !depth, depth)) return nullptr;
  if (depth) g.list->ClearDepthStencilView(t.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
  else {
    const float zero[4] = {0, 0, 0, 0};
    g.list->ClearRenderTargetView(t.rtv, zero, 0, nullptr);
  }
  return &t;
}

// FSR 1.0 (AMD FidelityFX Super Resolution, licença MIT): EASU e RCAS, versão HLSL para texturas LDR em espaço percetual.
// Portado da descrição de referência ffx_fsr1.h (forma de 32 bits com Load em vez de Gather).
#define FSR_HLSL \
  "float3 xe_t(int2 p) { uint w, h; src.GetDimensions(w, h); return src.Load(int3(clamp(p, int2(0, 0), int2(w - 1, h - 1)), 0)).rgb; }\n" \
  "float xe_l(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n" \
  "void xe_tap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c) {\n" \
  "  float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;\n" \
  "  float d2 = min(v.x * v.x + v.y * v.y, clp);\n" \
  "  float wB = 0.4 * d2 - 1.0, wA = lob * d2 - 1.0; wB *= wB; wA *= wA;\n" \
  "  wB = 1.5625 * wB - 0.5625;\n" \
  "  float w = wB * wA; aC += c * w; aW += w;\n" \
  "}\n" \
  "void xe_set(inout float2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {\n" \
  "  float dc = lD - lC, cb = lC - lB, lenX = max(abs(dc), abs(cb)); lenX = lenX > 0.0 ? 1.0 / lenX : 0.0;\n" \
  "  float dirX = lD - lB; dir.x += dirX * w; lenX = saturate(abs(dirX) * lenX); lenX *= lenX; len += lenX * w;\n" \
  "  float ec = lE - lC, ca = lC - lA, lenY = max(abs(ec), abs(ca)); lenY = lenY > 0.0 ? 1.0 / lenY : 0.0;\n" \
  "  float dirY = lE - lA; dir.y += dirY * w; lenY = saturate(abs(dirY) * lenY); lenY *= lenY; len += lenY * w;\n" \
  "}\n" \
  "float4 xe_easu(float2 uv) {\n" \
  "  uint tw, th; src.GetDimensions(tw, th);\n" \
  "  float2 pp = uv * float2(tw, th) - 0.5, fp = floor(pp); pp -= fp; int2 o = int2(fp);\n" \
  "  float3 b = xe_t(o + int2(0, -1)), c = xe_t(o + int2(1, -1)), e = xe_t(o + int2(-1, 0)), f = xe_t(o), g = xe_t(o + int2(1, 0)),\n" \
  "         h = xe_t(o + int2(2, 0)), i = xe_t(o + int2(-1, 1)), j = xe_t(o + int2(0, 1)), k = xe_t(o + int2(1, 1)),\n" \
  "         l = xe_t(o + int2(2, 1)), n = xe_t(o + int2(0, 2)), oo = xe_t(o + int2(1, 2));\n" \
  "  float bL = xe_l(b), cL = xe_l(c), eL = xe_l(e), fL = xe_l(f), gL = xe_l(g), hL = xe_l(h), iL = xe_l(i), jL = xe_l(j), kL = xe_l(k), lL = xe_l(l), nL = xe_l(n), oL = xe_l(oo);\n" \
  "  float2 dir = float2(0.0, 0.0); float len = 0.0;\n" \
  "  xe_set(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);\n" \
  "  xe_set(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);\n" \
  "  xe_set(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);\n" \
  "  xe_set(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);\n" \
  "  float dirR = dot(dir, dir); bool zro = dirR < (1.0 / 32768.0);\n" \
  "  dirR = zro ? 1.0 : 1.0 / sqrt(dirR); dir.x = zro ? 1.0 : dir.x; dir *= dirR;\n" \
  "  len = len * 0.5; len *= len;\n" \
  "  float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));\n" \
  "  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);\n" \
  "  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len, clp = 1.0 / lob;\n" \
  "  float3 mn = min(f, min(g, min(j, k))), mx = max(f, max(g, max(j, k)));\n" \
  "  float3 aC = float3(0, 0, 0); float aW = 0.0;\n" \
  "  xe_tap(aC, aW, float2(0, -1) - pp, dir, len2, lob, clp, b); xe_tap(aC, aW, float2(1, -1) - pp, dir, len2, lob, clp, c);\n" \
  "  xe_tap(aC, aW, float2(-1, 1) - pp, dir, len2, lob, clp, i); xe_tap(aC, aW, float2(0, 1) - pp, dir, len2, lob, clp, j);\n" \
  "  xe_tap(aC, aW, float2(0, 0) - pp, dir, len2, lob, clp, f); xe_tap(aC, aW, float2(-1, 0) - pp, dir, len2, lob, clp, e);\n" \
  "  xe_tap(aC, aW, float2(1, 1) - pp, dir, len2, lob, clp, k); xe_tap(aC, aW, float2(2, 1) - pp, dir, len2, lob, clp, l);\n" \
  "  xe_tap(aC, aW, float2(2, 0) - pp, dir, len2, lob, clp, h); xe_tap(aC, aW, float2(1, 0) - pp, dir, len2, lob, clp, g);\n" \
  "  xe_tap(aC, aW, float2(1, 2) - pp, dir, len2, lob, clp, oo); xe_tap(aC, aW, float2(0, 2) - pp, dir, len2, lob, clp, n);\n" \
  "  return float4(min(mx, max(mn, aC / aW)), 1.0);\n" \
  "}\n" \
  "float4 xe_rcas(float2 pos, float con) {\n" \
  "  int2 p = int2(pos);\n" \
  "  float3 b = xe_t(p + int2(0, -1)), d = xe_t(p + int2(-1, 0)), e = xe_t(p), f = xe_t(p + int2(1, 0)), h = xe_t(p + int2(0, 1));\n" \
  "  float bL = xe_l(b), dL = xe_l(d), eL = xe_l(e), fL = xe_l(f), hL = xe_l(h);\n" \
  "  float nz = 0.25 * (bL + dL + fL + hL) - eL;\n" \
  "  nz = saturate(abs(nz) / max(1e-5, max(max(bL, dL), max(eL, max(fL, hL))) - min(min(bL, dL), min(eL, min(fL, hL)))));\n" \
  "  nz = -0.5 * nz + 1.0;\n" \
  "  float3 mn4 = min(b, min(d, min(f, h))), mx4 = max(b, max(d, max(f, h)));\n" \
  "  float3 hitMin = mn4 / max(4.0 * mx4, 1e-5), hitMax = (1.0 - mx4) / min(4.0 * mn4 - 4.0, -1e-5);\n" \
  "  float3 lobeRGB = max(-hitMin, hitMax);\n" \
  "  float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * con;\n" \
  "  lobe *= nz;\n" \
  "  return float4(saturate((lobe * (b + d + h + f) + e) / (4.0 * lobe + 1.0)), 1.0);\n" \
  "}\n"

// ---- Root signatures / PSOs ----
bool create_roots() {
  D3D12_ROOT_PARAMETER p[16] = {};
  auto cbv = [&](int i, UINT reg, D3D12_SHADER_VISIBILITY vis) {
    p[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    p[i].Descriptor.ShaderRegister = reg;
    p[i].ShaderVisibility = vis;
  };
  cbv(0, 0, D3D12_SHADER_VISIBILITY_VERTEX);
  cbv(1, 0, D3D12_SHADER_VISIBILITY_PIXEL);
  cbv(2, 1, D3D12_SHADER_VISIBILITY_ALL);
  cbv(3, 2, D3D12_SHADER_VISIBILITY_ALL);
  cbv(4, 3, D3D12_SHADER_VISIBILITY_ALL);
  for (int i = 0; i < 9; i++) {  // t0 (índices) + t1..t8 (vértices), space1
    p[5 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    p[5 + i].Descriptor.ShaderRegister = i;
    p[5 + i].Descriptor.RegisterSpace = 1;
    p[5 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
  }
  D3D12_DESCRIPTOR_RANGE srv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0};
  D3D12_DESCRIPTOR_RANGE smp{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 32, 0, 0, 0};
  p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[14].DescriptorTable = {1, &srv};
  p[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  p[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  p[15].DescriptorTable = {1, &smp};
  p[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC rd{16, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = nullptr, *err = nullptr;
  if (FAILED(g.serialize_rs(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) {
    std::fprintf(stderr, "[d3d12] root signature: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
    return false;
  }
  if (FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root))))
    return false;
  blob->Release();

  // Blit: 4 constantes (uv0, duv por pixel) + 1 flag de filtro, tabela t0, samplers estáticos ponto/linear.
  D3D12_ROOT_PARAMETER bp[2] = {};
  bp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  bp[0].Constants = {0, 0, 8};
  bp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_DESCRIPTOR_RANGE bsrv{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
  bp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  bp[1].DescriptorTable = {1, &bsrv};
  bp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC ss[2] = {};
  for (int i = 0; i < 2; i++) {
    ss[i].Filter = i ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    ss[i].AddressU = ss[i].AddressV = ss[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    ss[i].MaxLOD = D3D12_FLOAT32_MAX;
    ss[i].ShaderRegister = i;
    ss[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }
  D3D12_ROOT_SIGNATURE_DESC brd{2, bp, 2, ss, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  if (FAILED(g.serialize_rs(&brd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err))) return false;
  if (FAILED(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                           IID_PPV_ARGS(&g.blit_root))))
    return false;
  blob->Release();

  static const char kBlitVs[] =
      "float4 main(uint id : SV_VertexID) : SV_Position {\n"
      "  float2 uv = float2((id << 1) & 2, id & 2);\n"
      "  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n}\n";
  static const char kBlitPs[] =
      "cbuffer P : register(b0) { float4 xf; float4 opt; };\n"
      "Texture2D<float4> src : register(t0);\n"
      "SamplerState sp : register(s0);\nSamplerState sl : register(s1);\n"
      FSR_HLSL
      "float4 main(float4 pos : SV_Position) : SV_Target {\n"
      "  float2 uv = xf.xy + pos.xy * xf.zw;\n"
      "  float4 c = opt.x > 0.5 ? src.SampleLevel(sl, uv, 0) : src.SampleLevel(sp, uv, 0);\n"
      // opt.y: 1 = profundidade em cinzento (capturas), 2 = D24S8 empacotado como 8888 (stencil no byte baixo,
      // profundidade nos 24 bits altos), como o Xenos escreve um resolve de profundidade.
      // opt.y = 3: EASU, opt.y = 4: RCAS (FSR 1.0; ver kFsr).
      "  if (opt.y > 3.5) return xe_rcas(pos.xy, opt.w);\n"
      "  if (opt.y > 2.5) return xe_easu(uv);\n"
      "  if (opt.y > 1.5) { uint d = (uint)(saturate(c.r) * 16777215.0 + 0.5);\n"
      "    c = float4(0.0, (d & 0xFFu) / 255.0, ((d >> 8) & 0xFFu) / 255.0, (d >> 16) / 255.0); }\n"
      "  else if (opt.y > 0.5) c = float4(c.rrr, 1.0);\n"
      "  else c.rgb *= opt.w;\n"  // copy_dest_exp_bias
      "  if (opt.z > 0.5) c.a = 1.0;\n"
      "  return c;\n}\n";
  std::string e;
  g.blit_vs = compile_hlsl(kBlitVs, "vs_5_0", "blit_vs", &e);
  g.blit_ps = compile_hlsl(kBlitPs, "ps_5_0", "blit_ps", &e);
  if (!g.blit_vs || !g.blit_ps) {
    std::fprintf(stderr, "[d3d12] shaders de blit: %s\n", e.c_str());
    return false;
  }
  return true;
}

ID3D12PipelineState* blit_pipeline(DXGI_FORMAT f) {
  auto it = g.blit_pso.find((uint32_t)f);
  if (it != g.blit_pso.end()) return it->second;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = g.blit_root;
  d.VS = {g.blit_vs->GetBufferPointer(), g.blit_vs->GetBufferSize()};
  d.PS = {g.blit_ps->GetBufferPointer(), g.blit_ps->GetBufferSize()};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = ~0u;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.RasterizerState.DepthClipEnable = TRUE;
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.NumRenderTargets = 1;
  d.RTVFormats[0] = f;
  d.SampleDesc.Count = 1;
  ID3D12PipelineState* pso = nullptr;
  if (FAILED(g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso)))) pso = nullptr;
  return g.blit_pso[(uint32_t)f] = pso;
}

// Copia src[sx..sx+w, sy..sy+h] para dst (viewport dx, dy, dw, dh), com escala e filtro opcional.
// depth_mode: 0 = cor (× color_scale), 1 = profundidade em cinzento, 2 = profundidade empacotada D24S8.
enum DepthMode { kColor = 0, kDepthGray = 1, kDepthPacked = 2, kColorEasu = 3, kColorRcas = 4 };
void blit(Texture& src, ID3D12Resource* dst_res, D3D12_RESOURCE_STATES& dst_state, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
          DXGI_FORMAT dst_format, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh,
          bool linear, int depth_mode, bool opaque, float color_scale = 1.0f, uint32_t swizzle = kSwizzleIdentity) {
  ID3D12PipelineState* pso = blit_pipeline(dst_format);
  if (!pso || !src.res) return;
  transition(src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  transition(dst_res, dst_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  uint32_t slot = srv_alloc(1);
  write_srv(src, slot, swizzle);
  g.list->SetPipelineState(pso);
  g.bound_root = nullptr, g.bound_pso = nullptr;
  g.list->SetGraphicsRootSignature(g.blit_root);
  float c[8] = {sx / src.width - dx * (sw / dw) / src.width, sy / src.height - dy * (sh / dh) / src.height,
                (sw / dw) / src.width, (sh / dh) / src.height, linear ? 1.0f : 0.0f, (float)depth_mode,
                opaque ? 1.0f : 0.0f, color_scale};
  g.list->SetGraphicsRoot32BitConstants(0, 8, c, 0);
  g.list->SetGraphicsRootDescriptorTable(1, gpu_handle(g.srv_heap, g.srv_inc, slot));
  g.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  D3D12_VIEWPORT vp{dx, dy, dw, dh, 0, 1};
  D3D12_RECT sc{(LONG)dx, (LONG)dy, (LONG)(dx + dw), (LONG)(dy + dh)};
  g.list->RSSetViewports(1, &vp);
  g.list->RSSetScissorRects(1, &sc);
  g.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.list->DrawInstanced(3, 1, 0, 0);
}

// ---- Shaders ----
// Compilação FXC em threads de fundo: o FXC demora de 50 ms a vários segundos por shader e parava o processador de
// comandos ao entrar em áreas novas. Um desenho espera pelo seu shader até gastar o orçamento do frame (GOW_SHADER_WAIT_MS,
// 120 ms): sem isto, numa cache vazia, as personagens (VS de skinning, até 8 s de FXC) ficavam invisíveis mais tempo. Mais do
// que isto faz o jogo parar ao andar por áreas novas. Esgotado o orçamento, é saltado, como no modo assíncrono do Xenia.
// GOW_SYNC_SHADERS=1 compila no momento.
struct CompileJob {
  Compiled* c;
  std::string name, target;
  std::filesystem::path cache_file;
  std::vector<uint32_t> ucode;
};
std::mutex g_compile_mu;
std::condition_variable g_compile_cv;
std::deque<CompileJob> g_compile_jobs;
std::atomic<uint32_t> g_compiles_pending{0};

void finish_compile(Compiled& c, const std::string& name, const std::filesystem::path& cache_file,
                    const std::vector<uint32_t>& u, std::string err, ULONGLONG dt) {
  if (c.blob && !cache_file.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(cache_file.parent_path(), ec);
    std::ofstream(cache_file, std::ios::binary)
        .write(static_cast<const char*>(c.blob->GetBufferPointer()), (std::streamsize)c.blob->GetBufferSize());
  }
  if (dt > 300)
    std::fprintf(stderr, "[d3d12] shader %s: compilação lenta (%llu ms, %u CF, %u ALU, %u fetch)\n", name.c_str(),
                 (unsigned long long)dt, c.info.cf_count, c.info.alu_count, c.info.fetch_count);
  if (!c.blob || g.dump_shaders) {
    std::error_code ec;
    std::filesystem::create_directories("frames/shaders", ec);
    std::ofstream("frames/shaders/" + name + ".hlsl") << c.info.hlsl << "\n/*\n" << err << "\n*/\n";
    std::ofstream bin("frames/shaders/" + name + ".bin", std::ios::binary);
    bin.write(reinterpret_cast<const char*>(u.data()), u.size() * 4);
  }
  if (!c.blob)
    std::fprintf(stderr, "[d3d12] shader %s não compilou (%u CF, %u ALU, %u fetch): %.300s\n", name.c_str(),
                 c.info.cf_count, c.info.alu_count, c.info.fetch_count, err.c_str());
}

bool wait_for_shader(Compiled& c) {
  static const int budget_ms = [] {
    const char* e = std::getenv("GOW_SHADER_WAIT_MS");
    return e ? std::atoi(e) : 120;
  }();
  const ULONGLONG t0 = GetTickCount64();
  while (c.pending.load(std::memory_order_acquire)) {
    if ((int)(GetTickCount64() - t0) + g.shader_wait_ms >= budget_ms) {
      g.shader_wait_ms += (int)(GetTickCount64() - t0);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  g.shader_wait_ms += (int)(GetTickCount64() - t0);
  return true;
}

void compile_worker() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);  // o jogo primeiro
  for (;;) {
    CompileJob job;
    {
      std::unique_lock<std::mutex> lock(g_compile_mu);
      g_compile_cv.wait(lock, [] { return !g_compile_jobs.empty(); });
      job = std::move(g_compile_jobs.front());
      g_compile_jobs.pop_front();
    }
    std::string err;
    ULONGLONG t0 = GetTickCount64();
    ID3DBlob* blob = compile_hlsl(job.c->info.hlsl, job.target.c_str(), job.name.c_str(), &err);
    job.c->blob = blob;
    finish_compile(*job.c, job.name, job.cache_file, job.ucode, err, GetTickCount64() - t0);
    job.c->pending.store(false, std::memory_order_release);
    g_compiles_pending.fetch_sub(1);
  }
}

Compiled* shader(bool pixel) {
  Prof pf(kProfShader);
  const std::vector<uint32_t>& u = pixel ? g.ps_ucode : g.vs_ucode;
  if (u.empty()) return nullptr;
  uint64_t h = (pixel ? g.ps_hash : g.vs_hash) ^ (pixel ? 0x5053ull : 0x5653ull);
  auto it = g.shaders.find(h);
  if (it != g.shaders.end()) {
    if (it->second.pending.load(std::memory_order_acquire) && !wait_for_shader(it->second)) return nullptr;
    return it->second.blob ? &it->second : nullptr;
  }
  Compiled& c = g.shaders[h];
  c.info = translate_shader(u, pixel);
  char name[48];
  std::snprintf(name, sizeof name, "%s_%016llX", pixel ? "ps" : "vs", (unsigned long long)c.info.hash);
  std::string err = c.info.error;
  Phase ph("compilação de shader");
  if (g.dump_shaders) {  // antes de compilar: se o FXC encravar, o shader culpado fica gravado
    std::error_code ec;
    std::filesystem::create_directories("frames/shaders", ec);
    std::ofstream(std::string("frames/shaders/") + name + ".hlsl") << c.info.hlsl;
  }
  ULONGLONG t0 = GetTickCount64();
  // Cache em disco do bytecode, pelo hash do HLSL gerado (mudanças no tradutor invalidam-na sozinhas).
  std::filesystem::path cache_file;
  if (c.info.ok) {

    char key[40];
    std::snprintf(key, sizeof key, "%016llX.dxbc",
                  (unsigned long long)hash_bytes(reinterpret_cast<const uint8_t*>(c.info.hlsl.data()), c.info.hlsl.size()));
    cache_file = vfs::user_root() / "shader_cache" / key;
    std::ifstream in(cache_file, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!bytes.empty() && g.create_blob && SUCCEEDED(g.create_blob(bytes.size(), &c.blob)))
      std::memcpy(c.blob->GetBufferPointer(), bytes.data(), bytes.size());
  }
  static const bool sync = std::getenv("GOW_SYNC_SHADERS") != nullptr;
  const bool cached = c.blob != nullptr;
  if (c.info.ok && !c.blob && !sync) {
    static std::once_flag workers;
    std::call_once(workers, [] {
      const char* e = std::getenv("GOW_FXC_THREADS");
      const int hc = (int)std::thread::hardware_concurrency();
      const int workers_n = e ? std::atoi(e) : std::clamp(hc - 2, 2, 6);
      for (int i = 0; i < workers_n; i++) std::thread(compile_worker).detach();
    });
    c.pending = true;
    g_compiles_pending.fetch_add(1);
    {
      std::lock_guard<std::mutex> lock(g_compile_mu);
      g_compile_jobs.push_back({&c, name, pixel ? "ps_5_1" : "vs_5_1", cache_file, u});
    }
    g_compile_cv.notify_one();
    return wait_for_shader(c) && c.blob ? &c : nullptr;
  }
  if (c.info.ok && !c.blob) c.blob = compile_hlsl(c.info.hlsl, pixel ? "ps_5_1" : "vs_5_1", name, &err);
  finish_compile(c, name, cached ? std::filesystem::path() : cache_file, u, err, GetTickCount64() - t0);
  return c.blob ? &c : nullptr;
}

D3D12_BLEND blend_factor(uint32_t f) {
  switch (f) {
    case 0: return D3D12_BLEND_ZERO;
    case 1: return D3D12_BLEND_ONE;
    case 4: return D3D12_BLEND_SRC_COLOR;
    case 5: return D3D12_BLEND_INV_SRC_COLOR;
    case 6: return D3D12_BLEND_SRC_ALPHA;
    case 7: return D3D12_BLEND_INV_SRC_ALPHA;
    case 8: return D3D12_BLEND_DEST_COLOR;
    case 9: return D3D12_BLEND_INV_DEST_COLOR;
    case 10: return D3D12_BLEND_DEST_ALPHA;
    case 11: return D3D12_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D12_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 16: return D3D12_BLEND_SRC_ALPHA_SAT;
    default: return D3D12_BLEND_ONE;
  }
}
D3D12_BLEND alpha_factor(uint32_t f) {  // fatores de cor não são válidos no alfa
  D3D12_BLEND b = blend_factor(f);
  if (b == D3D12_BLEND_SRC_COLOR) return D3D12_BLEND_SRC_ALPHA;
  if (b == D3D12_BLEND_INV_SRC_COLOR) return D3D12_BLEND_INV_SRC_ALPHA;
  if (b == D3D12_BLEND_DEST_COLOR) return D3D12_BLEND_DEST_ALPHA;
  if (b == D3D12_BLEND_INV_DEST_COLOR) return D3D12_BLEND_INV_DEST_ALPHA;
  return b;
}
D3D12_BLEND_OP blend_op(uint32_t o) {
  switch (o) {
    case 1: return D3D12_BLEND_OP_SUBTRACT;
    case 2: return D3D12_BLEND_OP_MIN;
    case 3: return D3D12_BLEND_OP_MAX;
    case 4: return D3D12_BLEND_OP_REV_SUBTRACT;
    default: return D3D12_BLEND_OP_ADD;
  }
}

struct DrawState {
  DXGI_FORMAT rt[4] = {};
  uint32_t num_rt = 0;
  DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
  uint32_t blend[4] = {}, color_mask = 0, depth_control = 0, stencil = 0;
  uint32_t cull = 0;  // PA_SU_SC_MODE_CNTL bits 0..2: cull_front, cull_back, face (1 = frente CW)
  int32_t depth_bias = 0;   // PA_SU_POLY_OFFSET em unidades de D24
  uint32_t slope_bits = 0;  // idem, declive (bits do float, para a chave do PSO)
  D3D12_PRIMITIVE_TOPOLOGY_TYPE topology = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
};

ID3D12PipelineState* pipeline(Compiled* vs, Compiled* ps, const DrawState& s) {
  Prof pf(kProfPso);
  uint32_t words[20] = {(uint32_t)vs->info.hash, (uint32_t)(vs->info.hash >> 32), ps ? (uint32_t)ps->info.hash : 0,
                        ps ? (uint32_t)(ps->info.hash >> 32) : 0, (uint32_t)s.rt[0], (uint32_t)s.rt[1],
                        (uint32_t)s.rt[2], (uint32_t)s.rt[3], s.num_rt, (uint32_t)s.dsv, s.blend[0], s.blend[1],
                        s.blend[2], s.blend[3], s.color_mask, s.depth_control, s.stencil, (uint32_t)s.topology, s.cull, (uint32_t)s.depth_bias ^ (s.slope_bits * 31u)};
  uint64_t key = hash_words(words, 20);
  auto it = g.psos.find(key);
  if (it != g.psos.end()) return it->second;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = g.root;
  d.VS = {vs->blob->GetBufferPointer(), vs->blob->GetBufferSize()};
  if (ps) d.PS = {ps->blob->GetBufferPointer(), ps->blob->GetBufferSize()};
  d.BlendState.IndependentBlendEnable = TRUE;
  for (uint32_t i = 0; i < s.num_rt; i++) {
    auto& b = d.BlendState.RenderTarget[i];
    uint32_t bc = s.blend[i];
    uint32_t cs = bc & 0x1F, co = (bc >> 5) & 7, cd = (bc >> 8) & 0x1F;
    uint32_t as = (bc >> 16) & 0x1F, ao = (bc >> 21) & 7, ad = (bc >> 24) & 0x1F;
    b.BlendEnable = !(cs == 1 && cd == 0 && co == 0 && as == 1 && ad == 0 && ao == 0);
    b.SrcBlend = blend_factor(cs);
    b.DestBlend = blend_factor(cd);
    b.BlendOp = blend_op(co);
    b.SrcBlendAlpha = alpha_factor(as);
    b.DestBlendAlpha = alpha_factor(ad);
    b.BlendOpAlpha = blend_op(ao);
    b.LogicOp = D3D12_LOGIC_OP_NOOP;
    b.RenderTargetWriteMask = (UINT8)((s.color_mask >> (4 * i)) & 0xF);
  }
  d.SampleMask = ~0u;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // Culling do Xenos: face = 1 → a frente é CW (convenção do D3D9); a orientação do ecrã é a mesma do host.
  d.RasterizerState.CullMode = (s.cull & 2)   ? D3D12_CULL_MODE_BACK
                               : (s.cull & 1) ? D3D12_CULL_MODE_FRONT
                                              : D3D12_CULL_MODE_NONE;
  d.RasterizerState.FrontCounterClockwise = (s.cull & 4) ? FALSE : TRUE;
  d.RasterizerState.DepthClipEnable = TRUE;
  uint32_t dc = s.depth_control;
  if (s.dsv != DXGI_FORMAT_UNKNOWN) {
    d.DepthStencilState.DepthEnable = (dc >> 1) & 1;
    d.DepthStencilState.DepthWriteMask = ((dc >> 2) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthStencilState.DepthFunc = (D3D12_COMPARISON_FUNC)(((dc >> 4) & 7) + 1);
    d.DepthStencilState.StencilEnable = dc & 1;
    d.DepthStencilState.StencilReadMask = (UINT8)((s.stencil >> 8) & 0xFF);
    d.DepthStencilState.StencilWriteMask = (UINT8)((s.stencil >> 16) & 0xFF);
    auto face = [](uint32_t f, uint32_t fail, uint32_t zpass, uint32_t zfail) {
      D3D12_DEPTH_STENCILOP_DESC o;
      o.StencilFunc = (D3D12_COMPARISON_FUNC)(f + 1);
      o.StencilFailOp = (D3D12_STENCIL_OP)(fail + 1);
      o.StencilPassOp = (D3D12_STENCIL_OP)(zpass + 1);
      o.StencilDepthFailOp = (D3D12_STENCIL_OP)(zfail + 1);
      return o;
    };
    d.DepthStencilState.FrontFace = face((dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7);
    d.DepthStencilState.BackFace = ((dc >> 7) & 1)
                                       ? face((dc >> 20) & 7, (dc >> 23) & 7, (dc >> 26) & 7, (dc >> 29) & 7)
                                       : d.DepthStencilState.FrontFace;
    d.DSVFormat = s.dsv;
  }
  // Passes de luz (profundidade igual à do passe base, sem escrita): cada VS traduzido calcula a posição com
  // arredondamentos ligeiramente diferentes (no Xenos é o mesmo microcódigo de posição, invariante) e o épsilon do jogo
  // não chega → z-fighting aos triângulos. Um pequeno desvio a favor do teste resolve.
  if (s.dsv != DXGI_FORMAT_UNKNOWN && ((dc >> 1) & 1) && !((dc >> 2) & 1)) {
    const uint32_t zf = (dc >> 4) & 7;
    static const int bias_units = std::getenv("GOW_LIGHT_BIAS") ? std::atoi(std::getenv("GOW_LIGHT_BIAS")) : 4000;
    static const float bias_slope = std::getenv("GOW_LIGHT_SLOPE") ? (float)std::atof(std::getenv("GOW_LIGHT_SLOPE")) : 0.25f;
    if (zf == 6 || zf == 4) d.RasterizerState.DepthBias = bias_units, d.RasterizerState.SlopeScaledDepthBias = bias_slope;  // >=, >
    else if (zf == 3 || zf == 1) d.RasterizerState.DepthBias = -bias_units, d.RasterizerState.SlopeScaledDepthBias = -bias_slope;  // <=, <
  }
  if (s.depth_bias || s.slope_bits) {
    float slope;
    std::memcpy(&slope, &s.slope_bits, 4);
    d.RasterizerState.DepthBias += s.depth_bias;
    d.RasterizerState.SlopeScaledDepthBias += slope;
  }
  d.PrimitiveTopologyType = s.topology;
  d.NumRenderTargets = s.num_rt;
  for (uint32_t i = 0; i < s.num_rt; i++) d.RTVFormats[i] = s.rt[i];
  d.SampleDesc.Count = 1;
  ID3D12PipelineState* pso = nullptr;
  Phase ph("criação de PSO");
  HRESULT hr = g.device->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (FAILED(hr)) {
    std::fprintf(stderr, "[d3d12] CreateGraphicsPipelineState falhou 0x%08lX (vs %016llX ps %016llX)\n", hr,
                 (unsigned long long)vs->info.hash, ps ? (unsigned long long)ps->info.hash : 0ull);
    pso = nullptr;
  }
  return g.psos[key] = pso;
}

// ---- Índices ----
uint32_t guest_index(const uint8_t* p, bool i32, uint32_t endian, uint32_t i) {
  if (i32) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    switch (endian) {
      case 1: v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu); break;
      case 2: v = __builtin_bswap32(v); break;
      case 3: v = (v << 16) | (v >> 16); break;
    }
    return v;
  }
  uint16_t v;
  std::memcpy(&v, p + 2 * i, 2);
  if (endian == 1 || endian == 2) v = __builtin_bswap16(v);
  return v;
}

// Lista do host: primitivas convertidas em listas; rect lists com o 4º canto marcado (0x80000000 | posição).
bool expand(uint32_t prim, const std::vector<uint32_t>& g_idx, bool restart, uint32_t restart_value,
            std::vector<uint32_t>& out, D3D12_PRIMITIVE_TOPOLOGY& topo, D3D12_PRIMITIVE_TOPOLOGY_TYPE& type) {
  size_t n = g_idx.size();
  out.clear();
  auto segments = [&](auto&& fn) {
    size_t s = 0;
    for (size_t i = 0; i <= n; i++)
      if (i == n || (restart && g_idx[i] == restart_value)) {
        fn(s, i);
        s = i + 1;
      }
  };
  switch (prim) {
    case 1:
      out = g_idx;
      topo = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
      return true;
    case 2:
      out.assign(g_idx.begin(), g_idx.begin() + (n & ~size_t(1)));
      topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case 3:
      segments([&](size_t a, size_t b) {
        for (size_t i = a; i + 1 < b; i++) out.push_back(g_idx[i]), out.push_back(g_idx[i + 1]);
      });
      topo = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
      type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
      return true;
    case 4:
      out.assign(g_idx.begin(), g_idx.begin() + n / 3 * 3);
      break;
    case 5:
      segments([&](size_t a, size_t b) {
        for (size_t i = a + 1; i + 1 < b; i++) out.push_back(g_idx[a]), out.push_back(g_idx[i]), out.push_back(g_idx[i + 1]);
      });
      break;
    case 6:
      segments([&](size_t a, size_t b) {
        for (size_t i = a; i + 2 < b; i++) {
          bool odd = (i - a) & 1;
          out.push_back(g_idx[odd ? i + 1 : i]);
          out.push_back(g_idx[odd ? i : i + 1]);
          out.push_back(g_idx[i + 2]);
        }
      });
      break;
    case 8:
      for (size_t r = 0; r + 3 <= n; r += 3) {
        uint32_t p = (uint32_t)out.size();
        out.push_back(g_idx[r]);
        out.push_back(g_idx[r + 1]);
        out.push_back(g_idx[r + 2]);
        out.push_back(g_idx[r + 2]);
        out.push_back(g_idx[r + 1]);
        out.push_back(0x80000000u | p);
      }
      break;
    case 0xD:
      for (size_t q = 0; q + 4 <= n; q += 4) {
        const uint32_t* v = &g_idx[q];
        out.insert(out.end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
      }
      break;
    default:
      return false;
  }
  topo = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  type = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  return true;
}

D3D12_TEXTURE_ADDRESS_MODE address_mode(uint32_t c) {
  switch (c) {
    case 0: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    case 1: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: case 5: case 7: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    case 6: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  }
}

struct SysConstants {
  float vp_scale[4], vp_offset[4], rt[4];
  uint32_t flags[4];
  float alpha_ref[4];
  uint32_t vb_size[8];
  float tex_size[32][4];
  float rt_scale[4];  // 2^color_exp_bias de cada RT (RB_COLOR*_INFO bits 20..25, com sinal)
  float rt_max[4];    // 31,875 nos RT 2_10_10_10_FLOAT (7e3); 0 nos outros
  float res[4];       // lógico/host
};

const uint8_t* physical(uint32_t addr) { return gm::base() + 0xA0000000u + (gpu_physical(addr) & 0x1FFFFFFFu); }

void dump_rgba(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, const std::string& name);

// ---- Resolve ----
void resolve() {
  Prof pf(kProfResolve);
  g.resolves++;
  uint32_t cc = reg(kRbCopyControl), info = reg(kRbCopyDestInfo), pitch_reg = reg(kRbCopyDestPitch);
  uint32_t src_sel = cc & 7, command = (cc >> 20) & 3;
  bool clear_color = (cc >> 8) & 1, clear_depth = (cc >> 9) & 1;
  uint32_t dest_base = reg(kRbCopyDestBase), dest_format = (info >> 7) & 0x3F;
  uint32_t dest_pitch = pitch_reg & 0x3FFF, dest_height = (pitch_reg >> 16) & 0x3FFF;
  // Retângulo: 3 vértices (x, y float) do buffer da constante de fetch 0.
  const uint32_t* fc = constant_registers() + (kConstFetch - 0x4000);
  uint32_t vaddr = fc[0] & ~3u, vend = fc[1] & 3;
  float xs[3], ys[3];
  const uint8_t* vp = physical(vaddr);
  for (int i = 0; i < 3; i++) {
    uint32_t w[2];
    std::memcpy(w, vp + 8 * i, 8);
    for (uint32_t& v : w) v = vend == 2 ? __builtin_bswap32(v) : vend == 1 ? (((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu)) : vend == 3 ? ((v << 16) | (v >> 16)) : v;
    std::memcpy(&xs[i], &w[0], 4);
    std::memcpy(&ys[i], &w[1], 4);
  }
  // Os cantos vêm com meio píxel de deslocamento (ex.: -0,5..1279,5): arredonda, como o Xenia. Truncar perdia a
  // última coluna e linha de cada tile, e a linha 511 ficava com conteúdo antigo (costura na fronteira dos tiles).
  for (int i = 0; i < 3; i++) xs[i] = std::round(xs[i]), ys[i] = std::round(ys[i]);
  float x0 = std::max(0.0f, std::min({xs[0], xs[1], xs[2]})), y0 = std::max(0.0f, std::min({ys[0], ys[1], ys[2]}));
  float x1 = std::max({xs[0], xs[1], xs[2]}), y1 = std::max({ys[0], ys[1], ys[2]});
  // Predicated tiling: cada tile é desenhado com PA_SC_WINDOW_OFFSET e fica na EDRAM a partir da origem; o
  // retângulo do resolve está em coordenadas de ecrã, a fonte na EDRAM é rect + deslocamento.
  uint32_t wo = reg(kPaScWindowOffset);
  float ox = (float)((int32_t)(wo << 17) >> 17), oy = (float)((int32_t)((wo >> 16) << 17) >> 17);
  float sx0 = std::max(0.0f, x0 + ox), sy0 = std::max(0.0f, y0 + oy), sx1 = x1 + ox, sy1 = y1 + oy;
  uint32_t surface = reg(kRbSurfaceInfo), pitch = surface & 0x3FFF;
  bool depth = src_sel >= 4;
  uint32_t cinfo = reg(src_sel == 0 ? kRbColorInfo : kRbColor1Info + std::min<uint32_t>(src_sel, 3) - 1);
  uint32_t dinfo = reg(kRbDepthInfo);
  Texture* src = depth ? render_target(dinfo & 0xFFF, 0, pitch, true, (uint32_t)sy1)
                       : render_target(cinfo & 0xFFF, (cinfo >> 16) & 0xF, pitch, false, (uint32_t)sy1);
  if (!src) return;
  if (g.dump_resolves && g.dump_frames.count(g.frame) && sx1 > sx0 && sy1 > sy0) {  // GOW_DUMP_RESOLVES=1
    Texture tmp;
    uint32_t w = (uint32_t)(sx1 - sx0), h = (uint32_t)(sy1 - sy0);
    if (create_texture(tmp, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, false)) {
      blit(*src, tmp.res, tmp.state, tmp.rtv, DXGI_FORMAT_R8G8B8A8_UNORM, sx0 * g.scale, sy0 * g.scale, (float)w * g.scale,
           (float)h * g.scale, 0, 0, (float)w, (float)h, false, depth ? kDepthGray : kColor, true);
      char name[128];
      std::snprintf(name, sizeof name, "frames/frame_%06llu_r%02u_%s%u_base%u_fmt%u_to_%08X.bmp",
                    (unsigned long long)g.frame, g.resolves, depth ? "prof" : "cor", src_sel,
                    depth ? dinfo & 0xFFF : cinfo & 0xFFF, depth ? 0 : (cinfo >> 16) & 0xF, dest_base);
      dump_rgba(tmp.res, tmp.state, name);
      g.garbage.push_back(tmp.res);
      if (!depth) {  // junto de cada resolve de cor, a profundidade atual (frames/depth_*.bmp)
        for (auto& [key, dt] : g.rts) {
          if (!key.depth || key.base != (dinfo & 0xFFF) || key.pitch != pitch || !dt.res) continue;
          Texture td;
          if (create_texture(td, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true, false)) {
            blit(dt, td.res, td.state, td.rtv, DXGI_FORMAT_R8G8B8A8_UNORM, sx0 * g.scale, sy0 * g.scale, (float)w * g.scale, (float)h * g.scale, 0, 0, (float)w, (float)h, false, kDepthGray, true);
            char nd[128];
            std::snprintf(nd, sizeof nd, "frames/depth_%06llu_r%02u.bmp", (unsigned long long)g.frame, g.resolves);
            dump_rgba(td.res, td.state, nd);
            g.garbage.push_back(td.res);
          }
        }
      }
    }
  }
  if (command != 3 && sx1 > sx0 && sy1 > sy0 && dest_base) {
    uint32_t w = (uint32_t)(sx1 - sx0), h = (uint32_t)(sy1 - sy0);
    uint32_t tw = std::min<uint32_t>(scaled((float)std::max(dest_pitch, w)), D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    uint32_t th = std::min<uint32_t>(scaled((float)std::max(dest_height, h)), D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION);
    FormatInfo fi = texture_format_info(dest_format);
    DXGI_FORMAT f = (DXGI_FORMAT)fi.dxgi;
    uint32_t bpp = fi.block_w == 1 ? fi.bytes_per_block : 4;
    if (fi.block_w != 1 || !f || f == DXGI_FORMAT_B5G6R5_UNORM || f == DXGI_FORMAT_B4G4R4A4_UNORM ||
        f == DXGI_FORMAT_B5G5R5A1_UNORM || f == DXGI_FORMAT_R32_UINT)
      f = DXGI_FORMAT_R8G8B8A8_UNORM;
    const uint32_t dphys = gpu_physical(dest_base);
    // copy_dest_exp_bias (RB_COPY_DEST_INFO bits 16..21, com sinal) escala a cor copiada por 2^bias.
    const int bias = (int32_t)(info << 10) >> 26;
    // Um resolve de profundidade dá duas cópias: 8888 empacotado (fetch como cor; stencil no byte baixo) e R32_FLOAT
    // com a profundidade em x (fetch k_24_8 / k_24_8_FLOAT: sombras e o DOF do UberPostProcess lêem t.x; com a cópia
    // empacotada liam o stencil, 0, e o DOF misturava o fundo desfocado por cima das personagens).
    auto copy_to = [&](DXGI_FORMAT tf, int mode, bool is_depth_copy) {
      // Um destino dentro de uma cópia já existente (mesmo pitch) é a continuação dela: o tile seguinte do
      // predicated tiling resolve para base + linhas × pitch × bpp.
      uint32_t row = 0;
      Texture* t = nullptr;
      for (auto& [key, tex] : g.resolved) {
        uint32_t b = key.first;
        if (!tex.res || b >= dphys || tex.guest_pitch != dest_pitch || tex.guest_bpp != bpp || tex.format != tf) continue;
        uint64_t stride = (uint64_t)dest_pitch * bpp, off = dphys - b;
        if (off % stride == 0 && scaled((float)(off / stride + h)) <= tex.height) {
          t = &tex;
          row = (uint32_t)(off / stride);
          break;
        }
      }
      if (!t) {
        t = &g.resolved[{dphys, (uint32_t)tf}];
        if (t->res && (t->width < tw || t->height < th)) g.garbage.push_back(t->res), t->res = nullptr;
        if (!t->res && !create_texture(*t, tw, th, tf, true, false)) t = nullptr;
        if (t) t->guest_pitch = dest_pitch, t->guest_bpp = bpp, t->depth_copy = is_depth_copy;
      }
      if (!t) return;
      const float S = g.scale;
      blit(*src, t->res, t->state, t->rtv, tf, sx0 * S, sy0 * S, w * S, h * S, 0, (float)scaled((float)row), w * S, h * S, false,
           mode, false, std::ldexp(1.0f, bias),
           !depth && ((info >> 24) & 1) ? kSwizzleSwapRB : kSwizzleIdentity);  // copy_dest_swap
      t->frame = g.frame;
      t->serial = ++g.resolve_serial;
      if (g.resolve_to_memory && g.scale == 1.0f && !is_depth_copy && tf == (DXGI_FORMAT)fi.dxgi)
        queue_writeback(*t, row, w, h, dphys, dest_pitch, bpp, info & 7);
    };
    copy_to(f, depth ? kDepthPacked : kColor, false);
    if (depth) copy_to(DXGI_FORMAT_R32_FLOAT, kDepthGray, true);
  }
  if (g.trace && traced_frame()) {  // resolves: sem limite de linhas
    g.trace_lines++;
    std::fprintf(stderr,
                 "[render] resolve frame %llu: fonte %s%u (base %u, fmt %u, pitch %u) rect (%.0f,%.0f)-(%.0f,%.0f) -> "
                 "0x%08X fmt %u pitch %u×%u cmd %u limpa cor %d prof %d info 0x%08X\n",
                 (unsigned long long)g.frame, depth ? "prof" : "cor", src_sel, depth ? dinfo & 0xFFF : cinfo & 0xFFF,
                 depth ? 0 : (cinfo >> 16) & 0xF, pitch, x0, y0, x1, y1, dest_base, dest_format, dest_pitch,
                 dest_height, command, clear_color, clear_depth, info);
  }
  // Limpezas pedidas pelo resolve (no retângulo, em coordenadas da EDRAM).
  D3D12_RECT rc{(LONG)scaled(sx0), (LONG)scaled(sy0), (LONG)scaled(sx1), (LONG)scaled(sy1)};
  if (clear_color && !depth) {
    Texture* c = render_target(cinfo & 0xFFF, (cinfo >> 16) & 0xF, pitch, false, (uint32_t)sy1);
    if (c) {
      uint32_t v = reg(kRbColorClear);
      float col[4] = {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, (v >> 24) / 255.0f};
      transition(*c, D3D12_RESOURCE_STATE_RENDER_TARGET);
      g.list->ClearRenderTargetView(c->rtv, col, 1, &rc);
    }
  }
  if (clear_depth) {
    Texture* d = render_target(dinfo & 0xFFF, 0, pitch, true, (uint32_t)sy1);
    if (d) {
      uint32_t v = reg(kRbDepthClear);
      transition(*d, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      g.list->ClearDepthStencilView(d->dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
                                    (float)(v >> 8) / 16777215.0f, (UINT8)(v & 0xFF), 1, &rc);
    }
  }
}

void write_bmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch) {
  std::ofstream f(path, std::ios::binary);
  uint32_t row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
  uint8_t hdr[54] = {'B', 'M'};
  auto put = [&](int o, uint32_t v) { std::memcpy(hdr + o, &v, 4); };
  put(2, size);
  put(10, 54);
  put(14, 40);
  put(18, w);
  put(22, h);
  hdr[26] = 1;
  hdr[28] = 24;
  put(34, size - 54);
  f.write(reinterpret_cast<char*>(hdr), 54);
  std::vector<uint8_t> line(row + pad);
  for (uint32_t y = 0; y < h; y++) {
    const uint8_t* s = rgba + (size_t)(h - 1 - y) * pitch;
    for (uint32_t x = 0; x < w; x++) {
      line[3 * x] = s[4 * x + 2];
      line[3 * x + 1] = s[4 * x + 1];
      line[3 * x + 2] = s[4 * x];
    }
    f.write(reinterpret_cast<char*>(line.data()), line.size());
  }
}

// Grava um recurso RGBA8 (backbuffer ou cópia temporária) como BMP; submete e espera pela GPU.
void dump_rgba(ID3D12Resource* res, D3D12_RESOURCE_STATES& state, const std::string& name) {
  D3D12_RESOURCE_DESC rd = res->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
  UINT64 total = 0;
  g.device->GetCopyableFootprints(&rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
  ID3D12Resource* rb = nullptr;
  auto hp = heap(D3D12_HEAP_TYPE_READBACK);
  auto bd = buffer_desc(total);
  if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&rb))))
    return;
  D3D12_RESOURCE_STATES before = state;
  transition(res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
  dst.pResource = rb;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = fp;
  src.pResource = res;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  g.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  transition(res, state, before);
  submit(true);
  void* p = nullptr;
  if (SUCCEEDED(rb->Map(0, nullptr, &p))) {
    std::error_code ec;
    std::filesystem::create_directories("frames", ec);
    write_bmp(name, (const uint8_t*)p, (uint32_t)rd.Width, rd.Height, fp.Footprint.RowPitch);
    rb->Unmap(0, nullptr);
    std::fprintf(stderr, "[d3d12] gravado %s\n", name.c_str());
  }
  rb->Release();
}

bool init_device() {
  g.d3d12 = LoadLibraryA("d3d12.dll");
  g.dxgi = LoadLibraryA("dxgi.dll");
  g.compiler = LoadLibraryA("d3dcompiler_47.dll");
  if (!g.d3d12 || !g.dxgi || !g.compiler) {
    std::fprintf(stderr, "[d3d12] DLLs indisponíveis (d3d12 %p, dxgi %p, d3dcompiler_47 %p)\n", (void*)g.d3d12,
                 (void*)g.dxgi, (void*)g.compiler);
    return false;
  }
  auto sym = [](HMODULE m, const char* n) { return reinterpret_cast<void*>(GetProcAddress(m, n)); };
  g.create_device = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(sym(g.d3d12, "D3D12CreateDevice"));
  g.serialize_rs = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(sym(g.d3d12, "D3D12SerializeRootSignature"));
  g.create_factory = reinterpret_cast<PFN_CreateDXGIFactory2>(sym(g.dxgi, "CreateDXGIFactory2"));
  g.compile = reinterpret_cast<PFN_D3DCompile>(sym(g.compiler, "D3DCompile"));
  g.create_blob = reinterpret_cast<HRESULT(WINAPI*)(SIZE_T, ID3DBlob**)>(sym(g.compiler, "D3DCreateBlob"));
  if (!g.create_device || !g.serialize_rs || !g.create_factory || !g.compile) return false;
  if (const char* e = std::getenv("GOW_D3D12_DEBUG"); e && *e == '1') {
    using PFN_GetDebug = HRESULT(WINAPI*)(REFIID, void**);
    auto get_debug = reinterpret_cast<PFN_GetDebug>(sym(g.d3d12, "D3D12GetDebugInterface"));
    ID3D12Debug* dbg = nullptr;
    if (get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer(), dbg->Release();
  }
  if (FAILED(g.create_factory(0, IID_PPV_ARGS(&g.factory)))) return false;
  if (FAILED(g.create_device(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)))) {
    std::fprintf(stderr, "[d3d12] D3D12CreateDevice falhou\n");
    return false;
  }
  D3D12_COMMAND_QUEUE_DESC qd{};
  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) return false;
  HWND hwnd = (HWND)window::create(kWidth, kHeight);
  if (!hwnd) return false;
  g.scale = config::get().resolution_scale;
  window::client_size(g.bb_w, g.bb_h);
  if (!g.bb_w || !g.bb_h) g.bb_w = kWidth, g.bb_h = kHeight;
  std::fprintf(stderr, "[d3d12] saída %u×%u, resolução interna %u×%u\n", g.bb_w, g.bb_h, scaled(kWidth), scaled(kHeight));
  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Width = g.bb_w;
  sd.Height = g.bb_h;
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = kBackBuffers;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* sc1 = nullptr;
  if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue, hwnd, &sd, nullptr, nullptr, &sc1))) {
    std::fprintf(stderr, "[d3d12] CreateSwapChainForHwnd falhou\n");
    return false;
  }
  sc1->QueryInterface(IID_PPV_ARGS(&g.swapchain));
  sc1->Release();
  g.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);  // a janela trata do Alt+Enter
  auto make_heap = [](D3D12_DESCRIPTOR_HEAP_TYPE t, UINT n, bool visible, ID3D12DescriptorHeap** out) {
    D3D12_DESCRIPTOR_HEAP_DESC d{t, n, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    return SUCCEEDED(g.device->CreateDescriptorHeap(&d, IID_PPV_ARGS(out)));
  };
  if (!make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize * kSlots, true, &g.srv_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize * kSlots, true, &g.sampler_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvHeapSize, false, &g.rtv_heap) ||
      !make_heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvHeapSize, false, &g.dsv_heap))
    return false;
  g.srv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  g.sampler_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  g.rtv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  g.dsv_inc = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
  for (uint32_t i = 0; i < kBackBuffers; i++) {
    g.swapchain->GetBuffer(i, IID_PPV_ARGS(&g.backbuffers[i]));
    g.device->CreateRenderTargetView(g.backbuffers[i], nullptr, cpu_handle(g.rtv_heap, g.rtv_inc, i));
  }
  for (auto& a : g.allocators)
    if (FAILED(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return false;
  g.allocator = g.allocators[0];
  if (
      FAILED(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr,
                                         IID_PPV_ARGS(&g.list))) ||
      FAILED(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence))))
    return false;
  g.fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  auto hp = heap(D3D12_HEAP_TYPE_UPLOAD);
  auto bd = buffer_desc(kUploadSize * kSlots);
  if (FAILED(g.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               nullptr, IID_PPV_ARGS(&g.upload))))
    return false;
  g.upload->Map(0, nullptr, reinterpret_cast<void**>(&g.upload_ptr));
  if (!create_roots()) return false;
  bind_heaps();
  // Textura 1×1 branca para slots sem textura.
  DecodedTexture white;
  white.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM;
  white.width = white.height = white.rows = 1;
  white.row_pitch = 4;
  white.data = {255, 255, 255, 255};
  if (create_texture(g.dummy, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, false)) upload_texture(g.dummy, white);
  if (create_texture(g.dummy_cube, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, false, false, 1, 6)) {
    g.dummy_cube.cube = true;
    for (uint32_t i = 0; i < 6; i++) upload_texture(g.dummy_cube, white, i);
  }
  return true;
}

}  // namespace

bool init() {
  if (const char* e = std::getenv("GOW_DUMP_FRAME")) {
    for (const char* p = e; *p;) {
      char* end;
      unsigned long long v = std::strtoull(p, &end, 10);
      if (end == p) break;
      g.dump_frames.insert(v);
      p = *end ? end + 1 : end;
    }
  }
  if (const char* e = std::getenv("GOW_DUMP_SHADERS"); e && *e == '1') g.dump_shaders = true;
  if (const char* e = std::getenv("GOW_DUMP_RESOLVES"); e && *e == '1') g.dump_resolves = true;
  if (const char* e = std::getenv("GOW_RESOLVE_TO_MEMORY"); e && *e == '1') g.resolve_to_memory = true;
  if (const char* e = std::getenv("GOW_TRACE_RENDER"); e && *e) g.trace = true, g.trace_frame = std::strtoull(e, nullptr, 10);
  g.active = init_device();
  std::fprintf(stderr, "[d3d12] renderer %s\n", g.active ? "ativo (janela 1280×720)" : "desligado");
  return g.active;
}

bool active() { return g.active; }
const char* phase() { return g_phase.load(); }

void load_shader(bool pixel, std::vector<uint32_t> ucode) {
  if (!g.active) return;
  uint64_t h = hash_words(ucode.data(), ucode.size());
  if (pixel) g.ps_ucode = std::move(ucode), g.ps_hash = h;
  else g.vs_ucode = std::move(ucode), g.vs_hash = h;
}

// Clears do D3D do X360: rect list em modo só-profundidade (RB_MODECONTROL 5), sem PS, em coordenadas de ecrã, com
// vértices (x, y, z, r, g, b, a). Para acelerar, a superfície é declarada a 4×MSAA com metade do pitch (1 "píxel" =
// 2×2 amostras) e, para limpar cor, a base de profundidade aponta para o RT de cor: a palavra D24<<8|stencil escrita na
// EDRAM é a cor. Aqui cor e profundidade são recursos separados e o MSAA é ignorado, por isso interpretam-se: sem isto
// o RT de atenuação (branco) ficava com a imagem do frame anterior — a sombra modulada multiplicava sobre ela e o
// resultado alimentava a iluminação (personagens "fantasma") — e a profundidade dos mapas de sombra não era limpa.
bool try_clear(uint32_t num) {
  const uint32_t surface = reg(kRbSurfaceInfo), msaa = (surface >> 16) & 3;
  const uint32_t sx = msaa == 2 ? 2 : 1, sy = msaa >= 1 ? 2 : 1;
  const uint32_t pitch = (surface & 0x3FFF) * sx;
  const uint32_t vte = reg(kPaClVteCntl);
  if (!(vte & 0x100) || (vte & 0xF) || !pitch || num < 3 || num % 3) return false;
  const uint32_t* fc = constant_registers() + (kConstFetch - 0x4000);
  const uint32_t vaddr = fc[0] & ~3u, vend = fc[1] & 3, vbytes = ((fc[1] >> 2) & 0xFFFFFF) * 4;
  const uint32_t stride = vbytes / num;
  if (stride < 12 || stride > 64) return false;
  const uint8_t* vp = physical(vaddr);
  auto rd = [&](uint32_t vtx, uint32_t comp) {
    uint32_t v;
    std::memcpy(&v, vp + vtx * stride + comp * 4, 4);
    v = vend == 2 ? __builtin_bswap32(v) : vend == 1 ? (((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu))
                                                     : vend == 3 ? ((v << 16) | (v >> 16)) : v;
    float f;
    std::memcpy(&f, &v, 4);
    return f;
  };
  const uint32_t wo = reg(kPaScWindowOffset);
  const int ox = (int32_t)(wo << 17) >> 17, oy = (int32_t)((wo >> 16) << 17) >> 17;
  const uint32_t dbase = reg(kRbDepthInfo) & 0xFFF, cbase = reg(kRbColorInfo) & 0xFFF;
  const uint32_t dc = reg(kRbDepthControl), st_reg = reg(kRbStencilRefMask);
  const bool clear_z = (dc & 2) && (dc & 4), clear_s = (dc & 1) && ((st_reg >> 16) & 0xFF);
  for (uint32_t r = 0; r < num / 3; r++) {
    float xs[3], ys[3];
    for (uint32_t v = 0; v < 3; v++) xs[v] = rd(r * 3 + v, 0), ys[v] = rd(r * 3 + v, 1);
    LONG x0 = (LONG)std::round(std::min({xs[0], xs[1], xs[2]})), x1 = (LONG)std::round(std::max({xs[0], xs[1], xs[2]}));
    LONG y0 = (LONG)std::round(std::min({ys[0], ys[1], ys[2]})), y1 = (LONG)std::round(std::max({ys[0], ys[1], ys[2]}));
    auto hs = [](LONG v) { return (LONG)std::lround((float)v * g.scale); };
    D3D12_RECT rc{hs((x0 + ox) * (LONG)sx), hs((y0 + oy) * (LONG)sy), hs((x1 + ox) * (LONG)sx), hs((y1 + oy) * (LONG)sy)};
    float z = rd(r * 3, 2);
    if (vte & 0x10) z *= regf(kPaClVportXscale + 4);
    if (vte & 0x20) z += regf(kPaClVportXscale + 5);
    z = std::min(1.0f, std::max(0.0f, z));
    const float color[4] = {stride >= 28 ? rd(r * 3, 3) : 0.0f, stride >= 28 ? rd(r * 3, 4) : 0.0f,
                            stride >= 28 ? rd(r * 3, 5) : 0.0f, stride >= 28 ? rd(r * 3, 6) : 0.0f};
    for (auto& [key, t] : g.rts) {
      if (!t.res || key.base != dbase || key.pitch != pitch) continue;
      D3D12_RECT c{std::max<LONG>(rc.left, 0), std::max<LONG>(rc.top, 0), std::min<LONG>(rc.right, t.width),
                   std::min<LONG>(rc.bottom, t.height)};
      if (c.right <= c.left || c.bottom <= c.top) continue;
      if (key.depth) {
        D3D12_CLEAR_FLAGS fl = (D3D12_CLEAR_FLAGS)((clear_z ? D3D12_CLEAR_FLAG_DEPTH : 0) | (clear_s ? D3D12_CLEAR_FLAG_STENCIL : 0));
        if (!fl) continue;
        transition(t, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        g.list->ClearDepthStencilView(t.dsv, fl, z, (UINT8)(st_reg & 0xFF), 1, &c);
      } else if (dbase == cbase && key.format == color_rt_dxgi((reg(kRbColorInfo) >> 16) & 0xF)) {
        // A palavra de profundidade é a cor deste RT. Só o formato declarado: a mesma base serve várias superfícies
        // (a cena HDR em R16F é guardada e restaurada pelo jogo em volta do buffer de atenuação 8888); aqui são
        // recursos separados, e limpar todos apagava a cena.
        transition(t, D3D12_RESOURCE_STATE_RENDER_TARGET);
        g.list->ClearRenderTargetView(t.rtv, color, 1, &c);
      }
    }
  }
  if (g.trace && traced_frame())
    std::fprintf(stderr, "[render] clear: base c %u d %u pitch %u (msaa %u) n=%u z=%g cor %g %g %g %g%c", cbase, dbase, pitch,
                 msaa, num, rd(0, 2), stride >= 28 ? rd(0, 3) : 0, stride >= 28 ? rd(0, 4) : 0, stride >= 28 ? rd(0, 5) : 0,
                 stride >= 28 ? rd(0, 6) : 0, 10);
  return true;
}

void draw(uint32_t initiator, uint32_t dma_base, uint32_t dma_size, const uint32_t* immediate, uint32_t immediate_count) {
  if (!g.active) return;
  uint32_t mode = reg(kRbModeControl) & 7;
  if (mode == 6) return resolve();
  if (mode != 4 && mode != 5) return;
  Prof pf_draw(kProfDraw);
  uint32_t prim = initiator & 0x3F, source = (initiator >> 6) & 3, num = initiator >> 16;
  static const bool no_clear_hack = std::getenv("GOW_NO_DRAW_CLEARS") != nullptr;
  if (mode == 5 && prim == 8 && source == 2 && !no_clear_hack && try_clear(num)) return;
  bool i32 = (initiator >> 11) & 1;
  if (!num) return;
  Compiled* vs = shader(false);
  Compiled* ps = mode == 4 ? shader(true) : nullptr;
  // GOW_SKIP_PS=hash6,hash6: desenhos com esses PS são saltados (para isolar um efeito).
  static const std::string skip_ps = std::getenv("GOW_SKIP_PS") ? std::getenv("GOW_SKIP_PS") : "";
  if (ps && !skip_ps.empty()) {
    char hx[8];
    std::snprintf(hx, sizeof hx, "%06llX", (unsigned long long)(ps->info.hash >> 40));
    if (skip_ps.find(hx) != std::string::npos) return;
  }
  if (!vs || (mode == 4 && !ps)) {
    g.skipped++;
    return;
  }
  // Margem nos anéis: um submit a meio do desenho invalidaria o que já foi reservado.
  if (g.upload_offset + (96ull << 20) > kUploadSize || g.srv_next + 64 > kSrvHeapSize ||
      g.sampler_next + 64 > kSamplerHeapSize) {
    g.stat_early[g.upload_offset + (96ull << 20) > kUploadSize ? 0 : g.srv_next + 64 > kSrvHeapSize ? 1 : 2]++;
    submit();
  }
  // Índices do convidado. Cada objeto é desenhado uma vez por tile e por luz: a mesma lista expandida serve para todos os
  // desenhos deste submit, se a memória não tiver mudado (expandir e copiar por desenho custava 12 ms/frame com 6000 desenhos).
  Prof pf_idx(kProfIndex);
  bool restart = (reg(kPaSuScModeCntl) >> 21) & 1;
  std::vector<uint32_t> host;
  D3D12_PRIMITIVE_TOPOLOGY topo;
  Phase ph("desenho (índices, texturas, gravação)");
  DrawState st;
  D3D12_GPU_VIRTUAL_ADDRESS idx_va = 0;
  uint32_t host_count = 0;
  uint64_t idx_key = 0;
  if (source == 0) {
    const uint8_t* p = physical(dma_base & ~(i32 ? 3u : 1u));
    const size_t bytes = (size_t)num * (i32 ? 4 : 2), n = std::min<size_t>(bytes, 1024);
    idx_key = hash_bytes(p, n) ^ hash_bytes(p + bytes - n, n) * 3 ^ (uint64_t)dma_base << 20 ^ (uint64_t)num * 0x100000001B3ull ^
              (uint64_t)(prim | (i32 ? 64u : 0u) | (restart ? 128u : 0u) | (dma_size >> 30) << 8) << 50 ^ (uint64_t)reg(kVgtMultiPrimIbResetIndx) * 31;
    if (auto it = g.idx_cache.find(idx_key); it != g.idx_cache.end()) {
      idx_va = it->second.va, host_count = it->second.count, topo = it->second.topo, st.topology = it->second.type;
    }
  }
  if (!idx_va) {
    std::vector<uint32_t> gi(num);
    if (source == 0) {
      const uint8_t* p = physical(dma_base & ~(i32 ? 3u : 1u));
      uint32_t endian = dma_size >> 30;
      for (uint32_t i = 0; i < num; i++) gi[i] = guest_index(p, i32, endian, i);
    } else if (source == 2) {
      for (uint32_t i = 0; i < num; i++) gi[i] = i;
    } else {
      for (uint32_t i = 0; i < num; i++) {
        uint32_t w = i32 ? i : i / 2;
        uint32_t v = w < immediate_count ? immediate[w] : 0;
        gi[i] = i32 ? v : ((i & 1) ? v >> 16 : v & 0xFFFF);
      }
    }
    if (!expand(prim, gi, restart, reg(kVgtMultiPrimIbResetIndx), host, topo, st.topology) || host.empty()) {
      warn_once("prim" + std::to_string(prim), "[d3d12] primitiva 0x%X não suportada\n", prim);
      g.skipped++;
      return;
    }
    host_count = (uint32_t)host.size();
  }
  pf_idx.stop();
  Prof pf_setup(kProfSetup);
  // Render targets
  uint32_t surface = reg(kRbSurfaceInfo), pitch = surface & 0x3FFF;
  uint32_t br = reg(kPaScWindowScissorBr), tl = reg(kPaScWindowScissorTl);
  uint32_t min_h = (br >> 16) & 0x3FFF;
  st.color_mask = reg(kRbColorMask);
  D3D12_CPU_DESCRIPTOR_HANDLE rtvs[4];
  float rt_scale[4] = {1, 1, 1, 1};
  Texture* first = nullptr;
  SysConstants sys{};
  if (mode == 4) {
    for (uint32_t i = 0; i < 4; i++) {
      if (!(ps->info.color_exports & (1u << i))) continue;
      uint32_t info = reg(i == 0 ? kRbColorInfo : kRbColor1Info + i - 1);
      Texture* t = render_target(info & 0xFFF, (info >> 16) & 0xF, pitch, false, min_h);
      if (!t) continue;
      transition(*t, D3D12_RESOURCE_STATE_RENDER_TARGET);
      rtvs[st.num_rt] = t->rtv;
      st.rt[st.num_rt] = t->format;
      st.blend[st.num_rt] = reg(i == 0 ? kRbBlendControl0 : kRbBlendControl1 + i - 1);
      rt_scale[i] = std::ldexp(1.0f, (int32_t)(info << 6) >> 26);
      const uint32_t xf = (info >> 16) & 0xF;
      sys.rt_max[i] = xf == 3 || xf == 12 ? 31.875f : 0.0f;
      st.num_rt++;
      if (!first) first = t;
    }
  }
  static const bool no_cull = std::getenv("GOW_NO_CULL") != nullptr;
  uint32_t mode_cntl_cull = reg(kPaSuScModeCntl) & 7;
  if ((mode_cntl_cull & 3) == 3 && st.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) return;  // tudo descartado
  // Rect lists (0x08) são primitivas de ecrã: sem culling. Com ele, o restauro de profundidade dos passes finais
  // (retângulo de ecrã inteiro, PS com oDepth) era descartado e o teste de profundidade falhava no mundo.
  st.cull = (no_cull || prim == 8 || st.topology != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE) ? 0 : mode_cntl_cull;
  // GOW_DEBUG_N=a,b: desenhos com esses números de índices sem culling nem teste de profundidade.
  static const std::set<uint32_t> debug_n = [] {
    std::set<uint32_t> o;
    if (const char* e = std::getenv("GOW_DEBUG_N"))
      for (const char* p = e; *p; p = std::strchr(p, ',') ? std::strchr(p, ',') + 1 : "") o.insert((uint32_t)std::atoi(p));
    return o;
  }();
  st.depth_control = reg(kRbDepthControl);
  st.stencil = reg(kRbStencilRefMask);
  // Desvio de profundidade (mapas de sombra, decalques). Como no Xenia: o declive está em 1/16 de subpíxel, e o desvio é
  // uma fração da gama de profundidade. Só há um desvio por PSO: o da frente, ou o de trás se só esse estiver ligado.
  if (const uint32_t sc = reg(kPaSuScModeCntl); st.topology == D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE && (sc & (3u << 11))) {
    const uint32_t r = (sc & (1u << 11)) ? kPaSuPolyOffsetFrontScale : kPaSuPolyOffsetFrontScale + 2;
    const float slope = regf(r) / 16.0f * g.scale;
    st.depth_bias = (int32_t)std::lround(regf(r + 1) * 16777216.0f);
    std::memcpy(&st.slope_bits, &slope, 4);
  }
  if (debug_n.count(num)) st.cull = 0, st.depth_control = 0;
  Texture* depth = nullptr;
  if (st.depth_control & 7) {
    uint32_t info = reg(kRbDepthInfo);
    depth = render_target(info & 0xFFF, 0, pitch, true, min_h);
    if (depth) {
      transition(*depth, D3D12_RESOURCE_STATE_DEPTH_WRITE);
      st.dsv = DXGI_FORMAT_D24_UNORM_S8_UINT;
      if (!first) first = depth;
    }
  }
  if (!first) {
    g.skipped++;
    return;
  }
  ID3D12PipelineState* pso = pipeline(vs, ps, st);
  if (!pso) {
    g.skipped++;
    return;
  }
  const uint32_t* cr = constant_registers();
  for (int i = 0; i < 3; i++) {
    sys.vp_scale[i] = regf(kPaClVportXscale + 2 * i);
    sys.vp_offset[i] = regf(kPaClVportXscale + 2 * i + 1);
  }
  sys.rt[0] = 2.0f * g.scale / first->width;  // tamanho lógico do RT
  sys.rt[1] = 2.0f * g.scale / first->height;
  sys.res[0] = sys.res[1] = 1.0f / g.scale;
  uint32_t mode_cntl = reg(kPaSuScModeCntl);
  if ((mode_cntl >> 16) & 1) {
    uint32_t wo = reg(kPaScWindowOffset);
    sys.rt[2] = (float)((int32_t)(wo << 17) >> 17);
    sys.rt[3] = (float)((int32_t)((wo >> 16) << 17) >> 17);
  }
  sys.flags[0] = reg(kPaClVteCntl);
  uint32_t colorcontrol = reg(kRbColorControl);
  sys.flags[1] = colorcontrol & 0xF;
  uint32_t pc = reg(kSqProgramCntl);
  sys.flags[2] = ((pc >> 18) & 1) ? (0x100u | ((reg(kSqContextMisc) >> 8) & 0x3F)) : 0;
  sys.flags[3] = reg(kVgtIndxOffset);
  sys.alpha_ref[0] = regf(kRbAlphaRef);
  std::memcpy(sys.rt_scale, rt_scale, sizeof rt_scale);
  pf_setup.stop();
  Prof pf_desc(kProfDesc);
  // Texturas e samplers (PS)
  Texture* slot_tex[32];
  D3D12_SAMPLER_DESC slot_smp[32];
  // Só os slots que o PS amostra: os restantes descritores da tabela nunca são lidos (32 SRVs + 32 samplers por
  // desenho custavam vários ms/frame).
  const uint32_t used_tex = ps ? ps->info.texture_mask : 0;
  for (uint32_t k = 0; k < 32; k++) {
    if (!(used_tex & (1u << k))) continue;
    Texture* t = &g.dummy;
    TextureFetch tf{};
    if (ps && (ps->info.texture_mask & (1u << k))) {
      std::memcpy(tf.dword, cr + (kConstFetch - 0x4000) + k * 6, 24);
      // O tipo tem de bater com a declaração do shader (Texture2D / TextureCube).
      const bool want_cube = (ps->info.cube_mask >> k) & 1;
      if (want_cube) t = (tf.dword[0] & 3) == 2 && tf.dimension() == 3 ? cube_texture_for(tf) : &g.dummy_cube;
      else if ((tf.dword[0] & 3) == 2 && tf.dimension() != 3) t = texture_for(tf);
      sys.tex_size[k][0] = (float)tf.width();
      sys.tex_size[k][1] = (float)tf.height();
      // UV: 1 em texturas da memória; numa cópia de resolve, a parte que o fetch descreve.
      const bool from_resolve = t->guest_pitch != 0;
      sys.tex_size[k][2] = from_resolve ? tf.width() * g.scale / t->width : 1.0f;
      sys.tex_size[k][3] = from_resolve ? tf.height() * g.scale / t->height : 1.0f;
    }
    transition(*t, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    slot_tex[k] = t;
    D3D12_SAMPLER_DESC sd{};
    // Filtro da instrução tfetch, ou o da constante de fetch se a instrução o deixar a ela (3).
    const uint8_t ov = ps->info.tex_filter[k];
    const uint32_t mag = (ov & 3) != 3 ? ov & 3 : tf.mag_filter(), min = ((ov >> 2) & 3) != 3 ? (ov >> 2) & 3 : tf.min_filter(),
                   mip = ((ov >> 4) & 3) != 3 ? (ov >> 4) & 3 : tf.mip_filter();
    sd.Filter = (D3D12_FILTER)((min == 1 ? 0x10 : 0) | (mag == 1 ? 0x04 : 0) | (mip == 1 ? 0x01 : 0));
    sd.AddressU = address_mode(tf.clamp_x());
    sd.AddressV = address_mode(tf.clamp_y());
    sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX;
    slot_smp[k] = sd;
  }
  // Desenhos seguidos usam quase sempre os mesmos materiais: reaproveitam-se as tabelas de descritores deste submit. Os
  // samplers têm chave própria porque o heap só leva 1024 por slot (32 tabelas), e havia 10 submits por frame só por isso;
  // as combinações de filtro e endereçamento são poucas.
  uint64_t srv_key = used_tex, smp_key = used_tex;
  for (uint32_t k = 0; k < 32; k++) {
    if (!(used_tex & (1u << k))) continue;
    const D3D12_SAMPLER_DESC& sd = slot_smp[k];
    srv_key = srv_key * 0x100000001B3ull ^ (uint64_t)(uintptr_t)slot_tex[k]->res;
    smp_key = smp_key * 0x100000001B3ull ^ (uint64_t)sd.Filter ^ (uint64_t)sd.AddressU << 12 ^ (uint64_t)sd.AddressV << 18;
  }
  uint32_t srv_base, smp_base;
  if (auto it = g.srv_tables.find(srv_key); it != g.srv_tables.end()) {
    srv_base = it->second;
  } else {
    srv_base = srv_alloc(32);
    for (uint32_t k = 0; k < 32; k++)
      if (used_tex & (1u << k)) write_srv(*slot_tex[k], srv_base + k);  // a swizzle da constante de fetch é aplicada no shader (xe_tswz)
    g.srv_tables[srv_key] = srv_base;
  }
  if (auto it = g.smp_tables.find(smp_key); it != g.smp_tables.end()) {
    smp_base = it->second;
  } else {
    // Só até ao último slot usado: os seguintes da tabela nunca são lidos, e o heap é pequeno.
    smp_base = sampler_alloc(used_tex ? 32 - (uint32_t)__builtin_clz(used_tex) : 1);
    for (uint32_t k = 0; k < 32; k++)
      if (used_tex & (1u << k)) g.device->CreateSampler(&slot_smp[k], cpu_handle(g.sampler_heap, g.sampler_inc, smp_base + k));
    g.smp_tables[smp_key] = smp_base;
  }
  if (g.dummy.state != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) transition(g.dummy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  // Buffers de vértices
  pf_desc.stop();
  D3D12_GPU_VIRTUAL_ADDRESS vbs[8] = {};
  Prof pf_vb(kProfVertex);
  for (size_t i = 0; i < vs->info.vfetch_slots.size() && i < 8; i++) {
    uint32_t slot = vs->info.vfetch_slots[i];
    const uint32_t* f = cr + (kConstFetch - 0x4000) + slot * 2;
    uint32_t addr = f[0] & ~3u, size = ((f[1] >> 2) & 0xFFFFFF) * 4;
    size = std::min<uint32_t>(size, 64u << 20);
    if (gpu_physical(addr) + (uint64_t)size > 0x20000000u) size = 0x20000000u - gpu_physical(addr);
    static const bool no_vb_cache = std::getenv("GOW_NO_VB_CACHE") != nullptr;
    uint64_t vkey = (uint64_t)gpu_physical(addr) << 32 | size;
    // O mesmo endereço pode ter vértices novos no mesmo submit (anéis dinâmicos da UE3): só se reutiliza a cópia se a
    // memória convidada não tiver mudado. Amostra de início, meio e fim: um hash completo por desenho custava 14 ms/frame
    // (buffers estáticos de vários MB partilhados por centenas de desenhos).
    uint64_t vhash = 0;
    if (size) {
      const uint8_t* vp = physical(addr);
      const uint32_t n = std::min<uint32_t>(size, 512);
      vhash = hash_bytes(vp, n) ^ hash_bytes(vp + (size - n) / 2, n) * 3 ^ hash_bytes(vp + size - n, n) * 5;
    }
    auto it = g.vb_cache.find(vkey);
    if (!no_vb_cache && it != g.vb_cache.end() && it->second.hash == vhash) {
      vbs[i] = it->second.va;
    } else {
      if (it != g.vb_cache.end()) g.vb_stale++;
      uint8_t* p = upload_alloc(std::max<uint32_t>(size, 16), 256, &vbs[i]);
      if (p && size) std::memcpy(p, physical(addr), size);
      if (p) g.vb_cache[vkey] = {vbs[i], vhash};
    }
    sys.vb_size[i] = size;
  }
  pf_vb.stop();
  Prof pf_const(kProfConst);
  // Constantes (depois das texturas e vértices: nada abaixo pode submeter)
  uint32_t vs_base = reg(kSqVsConst) & 0x1FF, ps_base = reg(kSqPsConst) & 0x1FF;
  if (vs_base > 256) vs_base = 0;
  if (ps_base > 256) ps_base = 256;
  D3D12_GPU_VIRTUAL_ADDRESS cb_vs = upload_alu(cr + vs_base * 4, vs->info.const_used);
  D3D12_GPU_VIRTUAL_ADDRESS cb_ps = ps ? upload_alu(cr + ps_base * 4, ps->info.const_used) : upload_cb(cr + ps_base * 4, 256 * 16);
  D3D12_GPU_VIRTUAL_ADDRESS cb_fetch = upload_cb(cr + (kConstFetch - 0x4000), 192 * 4);
  uint32_t bl[40];
  std::memcpy(bl, cr + (kConstBool - 0x4000), 8 * 4);
  std::memcpy(bl + 8, cr + (kConstLoop - 0x4000), 32 * 4);
  D3D12_GPU_VIRTUAL_ADDRESS cb_bools = upload_cb(bl, sizeof bl);
  D3D12_GPU_VIRTUAL_ADDRESS cb_sys = upload_cb(&sys, sizeof sys);
  if (!idx_va) {
    uint8_t* ip = upload_alloc(host.size() * 4, 256, &idx_va);
    if (!ip) return;
    std::memcpy(ip, host.data(), host.size() * 4);
    if (idx_key) g.idx_cache[idx_key] = {idx_va, host_count, topo, st.topology};
  }
  pf_const.stop();
  Prof pf_rec(kProfRecord);
  // Gravação
  if (g.bound_root != g.root) g.list->SetGraphicsRootSignature(g.root), g.bound_root = g.root;
  if (g.bound_pso != pso) g.list->SetPipelineState(pso), g.bound_pso = pso;
  g.list->SetGraphicsRootConstantBufferView(0, cb_vs);
  g.list->SetGraphicsRootConstantBufferView(1, cb_ps);
  g.list->SetGraphicsRootConstantBufferView(2, cb_fetch);
  g.list->SetGraphicsRootConstantBufferView(3, cb_bools);
  g.list->SetGraphicsRootConstantBufferView(4, cb_sys);
  g.list->SetGraphicsRootShaderResourceView(5, idx_va);
  for (int i = 0; i < 8; i++) g.list->SetGraphicsRootShaderResourceView(6 + i, vbs[i] ? vbs[i] : idx_va);
  g.list->SetGraphicsRootDescriptorTable(14, gpu_handle(g.srv_heap, g.srv_inc, srv_base));
  g.list->SetGraphicsRootDescriptorTable(15, gpu_handle(g.sampler_heap, g.sampler_inc, smp_base));
  g.list->OMSetRenderTargets(st.num_rt, st.num_rt ? rtvs : nullptr, FALSE, depth ? &depth->dsv : nullptr);
  float bf[4] = {regf(kRbBlendRed), regf(kRbBlendRed + 1), regf(kRbBlendRed + 2), regf(kRbBlendRed + 3)};
  g.list->OMSetBlendFactor(bf);
  g.list->OMSetStencilRef(st.stencil & 0xFF);
  D3D12_VIEWPORT vp{0, 0, (float)first->width, (float)first->height, 0, 1};
  g.list->RSSetViewports(1, &vp);
  LONG sx0 = tl & 0x3FFF, sy0 = (tl >> 16) & 0x3FFF, sx1 = br & 0x3FFF, sy1 = (br >> 16) & 0x3FFF;
  if (!((tl >> 31) & 1)) {  // window_offset_disable = 0: o deslocamento da janela também move a tesoura
    uint32_t wo = reg(kPaScWindowOffset);
    LONG ox = (int32_t)(wo << 17) >> 17, oy = (int32_t)((wo >> 16) << 17) >> 17;
    sx0 += ox, sx1 += ox, sy0 += oy, sy1 += oy;
  }
  D3D12_RECT sc{std::max<LONG>(0, (LONG)scaled((float)sx0)), std::max<LONG>(0, (LONG)scaled((float)sy0)),
                std::min<LONG>((LONG)std::lround(sx1 * g.scale), first->width),
                std::min<LONG>((LONG)std::lround(sy1 * g.scale), first->height)};
  if (sc.right <= sc.left || sc.bottom <= sc.top) sc = {0, 0, (LONG)first->width, (LONG)first->height};
  g.list->RSSetScissorRects(1, &sc);
  g.list->IASetPrimitiveTopology(topo);
  g.list->DrawInstanced(host_count, 1, 0, 0);
  g.draws++;
  // GOW_TRACE_AFTER_RESOLVE=N: só os desenhos depois do resolve N (passes de pós-processamento), com estado.
  static const uint32_t trace_after = std::getenv("GOW_TRACE_AFTER_RESOLVE") ? (uint32_t)std::atoi(std::getenv("GOW_TRACE_AFTER_RESOLVE")) : 0;
  static const uint32_t trace_max = std::getenv("GOW_TRACE_MAX") ? (uint32_t)std::atoi(std::getenv("GOW_TRACE_MAX")) : 400;
  if (g.trace && traced_frame() && g.trace_lines < trace_max && g.resolves >= trace_after) {
    g.trace_lines++;
    if (trace_after)
      std::fprintf(stderr, "[render] estado: blend %08X máscara %08X depth %08X stencil %08X depthinfo %08X scissor (%ld,%ld)-(%ld,%ld) janela %08X cull %X%c",
                   st.blend[0], st.color_mask, st.depth_control, st.stencil, reg(kRbDepthInfo), sc.left, sc.top, sc.right, sc.bottom, reg(kPaScWindowOffset), st.cull, 10);
    std::string tex;
    for (uint32_t k = 0; k < 32; k++)
      if (ps && (ps->info.texture_mask & (1u << k))) {
        char b[48];
        std::snprintf(b, sizeof b, " t%u=0x%08X", k, gpu_physical(cr[(kConstFetch - 0x4000) + k * 6 + 1] & 0xFFFFF000u));
        tex += b;
      }
    uint32_t ci = reg(kRbColorInfo);
    std::fprintf(stderr, "[render] desenho prim %u n=%u (%zu) rt base %u fmt %u pitch %u modo %u vs %016llX ps %016llX%s\n",
                 prim, num, (size_t)host_count, ci & 0xFFF, (ci >> 16) & 0xF, pitch, mode, (unsigned long long)vs->info.hash,
                 ps ? (unsigned long long)ps->info.hash : 0ull, tex.c_str());
  }
}

void swap(uint32_t frontbuffer, uint32_t width, uint32_t height, const uint32_t fetch[6]) {
  if (!g.active) return;
  g.swaps++;
  // A janela mudou de tamanho (arrasto, Alt+Enter): GPU parada, sem referências, e ResizeBuffers.
  uint32_t cw = 0, ch = 0;
  window::client_size(cw, ch);
  if (cw && ch && (cw != g.bb_w || ch != g.bb_h)) {
    submit(true);
    submit(true);
    for (auto& b : g.backbuffers) b->Release(), b = nullptr;
    HRESULT hr = g.swapchain->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) g.bb_w = cw, g.bb_h = ch;
    else std::fprintf(stderr, "[d3d12] ResizeBuffers(%u, %u) falhou 0x%08lX\n", cw, ch, (unsigned long)hr);
    for (uint32_t i = 0; i < kBackBuffers; i++) {
      g.swapchain->GetBuffer(i, IID_PPV_ARGS(&g.backbuffers[i]));
      g.device->CreateRenderTargetView(g.backbuffers[i], nullptr, cpu_handle(g.rtv_heap, g.rtv_inc, i));
    }
    std::fprintf(stderr, "[d3d12] saída %u×%u\n", g.bb_w, g.bb_h);
  }
  uint32_t bi = g.swapchain->GetCurrentBackBufferIndex();
  ID3D12Resource* bb = g.backbuffers[bi];
  D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_PRESENT;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_handle(g.rtv_heap, g.rtv_inc, bi);
  transition(bb, bb_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
  const float black[4] = {0, 0, 0, 1};
  g.list->ClearRenderTargetView(rtv, black, 0, nullptr);
  Texture* src = nullptr;
  Texture* fb = resolved_at(gpu_physical(frontbuffer));
  if (fb) src = fb;
  else if (frontbuffer) {
    TextureFetch tf{};
    std::memcpy(tf.dword, fetch, 24);
    if ((tf.dword[0] & 3) == 2 && tf.base()) src = texture_for(tf);
  }
  if (g.trace && traced_frame()) {
    g.trace_lines++;
    std::fprintf(stderr, "[render] swap frame %llu: frontbuffer 0x%08X %u×%u fetch %08X %08X %08X %08X -> %s\n",
                 (unsigned long long)g.frame, frontbuffer, width, height, fetch[0], fetch[1], fetch[2], fetch[3],
                 fb ? "cópia de resolve" : src && src != &g.dummy ? "memória" : "nada");
  }
  if (src && src != &g.dummy) {
    // Cópia de resolve: tamanho interno; textura da memória: tamanho lógico.
    const float isc = src->guest_pitch ? g.scale : 1.0f;
    float w = width ? std::min(width * isc, (float)src->width) : (float)src->width;
    float h = height ? std::min(height * isc, (float)src->height) : (float)src->height;
    // Para a saída, com barras se a proporção não for a do jogo.
    const float k = std::min((float)g.bb_w / w, (float)g.bb_h / h);
    const float dw = std::round(w * k), dh = std::round(h * k);
    const float dx = std::floor((g.bb_w - dw) / 2), dy = std::floor((g.bb_h - dh) / 2);
    const config::Config& cfg = config::get();
    const uint32_t swz = (fetch[3] >> 1) & 0xFFF;  // swizzle da constante de fetch do frontbuffer
    if (cfg.sharpening) {
      // FSR 1.0: EASU quando a saída é maior que a imagem interna (senão filtro linear), depois RCAS.
      if (!g.fsr_tmp.res || g.fsr_tmp.width != (uint32_t)dw || g.fsr_tmp.height != (uint32_t)dh) {
        if (g.fsr_tmp.res) g.garbage.push_back(g.fsr_tmp.res), g.fsr_tmp.res = nullptr;
        create_texture(g.fsr_tmp, (uint32_t)dw, (uint32_t)dh, DXGI_FORMAT_R8G8B8A8_UNORM, true, false);
      }
      if (g.fsr_tmp.res) {
        blit(*src, g.fsr_tmp.res, g.fsr_tmp.state, g.fsr_tmp.rtv, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, w, h, 0, 0, dw, dh, true,
             dw > w || dh > h ? kColorEasu : kColor, true, 1.0f, swz);
        // con = 2^-paragens: força 1 = 0 paragens (máximo), força 0 = 2 paragens.
        const float con = std::pow(0.5f, 2.0f * (1.0f - cfg.sharpening_strength));
        blit(g.fsr_tmp, bb, bb_state, rtv, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, dw, dh, dx, dy, dw, dh, false, kColorRcas, true, con);
      }
    } else {
      blit(*src, bb, bb_state, rtv, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 0, w, h, dx, dy, dw, dh, true, kColor, true, 1.0f, swz);
    }
  }
  if (g.dump_frames.count(g.frame)) {
    char name[64];
    std::snprintf(name, sizeof name, "frames/frame_%06llu.bmp", (unsigned long long)g.frame);
    dump_rgba(bb, bb_state, name);
  }
  transition(bb, bb_state, D3D12_RESOURCE_STATE_PRESENT);
  submit();
  g.swapchain->Present(config::get().vsync ? 1 : 0, 0);
  {
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - g.last_present).count();
    g.last_present = now;
    g.stat_worst_ms = std::max(g.stat_worst_ms, ms);
    if (ms > 50.0f) g.stat_slow_frames++;
  }
  if (g.frame % 120 == 0) {
    char title_[160];
    std::snprintf(title_, sizeof title_, "Gears of War (recompilado) - frame %llu, %u desenhos, %u resolves",
                  (unsigned long long)g.frame, g.draws, g.resolves);
    window::set_title((title_ + net::status()).c_str());
    std::fprintf(stderr, "[d3d12] frame %llu: %u desenhos, %u ignorados, %u resolves, %zu shaders, %zu PSOs, %zu RTs, %llu vértices refeitos, %u submits (a meio: upload %u, SRV %u, samplers %u) e %u recursos criados em 120 frames; pior frame %.0f ms, %u acima de 50 ms\n",
                 (unsigned long long)g.frame, g.draws, g.skipped, g.resolves, g.shaders.size(), g.psos.size(),
                 g.rts.size(), (unsigned long long)g.vb_stale, g.stat_submits, g.stat_early[0], g.stat_early[1], g.stat_early[2], g.stat_creates, g.stat_worst_ms, g.stat_slow_frames);
    g.stat_worst_ms = 0;
    g.stat_slow_frames = 0;
    g.stat_submits = g.stat_creates = 0;
    g.stat_early[0] = g.stat_early[1] = g.stat_early[2] = 0;
    if (g_profile && g.frame) {
      std::string line;
      char b[64];
      for (int i = 0; i < kProfCount; i++) {
        std::snprintf(b, sizeof b, " %s %.1f", kProfNames[i], g_prof[i] / 120);
        line += b;
        g_prof[i] = 0;
      }
      std::fprintf(stderr, "[perf] ms/frame: total %.1f (%.1f fps) |%s\n", g_prof_frame_ms / 120,
                   120000.0 / std::max(1.0, g_prof_frame_ms), line.c_str());
      g_prof_frame_ms = 0;
    }
  }
  if (g_profile) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_prof_last_swap.QuadPart) g_prof_frame_ms += prof_ms(now.QuadPart - g_prof_last_swap.QuadPart);
    g_prof_last_swap = now;
  }
  g.draws = g.skipped = g.resolves = 0;
  g.shader_wait_ms = 0;
  g.frame++;
  // ResolutionScale mudou no menu F8: as superfícies são recriadas no novo tamanho a partir do próximo frame.
  if (const float want = config::get().resolution_scale; want != g.scale) {
    submit(true);
    submit(true);
    {
      auto* m = &g.rts;
      for (auto& [key, t] : *m)
        if (t.res) g.garbage.push_back(t.res);
      m->clear();
    }
    for (auto& [key, t] : g.resolved)
      if (t.res) g.garbage.push_back(t.res);
    g.resolved.clear();
    g.scale = want;
    std::fprintf(stderr, "[d3d12] resolução interna %u×%u\n", scaled(kWidth), scaled(kHeight));
  }
}

}  // namespace gpu::render
