#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcompiler.h>

#include "gpu/shader_compile.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

#include "gpu/texture.h"
#include "kernel/vfs.h"

namespace gpu {
namespace {

pD3DCompile g_compile = nullptr;

void load() {
  static std::once_flag once;
  std::call_once(once, [] {
    if (HMODULE m = LoadLibraryA("d3dcompiler_47.dll"))
      g_compile = reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile"));
  });
}

}  // namespace

bool compiler_available() {
  load();
  return g_compile != nullptr;
}

ID3DBlob* compile_hlsl(const std::string& src, const char* target, const char* name, std::string* err) {
  load();
  if (!g_compile) return nullptr;
  static const UINT kFlags[] = {D3DCOMPILE_OPTIMIZATION_LEVEL1, D3DCOMPILE_SKIP_OPTIMIZATION, D3DCOMPILE_OPTIMIZATION_LEVEL3};
  for (UINT flags : kFlags) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = g_compile(src.data(), src.size(), name, nullptr, nullptr, "main", target, flags, 0, &code, &errors);
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

uint64_t shader_cache_key(const std::string& hlsl) {
  return hash_bytes(reinterpret_cast<const uint8_t*>(hlsl.data()), hlsl.size());
}

std::filesystem::path shader_cache_file(const std::string& hlsl) {
  char key[40];
  std::snprintf(key, sizeof key, "%016llX.dxbc", (unsigned long long)shader_cache_key(hlsl));
  return vfs::user_root() / "shader_cache" / key;
}

uint32_t microcode_size(const uint32_t* be, size_t left, bool* vertex) {
  // O fluxo de controle ocupa [0, 1.º endereço de ALU); os ENDs condicionais (4, 6, 14) não o terminam, por isso lê-se até lá.
  uint32_t first = 0xFFFFFFFFu, end = 0, cfs = 0;
  bool execs = false, uncond_end = false;
  *vertex = false;
  size_t d = 0;
  while (first == 0xFFFFFFFFu || d < (size_t)first * 3) {
    if (d + 3 > left || cfs >= 512) return 0;
    const uint32_t w0 = _byteswap_ulong(be[d]), w1 = _byteswap_ulong(be[d + 1]), w2 = _byteswap_ulong(be[d + 2]);
    const uint32_t lo[2] = {w0, (w1 >> 16) | (w2 << 16)}, hi[2] = {w1 & 0xFFFF, w2 >> 16};
    for (int i = 0; i < 2; i++, cfs++) {
      const uint32_t op = (hi[i] >> 12) & 15;
      const bool is_exec = (op >= 1 && op <= 6) || op == 13 || op == 14;
      if (is_exec) {
        const uint32_t addr = lo[i] & 0xFFF, count = (lo[i] >> 12) & 7;
        const bool last = op == 2 || op == 4 || op == 6 || op == 14;
        if (!count && !last) return 0;
        if (count) first = std::min(first, addr);
        end = std::max(end, addr + count);
        execs = true;
        uncond_end |= op == 2;
      } else if (op == 12) {
        if (((hi[i] >> 9) & 3) == 1) *vertex = true;
      }
    }
    d += 3;
    // Sem nenhuma instrução ALU (só ENDs vazios): o fluxo acaba no EXEC_END incondicional.
    if (first == 0xFFFFFFFFu && uncond_end) break;
  }
  if (!execs || end > 3000 || (first != 0xFFFFFFFFu && first * 3 != d)) return 0;
  return std::max<uint32_t>(end * 3, (uint32_t)d);
}

}  // namespace gpu
