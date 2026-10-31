// xam_ui.cpp — UI do sistema (guia, caixas de mensagem, teclado virtual, sign-in...).
//
// Não há UI: cada chamada é registada, devolve a escolha predefinida e emite XN_SYS_UI (aberta/fechada), que
// alguns títulos esperam para sair da pausa. GOW_MSGBOX_BUTTON=N força o botão escolhido nas caixas de mensagem.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "kernel/kernel.h"
#include "xam/xam.h"

using namespace kernel;
using namespace xam;

namespace {

void log_ui(const char* name) {
  std::fprintf(stderr, "[xam] %s\n", name);
  system_ui_shown();
}

}  // namespace

// XamShowSigninUI(DWORD PaneCount, DWORD Flags): o utilizador 0 já tem sessão.
XBOX_IMPORT(XamShowSigninUI) {
  log_ui("XamShowSigninUI");
  broadcast(kXnSysSigninChanged, 1);
  ret(ctx, kErrorSuccess);
}

// XamShowMessageBoxUIEx(DWORD UserIndex, PWSTR Title, PWSTR Text, DWORD ButtonCount, PWSTR* Buttons,
//                       DWORD FocusButton, DWORD Flags, PDWORD Result, PXOVERLAPPED [pilha])
XBOX_IMPORT(XamShowMessageBoxUIEx) {
  uint32_t buttons = arg(ctx, 4), count = arg(ctx, 3), result_ptr = arg(ctx, 7);
  uint32_t choice = arg(ctx, 5);
  if (const char* e = std::getenv("GOW_MSGBOX_BUTTON")) choice = (uint32_t)std::atoi(e);
  std::fprintf(stderr, "[xam] XamShowMessageBoxUIEx: \"%s\" — \"%s\"\n", read_utf16(mem_base, arg(ctx, 1)).c_str(),
               read_utf16(mem_base, arg(ctx, 2)).c_str());
  for (uint32_t i = 0; i < count && buttons; i++)
    std::fprintf(stderr, "[xam]   [%u]%s %s\n", i, i == choice ? "*" : " ",
                 read_utf16(mem_base, ppc_ld32(mem_base, buttons + 4 * i)).c_str());
  system_ui_shown();
  out32(mem_base, result_ptr, choice);
  ret(ctx, finish(mem_base, stack_arg(ctx, mem_base, 8), kErrorSuccess));
}

// XamShowKeyboardUI(DWORD UserIndex, DWORD Flags, PWSTR Default, PWSTR Title, PWSTR Description,
//                   PWSTR Result, DWORD ResultLength, PXOVERLAPPED): devolve o texto predefinido.
XBOX_IMPORT(XamShowKeyboardUI) {
  std::string text = read_utf16(mem_base, arg(ctx, 2));
  std::fprintf(stderr, "[xam] XamShowKeyboardUI: \"%s\" -> \"%s\"\n", read_utf16(mem_base, arg(ctx, 3)).c_str(),
               text.c_str());
  system_ui_shown();
  write_utf16(mem_base, arg(ctx, 5), text, arg(ctx, 6));
  ret(ctx, finish(mem_base, arg(ctx, 7), kErrorSuccess));
}

// Painéis do guia sem resultado: abrem e fecham.
XBOX_IMPORT(XamShowFriendsUI) { log_ui("XamShowFriendsUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowMessagesUI) { log_ui("XamShowMessagesUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowAchievementsUI) { log_ui("XamShowAchievementsUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowPlayerReviewUI) { log_ui("XamShowPlayerReviewUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowMarketplaceUI) { log_ui("XamShowMarketplaceUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowPlayersUI) { log_ui("XamShowPlayersUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowGameInviteUI) { log_ui("XamShowGameInviteUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowFriendRequestUI) { log_ui("XamShowFriendRequestUI"), ret(ctx, kErrorSuccess); }
XBOX_IMPORT(XamShowGamerCardUIForXUID) { log_ui("XamShowGamerCardUIForXUID"), ret(ctx, kErrorSuccess); }
