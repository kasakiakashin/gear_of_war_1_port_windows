// memory.cpp — imports de memória virtual/física e pools (xboxkrnl).
// Ponteiros de entrada/saída (BaseAddress, RegionSize, MEMORY_BASIC_INFORMATION) são BE na memória convidada.
#include <cstdio>

#include "guest_memory.h"
#include "kernel/kernel.h"

using namespace kernel;

// NtAllocateVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect, BOOLEAN Debug)
XBOX_IMPORT(NtAllocateVirtualMemory) {
  uint32_t base_ptr = arg(ctx, 0), size_ptr = arg(ctx, 1), type = arg(ctx, 2), protect = arg(ctx, 3);
  uint32_t addr = ppc_ld32(mem_base, base_ptr), size = ppc_ld32(mem_base, size_ptr);
  if (!size || !(type & (gm::kMemCommit | gm::kMemReserve))) return ret_status(ctx, kStatusInvalidParameter);
  if (!gm::virtual_alloc(addr, size, type, protect)) {
    std::fprintf(stderr, "[mem] NtAllocateVirtualMemory falhou: base=0x%08X size=0x%X type=0x%X\n",
                 ppc_ld32(mem_base, base_ptr), ppc_ld32(mem_base, size_ptr), type);
    return ret_status(ctx, addr ? kStatusConflictingAddresses : kStatusNoMemory);
  }
  ppc_st32(mem_base, base_ptr, addr);
  ppc_st32(mem_base, size_ptr, size);
  ret_status(ctx, kStatusSuccess);
}

// NtFreeVirtualMemory(PVOID* BaseAddress, PSIZE_T RegionSize, ULONG FreeType, BOOLEAN Debug)
XBOX_IMPORT(NtFreeVirtualMemory) {
  uint32_t base_ptr = arg(ctx, 0), size_ptr = arg(ctx, 1);
  uint32_t addr = ppc_ld32(mem_base, base_ptr), size = size_ptr ? ppc_ld32(mem_base, size_ptr) : 0;
  if (!gm::virtual_free(addr, size, arg(ctx, 2))) return ret_status(ctx, kStatusMemoryNotAllocated);
  ppc_st32(mem_base, base_ptr, addr);
  out32(mem_base, size_ptr, size);
  ret_status(ctx, kStatusSuccess);
}

// NtQueryVirtualMemory(PVOID BaseAddress, PMEMORY_BASIC_INFORMATION Info) — 7 × u32 BE
XBOX_IMPORT(NtQueryVirtualMemory) {
  gm::RegionInfo r;
  if (!gm::query(arg(ctx, 0), r)) return ret_status(ctx, kStatusInvalidParameter);
  uint32_t p = arg(ctx, 1);
  const uint32_t f[7] = {r.base, r.allocation_base, r.allocation_protect, r.size, r.state, r.protect, r.type};
  for (int i = 0; i < 7; i++) ppc_st32(mem_base, p + 4 * i, f[i]);
  ret_status(ctx, kStatusSuccess);
}

// MmAllocatePhysicalMemoryEx(Flags, SIZE_T Size, Protect, MinAddress, MaxAddress, Alignment) -> PVOID
XBOX_IMPORT(MmAllocatePhysicalMemoryEx) {
  uint32_t p = gm::physical_alloc(arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4), arg(ctx, 5));
  if (!p) std::fprintf(stderr, "[mem] MmAllocatePhysicalMemoryEx falhou: size=0x%X protect=0x%X\n", arg(ctx, 1),
                       arg(ctx, 2));
  ret(ctx, p);
}
// MmAllocatePhysicalMemory(Flags, Size, Protect) — não importado por este jogo; mantido por completude.
XBOX_IMPORT(MmAllocatePhysicalMemory) { ret(ctx, gm::physical_alloc(arg(ctx, 1), arg(ctx, 2), 0, 0, 0)); }
XBOX_IMPORT(MmFreePhysicalMemory) { gm::physical_free(arg(ctx, 1)); }  // (Type, Address)
XBOX_IMPORT(MmQueryAddressProtect) { ret(ctx, gm::query_protect(arg(ctx, 0))); }
XBOX_IMPORT(MmSetAddressProtect) { gm::set_protect(arg(ctx, 0), arg(ctx, 1), arg(ctx, 2)); }
XBOX_IMPORT(MmGetPhysicalAddress) { ret(ctx, gm::physical_address(arg(ctx, 0))); }

// ExAllocatePool(Size) / ExFreePool(P): páginas de 4 KiB do heap virtual.
XBOX_IMPORT(ExAllocatePool) {
  uint32_t addr = 0, size = arg(ctx, 0);
  ret(ctx, gm::virtual_alloc(addr, size, gm::kMemReserve | gm::kMemCommit, gm::kPageReadWrite) ? addr : 0);
}
XBOX_IMPORT(ExFreePool) {
  uint32_t addr = arg(ctx, 0), size = 0;
  gm::virtual_free(addr, size, gm::kMemRelease);
}

// XamAlloc(DWORD Flags, DWORD Size, PVOID* Out) / XamFree(PVOID): mesmo heap de 4 KiB do ExAllocatePool.
XBOX_IMPORT(XamAlloc) {
  uint32_t addr = 0, size = arg(ctx, 1) ? arg(ctx, 1) : 1;
  bool ok = gm::virtual_alloc(addr, size, gm::kMemReserve | gm::kMemCommit, gm::kPageReadWrite);
  out32(mem_base, arg(ctx, 2), ok ? addr : 0);
  ret(ctx, ok ? 0 : 0x8007000Eu);  // E_OUTOFMEMORY
}
XBOX_IMPORT(XamFree) {
  uint32_t addr = arg(ctx, 0), size = 0;
  if (addr) gm::virtual_free(addr, size, gm::kMemRelease);
  ret(ctx, 0);
}

// MmQueryStatistics(MM_STATISTICS*): 26 DWORDs (Length = 0x68 na entrada). Valores aproximados de um título
// típico com 512 MiB: o UE3 usa TitleAvailablePages para dimensionar caches.
XBOX_IMPORT(MmQueryStatistics) {
  uint32_t p = arg(ctx, 0);
  if (!p || ppc_ld32(mem_base, p) != 0x68) return ret_status(ctx, kStatusInvalidParameter);
  const uint32_t total = gm::kPhysicalSize >> 12;
  const uint32_t v[26] = {0x68,        total,      0x400,  total - 0x8000, 0x80000000u, 0x10000000u, 0x4000,
                          0x100,       0x200,      0xCE0,  0x2000,         0x1000,      0x40,        0x80,
                          0x2000,      0x10000000, 0,      0x2000,         0x100,       0x40,        0x400,
                          0x200,       0x100,      0x20,   0x40,           total - 1};
  for (uint32_t i = 0; i < 26; i++) ppc_st32(mem_base, p + 4 * i, v[i]);
  ret_status(ctx, kStatusSuccess);
}
