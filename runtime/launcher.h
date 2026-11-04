// launcher.h — janela que abre antes do jogo: estado da ISO, preparação dos shaders com barra de progresso, configurações (as
// mesmas abas do F8) e o botão para começar. gow.exe --skip-launcher (ou GOW_SKIP_LAUNCHER=1) salta-a.
#pragma once

namespace launcher {

// true = o jogador carregou em Iniciar Jogo; false = fechou a janela.
bool run();

}  // namespace launcher
