// kernel.h — constantes e utilitários partilhados pelas implementações HLE de xboxkrnl/xam.
//
// ENDIANNESS: toda a memória convidada é big-endian. Ler/escrever SEMPRE com ppc_ld*/ppc_st* (ou os
// atómicos abaixo), nunca com memcpy/ponteiros host diretos.
#pragma once

#include <atomic>
#include <cstdint>

#include "ppc_context.h"
#include "xbox_import.h"

namespace kernel {

constexpr uint32_t kStatusSuccess = 0x00000000, kStatusAbandonedWait0 = 0x00000080,
                   kStatusUserApc = 0x000000C0, kStatusAlerted = 0x00000101, kStatusTimeout = 0x00000102,
                   kStatusObjectNameExists = 0x40000000, kStatusInvalidHandle = 0xC0000008,
                   kStatusInvalidParameter = 0xC000000D, kStatusNoMemory = 0xC0000017,
                   kStatusConflictingAddresses = 0xC0000018, kStatusObjectTypeMismatch = 0xC0000024,
                   kStatusMutantNotOwned = 0xC0000046, kStatusSemaphoreLimitExceeded = 0xC0000047,
                   kStatusMemoryNotAllocated = 0xC00000A0, kStatusNotImplemented = 0xC0000002,
                   kStatusInvalidInfoClass = 0xC0000003, kStatusInfoLengthMismatch = 0xC0000004,
                   kStatusNoSuchFile = 0xC000000F, kStatusEndOfFile = 0xC0000011,
                   kStatusAccessDenied = 0xC0000022, kStatusBufferTooSmall = 0xC0000023,
                   kStatusObjectNameInvalid = 0xC0000033, kStatusObjectNameNotFound = 0xC0000034,
                   kStatusObjectNameCollision = 0xC0000035, kStatusObjectPathNotFound = 0xC000003A,
                   kStatusMediaWriteProtected = 0xC00000A2, kStatusFileIsADirectory = 0xC00000BA,
                   kStatusNotADirectory = 0xC0000103, kStatusDllNotFound = 0xC0000135,
                   kStatusNotFound = 0xC0000225, kStatusNoMoreFiles = 0x80000006;

// i-ésimo argumento inteiro (r3 + i) como endereço/valor de 32 bits.
inline uint32_t arg(const PPCContext& ctx, int i) { return (uint32_t)ctx.r[3 + i]; }

// Argumentos 9+ (i >= 8) vão na pilha do chamador: r1 + 0x54 + (i - 8) * 8 (metade baixa do slot de 64 bits).
inline uint32_t stack_arg(const PPCContext& ctx, uint8_t* m, int i) {
  return ppc_ld32(m, (uint32_t)ctx.r[1] + 0x54 + (i - 8) * 8);
}

// Retorno em r3. O ABI estende valores de 32 bits pelo tipo: DWORD/ponteiros/handles com zeros (`ret`),
// NTSTATUS/LONG com sinal (`ret_status`). O código do jogo testa NTSTATUS com `mr. rX,r3` (comparação de 64 bits):
// um 0xC0000034 estendido com zeros seria positivo e a falha passaria por sucesso.
inline void ret(PPCContext& ctx, uint64_t v) { ctx.r[3] = v; }
inline void ret_status(PPCContext& ctx, uint32_t v) { ctx.r[3] = (uint64_t)(int64_t)(int32_t)v; }

// Escrita condicional em ponteiro de saída opcional.
inline void out32(uint8_t* m, uint32_t ptr, uint32_t v) {
  if (ptr) ppc_st32(m, ptr, v);
}

// Atómicos sobre inteiros big-endian na memória convidada (compatíveis com lwarx/stwcx. traduzidos,
// que fazem CAS sobre o valor bruto).
inline int32_t atomic_add32(uint8_t* m, uint32_t ea, int32_t delta) {
  std::atomic_ref<uint32_t> ref(*reinterpret_cast<uint32_t*>(PPC_HOST(m, ea)));
  uint32_t raw = ref.load();
  for (;;) {
    int32_t v = (int32_t)ppc_bswap(raw) + delta;
    if (ref.compare_exchange_weak(raw, ppc_bswap((uint32_t)v))) return v;
  }
}
inline bool atomic_cas32(uint8_t* m, uint32_t ea, uint32_t expected, uint32_t desired) {
  std::atomic_ref<uint32_t> ref(*reinterpret_cast<uint32_t*>(PPC_HOST(m, ea)));
  uint32_t e = ppc_bswap(expected);
  return ref.compare_exchange_strong(e, ppc_bswap(desired));
}

// FILETIME atual (100 ns desde 1601-01-01) e contador monotónico em 100 ns.
uint64_t system_time();
uint64_t interrupt_time();

}  // namespace kernel
