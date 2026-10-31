// time_bundle.h — relógios do kernel partilhados entre o loader, os imports e ppc_mftb.
#pragma once

#include <cstdint>

namespace kernel {

constexpr uint64_t kTimebaseFrequency = 50000000;  // 50 MHz (mftb e KeQueryPerformanceFrequency)

// Inicia a thread que mantém o KeTimeStampBundle exportado atualizado.
void start_time_stamp_bundle(uint32_t bundle);

}  // namespace kernel
