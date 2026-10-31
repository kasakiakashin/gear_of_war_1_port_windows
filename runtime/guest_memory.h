// guest_memory.h — espaço de endereçamento de 4 GiB do convidado (Xbox 360) e alocador de páginas.
//
// Mapa (endereços convidados), seguindo o layout do kernel do Xbox 360:
//   0x00000000-0x0000FFFF  nunca mapeado (apanha ponteiros nulos)
//   0x00010000-0x3FFFFFFF  heap virtual de páginas de 4 KiB   (NtAllocateVirtualMemory)
//   0x40000000-0x6FFFFFFF  heap virtual de páginas de 64 KiB  (NtAllocateVirtualMemory + MEM_LARGE_PAGES)
//   0x70000000-0x7DFFFFFF  pilhas das threads convidadas
//   0x7E000000-0x7EFFFFFF  objetos do runtime/kernel HLE (PCR, KTHREAD, eventos, variáveis exportadas)
//   0x7FC80000-0x7FC8FFFF  MMIO: registradores da GPU Xenos (registrador N em +4N)
//   0x7FEA0000-0x7FEAFFFF  MMIO: registradores do decodificador de áudio XMA
//   0x82000000-0x82CDFFFF  imagem do default.xex
//   0xA0000000-0xBFFFFFFF  memória física (512 MiB), espelho de páginas de 64 KiB
//   0xC0000000-0xDFFFFFFF  memória física, espelho de páginas de 16 MiB
//   0xE0000000-0xFFFFFFFF  memória física, espelho de páginas de 4 KiB
// Os três espelhos físicos são vistas da mesma section: escrever em 0xA0001000 altera 0xE0001000.
// Toda a memória convidada é big-endian: use ppc_ld*/ppc_st* (fazem o byteswap).
#pragma once

#include <cstdint>

namespace gm {

// Constantes do kernel do Xbox 360 (tipos de alocação e proteção).
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000, kMemRelease = 0x8000,
                   kMemFree = 0x10000, kMemPrivate = 0x20000, kMemImage = 0x1000000, kMemTopDown = 0x100000,
                   kMemLargePages = 0x20000000, kMem16MBPages = 0x80000000u;
constexpr uint32_t kPageNoAccess = 0x01, kPageReadOnly = 0x02, kPageReadWrite = 0x04;

constexpr uint32_t kStackBase = 0x70000000u, kStackEnd = 0x7E000000u;
constexpr uint32_t kRuntimeBase = 0x7E000000u, kRuntimeEnd = 0x7F000000u;
constexpr uint32_t kPhysicalSize = 0x20000000u;
// Blocos MMIO: por enquanto RAM simples (escrita guardada, leitura devolve o último valor / 0).
constexpr uint32_t kGpuRegBase = 0x7FC80000u, kXmaRegBase = 0x7FEA0000u, kMmioBlockSize = 0x10000u;

struct RegionInfo {
  uint32_t base, allocation_base, allocation_protect, size, state, protect, type;
};

bool init();
uint8_t* base();
bool commit(uint32_t addr, uint32_t size);  // fora dos heaps (imagem, pilhas, área do runtime)
uint32_t alloc_runtime(uint32_t size, uint32_t align = 16);

// Memória virtual (heaps 0x00010000 e 0x40000000). Devolvem false em falta de espaço/parâmetro inválido.
bool virtual_alloc(uint32_t& addr, uint32_t& size, uint32_t type, uint32_t protect);
bool virtual_free(uint32_t& addr, uint32_t& size, uint32_t type);
bool query(uint32_t addr, RegionInfo& out);
// Memória física: devolve o endereço no espelho escolhido pela proteção (0 em falha).
uint32_t physical_alloc(uint32_t size, uint32_t protect, uint32_t min_phys, uint32_t max_phys, uint32_t align);
bool physical_free(uint32_t addr);
uint32_t query_protect(uint32_t addr);
bool set_protect(uint32_t addr, uint32_t size, uint32_t protect);
uint32_t physical_address(uint32_t addr);

uint64_t demand_commits();

}  // namespace gm
