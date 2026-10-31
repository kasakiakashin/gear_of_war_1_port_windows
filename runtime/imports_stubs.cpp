// Gerado por tools/gen_import_stubs.py — não editar.
// Stubs das funções importadas sem implementação real. Para implementar uma função, defina
// XBOX_IMPORT(Nome) noutro ficheiro de runtime/ e rode o gerador de novo (o stub some daqui).
// Convenção: STUBBED_IMPORT regista a chamada; o stub devolve 0 em r3 (STATUS_SUCCESS /
// ERROR_SUCCESS / NULL, conforme a função).
#include "xbox_import.h"

// ---- xboxkrnl.exe: 19 stubs ----
XBOX_IMPORT_STUB(DbgBreakPoint) { STUBBED_IMPORT("DbgBreakPoint"); ctx.r[3] = 0; }  // ordinal 0x0001
XBOX_IMPORT_STUB(ExRegisterTitleTerminateNotification) { STUBBED_IMPORT("ExRegisterTitleTerminateNotification"); ctx.r[3] = 0; }  // ordinal 0x0015
XBOX_IMPORT_STUB(FscSetCacheElementCount) { STUBBED_IMPORT("FscSetCacheElementCount"); ctx.r[3] = 0; }  // ordinal 0x0021
XBOX_IMPORT_STUB(HalReturnToFirmware) { STUBBED_IMPORT("HalReturnToFirmware"); ctx.r[3] = 0; }  // ordinal 0x0028
XBOX_IMPORT_STUB(KeBugCheck) { STUBBED_IMPORT("KeBugCheck"); ctx.r[3] = 0; }  // ordinal 0x0052
XBOX_IMPORT_STUB(KeBugCheckEx) { STUBBED_IMPORT("KeBugCheckEx"); ctx.r[3] = 0; }  // ordinal 0x0053
XBOX_IMPORT_STUB(KeLockL2) { STUBBED_IMPORT("KeLockL2"); ctx.r[3] = 0; }  // ordinal 0x006B
XBOX_IMPORT_STUB(KeUnlockL2) { STUBBED_IMPORT("KeUnlockL2"); ctx.r[3] = 0; }  // ordinal 0x006C
XBOX_IMPORT_STUB(ObCreateSymbolicLink) { STUBBED_IMPORT("ObCreateSymbolicLink"); ctx.r[3] = 0; }  // ordinal 0x0103
XBOX_IMPORT_STUB(ObDeleteSymbolicLink) { STUBBED_IMPORT("ObDeleteSymbolicLink"); ctx.r[3] = 0; }  // ordinal 0x0104
XBOX_IMPORT_STUB(RtlCaptureContext) { STUBBED_IMPORT("RtlCaptureContext"); ctx.r[3] = 0; }  // ordinal 0x0119
XBOX_IMPORT_STUB(RtlRaiseException) { STUBBED_IMPORT("RtlRaiseException"); ctx.r[3] = 0; }  // ordinal 0x0136
XBOX_IMPORT_STUB(RtlUnwind) { STUBBED_IMPORT("RtlUnwind"); ctx.r[3] = 0; }  // ordinal 0x0147
XBOX_IMPORT_STUB(XexGetProcedureAddress) { STUBBED_IMPORT("XexGetProcedureAddress"); ctx.r[3] = 0; }  // ordinal 0x0197
XBOX_IMPORT_STUB(__C_specific_handler) { STUBBED_IMPORT("__C_specific_handler"); ctx.r[3] = 0; }  // ordinal 0x01A5
XBOX_IMPORT_STUB(VdGetCurrentDisplayInformation) { STUBBED_IMPORT("VdGetCurrentDisplayInformation"); ctx.r[3] = 0; }  // ordinal 0x01BA
XBOX_IMPORT_STUB(VdInitializeScalerCommandBuffer) { STUBBED_IMPORT("VdInitializeScalerCommandBuffer"); ctx.r[3] = 0; }  // ordinal 0x01C5
XBOX_IMPORT_STUB(VdSetDisplayMode) { STUBBED_IMPORT("VdSetDisplayMode"); ctx.r[3] = 0; }  // ordinal 0x01D3
XBOX_IMPORT_STUB(KiApcNormalRoutineNop) { STUBBED_IMPORT("KiApcNormalRoutineNop"); ctx.r[3] = 0; }  // ordinal 0x01DF

// ---- xam.xex: 9 stubs ----
XBOX_IMPORT_STUB(XamGetSystemVersion) { STUBBED_IMPORT("XamGetSystemVersion"); ctx.r[3] = 0; }  // ordinal 0x0282
XBOX_IMPORT_STUB(XamWriteGamerTile) { STUBBED_IMPORT("XamWriteGamerTile"); ctx.r[3] = 0; }  // ordinal 0x02F0
XBOX_IMPORT_STUB(XamUserCreateStatsEnumerator) { STUBBED_IMPORT("XamUserCreateStatsEnumerator"); ctx.r[3] = 0; }  // ordinal 0x02F7
XBOX_IMPORT_STUB(XamVoiceCreate) { STUBBED_IMPORT("XamVoiceCreate"); ctx.r[3] = 0; }  // ordinal 0x030C
XBOX_IMPORT_STUB(XamVoiceHeadsetPresent) { STUBBED_IMPORT("XamVoiceHeadsetPresent"); ctx.r[3] = 0; }  // ordinal 0x030D
XBOX_IMPORT_STUB(XamVoiceSubmitPacket) { STUBBED_IMPORT("XamVoiceSubmitPacket"); ctx.r[3] = 0; }  // ordinal 0x030E
XBOX_IMPORT_STUB(XamVoiceClose) { STUBBED_IMPORT("XamVoiceClose"); ctx.r[3] = 0; }  // ordinal 0x030F
XBOX_IMPORT_STUB(XamSessionCreateHandle) { STUBBED_IMPORT("XamSessionCreateHandle"); ctx.r[3] = 0; }  // ordinal 0x0316
XBOX_IMPORT_STUB(XamSessionRefObjByHandle) { STUBBED_IMPORT("XamSessionRefObjByHandle"); ctx.r[3] = 0; }  // ordinal 0x0317
