# runtime/ — gow.exe: loader + kernel/XAM HLE, ligado contra o código traduzido (ppc_recomp).
# Não inclui generated_src/ppc_runtime.cpp (stub do ppc_smoke): os ganchos estão em ppc_hooks.cpp.
add_executable(gow
  main.cpp
  config.cpp
  keybinds.cpp
  language.cpp
  game_source.cpp
  launcher.cpp
  shader_prep.cpp
  guest_memory.cpp
  loader.cpp
  ppc_hooks.cpp
  import_table.cpp
  import_dispatch.cpp
  imports_stubs.cpp
  kernel/diag.cpp
  kernel/thread.cpp
  kernel/sync.cpp
  kernel/rtl.cpp
  kernel/memory.cpp
  kernel/tls_time.cpp
  kernel/threads.cpp
  kernel/xconfig.cpp
  kernel/rtl_string.cpp
  kernel/vfs.cpp
  kernel/iso.cpp
  kernel/xex_image.cpp
  kernel/file.cpp
  kernel/xex.cpp
  kernel/xam_loader.cpp
  kernel/strings.cpp
  kernel/timer.cpp
  kernel/apc.cpp
  kernel/printf_test.cpp
  xam/xam_notify.cpp
  xam/xam_user.cpp
  xam/xam_input.cpp
  xam/xam_content.cpp
  xam/xam_ui.cpp
  xam/xnet.cpp
  net/tunnel.cpp
  gpu/settings_menu.cpp
  apu/audio.cpp
  apu/audio_out.cpp
  apu/xma.cpp
  apu/xma_decoder.cpp
  gpu/video.cpp
  gpu/command_processor.cpp
  gpu/window.cpp
  gpu/xenos_shader.cpp
  gpu/shader_compile.cpp
  gpu/texture.cpp
  gpu/d3d12_renderer.cpp
)
target_include_directories(gow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gow PRIVATE ppc_recomp ws2_32 comctl32 gdi32 bcrypt comdlg32 shell32)
if(NOT MSVC)
  target_compile_options(gow PRIVATE -fwrapv -fno-strict-aliasing -Wall -Wno-unused-function)
  # Sem janela de consola; ver setup_logging em main.cpp.
  target_link_options(gow PRIVATE -Wl,--subsystem,windows)
endif()

# Áudio XMA: DLLs do FFmpeg (xma2) junto do executável, em ffmpeg/. A origem são as DLLs já instaladas nesta máquina
# (GOW_FFMPEG_SOURCE); só as dependências reais do avcodec. Sem origem, o jogo corre com o XMA em silêncio.
set(GOW_FFMPEG_SOURCE "C:/Program Files/Blackmagic Design/DaVinci Resolve" CACHE PATH "Pasta com as DLLs do FFmpeg a copiar")
set(GOW_FFMPEG_DLLS avcodec-60.dll avutil-58.dll libwinpthread-1.dll zlib1.dll VCRUNTIME140.dll)
if(EXISTS "${GOW_FFMPEG_SOURCE}/avcodec-60.dll")
  foreach(dll ${GOW_FFMPEG_DLLS})
    add_custom_command(TARGET gow POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gow>/ffmpeg"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${GOW_FFMPEG_SOURCE}/${dll}" "$<TARGET_FILE_DIR:gow>/ffmpeg/${dll}"
      VERBATIM)
  endforeach()
else()
  message(WARNING "GOW_FFMPEG_SOURCE sem avcodec-60.dll: gow.exe sem som XMA a menos que exista ffmpeg/ junto dele")
endif()
