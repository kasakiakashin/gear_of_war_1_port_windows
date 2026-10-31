# Gears of War (Xbox 360) — port por recompilação estática PPC -> x86_64.
#
#   cmake -G Ninja -S . -B build              (Zig/Clang por padrão; ver README.md)
#   cmake --build build                       -> build/runtime/gow.exe, build/generated_src/ppc_smoke.exe
#
# Compiladores: Zig (Clang, ABI MinGW) é o padrão quando nenhum compilador é indicado. Com MSVC ou
# clang-cl, passe -DCMAKE_CXX_COMPILER=clang-cl (o código traduzido usa __int128: cl.exe puro não serve).
cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
  set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/generated_src/toolchain/zig-toolchain.cmake")
endif()
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Debug/Release/RelWithDebInfo" FORCE)
endif()

project(gears_of_war_port CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SIMD do host para as rotinas VMX/VMX128 (escritas como laços por elemento que o Clang vetoriza).
# Contração em FMA fica desligada: o PowerPC só funde onde o binário usa fmadd/vmaddfp explícitos,
# e fundir fmul+fadd separados mudaria os resultados de ponto flutuante.
option(GOW_AVX2 "Compilar com AVX2/BMI2/F16C (x86-64-v3)" ON)
if(MSVC)
  add_compile_options(/fp:precise /EHsc)
  if(GOW_AVX2)
    add_compile_options(/arch:AVX2)
  endif()
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")  # clang-cl
    add_compile_options(/clang:-ffp-contract=off)
  endif()
else()
  add_compile_options(-ffp-contract=off)
  if(GOW_AVX2)
    add_compile_options(-mavx2 -mbmi -mbmi2 -mf16c -mlzcnt -mpopcnt)
  endif()
endif()

add_subdirectory(generated_src)
add_subdirectory(runtime)
