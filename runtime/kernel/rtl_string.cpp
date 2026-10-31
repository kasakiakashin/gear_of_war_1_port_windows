// rtl_string.cpp — strings ANSI/Unicode do RTL, preenchimento de memória, conversões de tempo e status.
//   ANSI_STRING / UNICODE_STRING (8 bytes BE): +0 u16 Length (bytes), +2 u16 MaximumLength, +4 ptr Buffer.
//   Caracteres Unicode do convidado são u16 BE.
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "guest_memory.h"
#include "kernel/kernel.h"

using namespace kernel;

namespace {

constexpr uint32_t kStatusBufferOverflow = 0x80000005;

void set_string(uint8_t* m, uint32_t s, uint16_t len, uint16_t max, uint32_t buf) {
  ppc_st16(m, s + 0, len);
  ppc_st16(m, s + 2, max);
  ppc_st32(m, s + 4, buf);
}

uint32_t guest_alloc(uint32_t size) {
  uint32_t addr = 0;
  return gm::virtual_alloc(addr, size, gm::kMemReserve | gm::kMemCommit, gm::kPageReadWrite) ? addr : 0;
}

}  // namespace

XBOX_IMPORT(RtlInitAnsiString) {  // (PANSI_STRING Dest, PCSZ Source)
  uint32_t dst = arg(ctx, 0), src = arg(ctx, 1);
  uint16_t len = src ? (uint16_t)std::strlen(reinterpret_cast<const char*>(PPC_HOST(mem_base, src))) : 0;
  set_string(mem_base, dst, len, src ? len + 1 : 0, src);
}

XBOX_IMPORT(RtlInitUnicodeString) {  // (PUNICODE_STRING Dest, PCWSTR Source)
  uint32_t dst = arg(ctx, 0), src = arg(ctx, 1);
  uint16_t chars = 0;
  if (src)
    while (ppc_ld16(mem_base, src + 2 * chars)) chars++;
  set_string(mem_base, dst, chars * 2, src ? chars * 2 + 2 : 0, src);
}

XBOX_IMPORT(RtlFreeAnsiString) {  // libera o buffer alocado por RtlUnicodeStringToAnsiString
  uint32_t s = arg(ctx, 0), buf = ppc_ld32(mem_base, s + 4), size = 0;
  if (buf) gm::virtual_free(buf, size, gm::kMemRelease);
  set_string(mem_base, s, 0, 0, 0);
}

// RtlUnicodeStringToAnsiString(PANSI_STRING Dest, PCUNICODE_STRING Source, BOOLEAN AllocateDestination)
XBOX_IMPORT(RtlUnicodeStringToAnsiString) {
  uint32_t dst = arg(ctx, 0), src = arg(ctx, 1);
  uint16_t chars = ppc_ld16(mem_base, src) / 2;
  uint32_t sbuf = ppc_ld32(mem_base, src + 4), dbuf;
  if (arg(ctx, 2) & 0xFF) {
    if (!(dbuf = guest_alloc(chars + 1u))) return ret_status(ctx, kStatusNoMemory);
    set_string(mem_base, dst, chars, chars + 1, dbuf);
  } else {
    dbuf = ppc_ld32(mem_base, dst + 4);
    if (ppc_ld16(mem_base, dst + 2) < chars + 1) return ret_status(ctx, kStatusBufferOverflow);
    ppc_st16(mem_base, dst, chars);
  }
  for (uint16_t i = 0; i < chars; i++) {
    uint16_t c = ppc_ld16(mem_base, sbuf + 2 * i);
    ppc_st8(mem_base, dbuf + i, c < 0x80 ? (uint8_t)c : '?');
  }
  ppc_st8(mem_base, dbuf + chars, 0);
  ret_status(ctx, kStatusSuccess);
}

// RtlMultiByteToUnicodeN(PWCH Dest, ULONG DestBytes, PULONG ResultBytes, PCSTR Source, ULONG SourceBytes)
XBOX_IMPORT(RtlMultiByteToUnicodeN) {
  uint32_t dst = arg(ctx, 0), n = std::min(arg(ctx, 1) / 2, arg(ctx, 4)), src = arg(ctx, 3);
  for (uint32_t i = 0; i < n; i++) ppc_st16(mem_base, dst + 2 * i, ppc_ld8(mem_base, src + i));
  out32(mem_base, arg(ctx, 2), n * 2);
  ret_status(ctx, kStatusSuccess);
}

// RtlUnicodeToMultiByteN(PCHAR Dest, ULONG DestBytes, PULONG ResultBytes, PCWCH Source, ULONG SourceBytes)
XBOX_IMPORT(RtlUnicodeToMultiByteN) {
  uint32_t dst = arg(ctx, 0), n = std::min(arg(ctx, 1), arg(ctx, 4) / 2), src = arg(ctx, 3);
  for (uint32_t i = 0; i < n; i++) {
    uint16_t c = ppc_ld16(mem_base, src + 2 * i);
    ppc_st8(mem_base, dst + i, c < 0x80 ? (uint8_t)c : '?');
  }
  out32(mem_base, arg(ctx, 2), n);
  ret_status(ctx, kStatusSuccess);
}

// RtlFillMemoryUlong(PVOID Dest, SIZE_T Length, ULONG Pattern) — padrão escrito como u32 BE
XBOX_IMPORT(RtlFillMemoryUlong) {
  uint32_t dst = arg(ctx, 0), n = arg(ctx, 1) / 4, pattern = arg(ctx, 2);
  for (uint32_t i = 0; i < n; i++) ppc_st32(mem_base, dst + 4 * i, pattern);
}

// RtlCompareMemoryUlong(PVOID Source, SIZE_T Length, ULONG Pattern) -> bytes iniciais iguais ao padrão
XBOX_IMPORT(RtlCompareMemoryUlong) {
  uint32_t src = arg(ctx, 0), n = arg(ctx, 1) / 4, pattern = arg(ctx, 2), i = 0;
  while (i < n && ppc_ld32(mem_base, src + 4 * i) == pattern) i++;
  ret(ctx, i * 4);
}

// TIME_FIELDS (16 bytes BE): u16 Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday
XBOX_IMPORT(RtlTimeToTimeFields) {  // (PLARGE_INTEGER Time, PTIME_FIELDS TimeFields)
  uint64_t t = ppc_ld64(mem_base, arg(ctx, 0));
  FILETIME ft{(DWORD)t, (DWORD)(t >> 32)};
  SYSTEMTIME st{};
  FileTimeToSystemTime(&ft, &st);
  const uint16_t f[8] = {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                         st.wDayOfWeek};
  for (int i = 0; i < 8; i++) ppc_st16(mem_base, arg(ctx, 1) + 2 * i, f[i]);
}
XBOX_IMPORT(RtlTimeFieldsToTime) {  // (PTIME_FIELDS, PLARGE_INTEGER Time) -> BOOLEAN
  uint32_t tf = arg(ctx, 0);
  SYSTEMTIME st{};
  st.wYear = ppc_ld16(mem_base, tf + 0);
  st.wMonth = ppc_ld16(mem_base, tf + 2);
  st.wDay = ppc_ld16(mem_base, tf + 4);
  st.wHour = ppc_ld16(mem_base, tf + 6);
  st.wMinute = ppc_ld16(mem_base, tf + 8);
  st.wSecond = ppc_ld16(mem_base, tf + 10);
  st.wMilliseconds = ppc_ld16(mem_base, tf + 12);
  FILETIME ft;
  if (!SystemTimeToFileTime(&st, &ft)) return ret(ctx, 0);
  ppc_st64(mem_base, arg(ctx, 1), (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
  ret(ctx, 1);
}

XBOX_IMPORT(RtlNtStatusToDosError) {
  uint32_t s = arg(ctx, 0), e;
  switch (s) {
    case kStatusSuccess: e = 0; break;
    case kStatusTimeout: e = 258; break;            // WAIT_TIMEOUT
    case 0xC0000034: e = 2; break;                  // OBJECT_NAME_NOT_FOUND -> FILE_NOT_FOUND
    case 0xC000003A: e = 3; break;                  // OBJECT_PATH_NOT_FOUND -> PATH_NOT_FOUND
    case kStatusInvalidHandle: e = 6; break;
    case kStatusNoMemory: e = 8; break;
    case 0xC0000011: e = 38; break;                 // END_OF_FILE -> HANDLE_EOF
    case kStatusInvalidParameter: e = 87; break;
    case 0xC0000022: e = 5; break;                  // ACCESS_DENIED
    case 0xC0000035: e = 183; break;                // OBJECT_NAME_COLLISION -> ALREADY_EXISTS
    case 0x80000005: e = 234; break;                // BUFFER_OVERFLOW -> MORE_DATA
    case 0xC0000023: e = 122; break;                // BUFFER_TOO_SMALL -> INSUFFICIENT_BUFFER
    default: e = (s & 0xFFFF0000u) == 0x80070000u ? s & 0xFFFF : 317;  // HRESULT_FROM_WIN32 / MR_MID_NOT_FOUND
  }
  ret(ctx, e);
}

XBOX_IMPORT(KeEnableFpuExceptions) {}  // exceções de FPU ficam desligadas no host
