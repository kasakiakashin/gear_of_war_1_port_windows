// printf_test.h — GOW_TEST_PRINTF=1: autoteste da formatação de floats da CRT do jogo (ver printf_test.cpp).
#pragma once

struct PPCContext;

namespace kernel {

// Corre uma vez, na thread convidada que chama (precisa da CRT do jogo já iniciada).
void run_guest_printf_test(PPCContext& ctx);

}  // namespace kernel
