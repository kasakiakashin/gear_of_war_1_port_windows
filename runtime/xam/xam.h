// xam.h — utilitários partilhados pelo HLE do xam.xex (erros Win32, XOVERLAPPED, notificações, utilizadores).
//
// As funções do XAM devolvem códigos de erro Win32 (DWORD, `ret`), não NTSTATUS. As assíncronas recebem um
// XOVERLAPPED opcional; aqui completam sempre de imediato (resultado escrito + evento sinalizado) e devolvem
// ERROR_IO_PENDING, como o XAM real faz quando há XOVERLAPPED.
#pragma once

#include <cstdint>
#include <string>

#include "ppc_context.h"

namespace xam {

constexpr uint32_t kErrorSuccess = 0, kErrorFileNotFound = 2, kErrorPathNotFound = 3, kErrorAccessDenied = 5,
                   kErrorInvalidHandle = 6, kErrorNoMoreFiles = 0x12, kErrorInvalidParameter = 0x57,
                   kErrorInsufficientBuffer = 0x7A, kErrorAlreadyExists = 0xB7, kErrorIoPending = 0x3E5,
                   kErrorDeviceNotConnected = 0x48F, kErrorNotFound = 0x490, kErrorCancelled = 0x4C7,
                   kErrorNoSuchUser = 0x525, kErrorFunctionFailed = 0x65B;

constexpr uint32_t kTitleId = 0x4D5307D5;  // Gears of War (nome do recurso do XEX)
constexpr uint32_t kUserCount = 4;

// XOVERLAPPED (0x1C bytes BE): +0 InternalLow (resultado), +4 InternalHigh (tamanho), +8 InternalContext,
// +0xC hEvent, +0x10 pCompletionRoutine, +0x14 dwCompletionContext, +0x18 dwExtendedError.
void complete_overlapped(uint8_t* m, uint32_t overlapped, uint32_t result, uint32_t length = 0);
// Com XOVERLAPPED: completa-o e devolve ERROR_IO_PENDING; sem: devolve `result`.
uint32_t finish(uint8_t* m, uint32_t overlapped, uint32_t result, uint32_t length = 0);

// Enumerador vazio para XamEnumerate (amigos, estatísticas… sem Live): devolve o handle.
uint32_t create_empty_enumerator(uint32_t item_size, uint32_t per_call);

// Notificações XN_* (área = id >> 25; o listener recebe se mask & (1 << área)).
constexpr uint32_t kXnSysUi = 0x00000009, kXnSysSigninChanged = 0x0000000A,
                   kXnSysStorageDevicesChanged = 0x0000000B, kXnSysProfileSettingChanged = 0x0000000E,
                   kXnSysInputDevicesChanged = 0x00000012, kXnLiveConnectionChanged = 0x02000001;
void broadcast(uint32_t id, uint32_t param);
// UI do sistema aberta e fechada logo a seguir (XN_SYS_UI 1, 0).
void system_ui_shown();

// Perfis: só o utilizador 0 tem sessão iniciada (local, sem Live).
bool signed_in(uint32_t user_index);
uint64_t xuid(uint32_t user_index);  // 0 se não houver sessão
const std::string& gamertag();

// Texto UTF-16 BE do convidado <-> UTF-8 (para registo e caminhos).
std::string read_utf16(uint8_t* m, uint32_t p, size_t max_chars = 4096);
void write_utf16(uint8_t* m, uint32_t p, const std::string& s, size_t max_chars);

}  // namespace xam
