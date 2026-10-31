// game_source.cpp — ordem de procura:
//   gow.exe x.iso ou GOW_ISO, depois IsoPath do gow_config.ini; sem extracted/ completa: *.iso e "Gears of War" nas
//   pastas de trabalho e do exe; por fim o seletor de ficheiros (guarda a escolha em IsoPath).
// A imagem base vem do argumento gow.exe x.bin, de extracted/base_image.bin, de user_data/base_image.bin ou do default.xex
// (solto ou na imagem), que na 1ª execução é descodificado e guardado nesse user_data/base_image.bin. Uma imagem de disco só é
// aceite se o seu default.xex for a versão para a qual o código foi traduzido; outra versão daria um crash incompreensível
// mais tarde.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <commdlg.h>
#include <shellapi.h>

#include "game_source.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>

#include "config.h"
#include "kernel/iso.h"
#include "kernel/vfs.h"
#include "kernel/xex_image.h"

namespace fs = std::filesystem;

namespace game_source {
namespace {

fs::path g_arg_iso, g_arg_image;

std::wstring to_wide(const std::string& s) {
  std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), (int)w.size());
  return w;
}

std::string to_utf8(const std::wstring& w) {
  std::string s(WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
  return s;
}

bool dialogs_allowed() {
  const char* e = std::getenv("GOW_NO_DIALOGS");
  return !(e && *e == '1');
}

void tell(const std::wstring& text) {
  std::fprintf(stderr, "[gow] %s\n", to_utf8(text).c_str());
  if (dialogs_allowed()) MessageBoxW(nullptr, text.c_str(), L"Gears of War", MB_OK | MB_ICONWARNING);
}

// Tudo o que é preciso para jogar sem imagem de disco; só alguns ficheiros soltos (mods) não chegam.
bool loose_game_complete() {
  std::error_code ec;
  const fs::path root = vfs::game_root();
  return fs::is_regular_file(root / "default.xex", ec) && fs::is_regular_file(root / "WarGame" / "Xbox360TOC.txt", ec);
}

bool try_iso(const fs::path& path, std::string* error) {
  if (!iso::mount(path, error)) return false;
  const iso::Node* xex = iso::find("default.xex");
  std::vector<uint8_t> data;
  if (!xex || !iso::read_all(*xex, &data)) {
    *error = "a imagem não tem default.xex";
  } else if (xex_image::sha1_hex(data.data(), data.size()) == xex_image::kExpectedXexSha1) {
    return true;
  } else {
    *error = "não é a versão suportada do Gears of War 1 (Xbox 360)";
  }
  iso::unmount();
  return false;
}

bool search_folder(const fs::path& dir) {
  std::error_code ec;
  std::vector<fs::path> candidates;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec)) continue;
    std::wstring name = e.path().filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    const std::wstring ext = e.path().extension().wstring();
    if (name == L"gears of war" || _wcsicmp(ext.c_str(), L".iso") == 0) candidates.push_back(e.path());
  }
  for (const fs::path& c : candidates) {
    std::string error;
    if (try_iso(c, &error)) return true;
    std::fprintf(stderr, "[gow] %s ignorada: %s\n", c.string().c_str(), error.c_str());
  }
  return false;
}

fs::path exe_dir() {
  wchar_t buf[MAX_PATH * 4];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
  return fs::path(std::wstring(buf, n)).parent_path();
}

bool choose_with_dialog() {
  if (!dialogs_allowed()) return false;
  for (;;) {
    wchar_t file[32768] = L"";
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.lpstrFilter = L"Imagem de disco (*.iso)\0*.iso\0Todos os ficheiros (*.*)\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrTitle = L"Selecione a ISO do Gears of War 1 (Xbox 360)";
    // NOCHANGEDIR: user_data/ e o gow_config.ini são relativos à pasta de trabalho.
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return false;
    std::string error;
    if (try_iso(file, &error)) {
      config::edit().iso_path = to_utf8(file);
      config::save();
      return true;
    }
    tell(L"Esta imagem não pode ser usada: " + to_wide(error));
  }
}

bool image_valid(const std::vector<uint8_t>& image) {
  return xex_image::sha1_hex(image.data(), image.size()) == xex_image::kExpectedSha1;
}

// Se falhar, o jogo corre na mesma: volta a descodificar na próxima abertura.
void write_cache(const fs::path& path, const std::vector<uint8_t>& image) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  fs::path tmp = path;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(reinterpret_cast<const char*>(image.data()), (std::streamsize)image.size());
    if (!out) return;
  }
  fs::rename(tmp, path, ec);
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  out->resize((size_t)in.tellg());
  in.seekg(0);
  return (bool)in.read(reinterpret_cast<char*>(out->data()), (std::streamsize)out->size());
}

}  // namespace

void init_working_dir() {
  int argc = 0;
  if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
    if (argc > 1) {
      std::error_code ec;
      const fs::path p = fs::absolute(argv[1], ec);
      (_wcsicmp(p.extension().c_str(), L".iso") == 0 ? g_arg_iso : g_arg_image) = p;
    }
    LocalFree(argv);
  }
  std::error_code ec;
  if (fs::exists("gow_config.ini", ec) || fs::exists(vfs::game_root(), ec)) return;
  fs::current_path(exe_dir(), ec);
}

bool prepare(std::vector<uint8_t>* image) {
  std::string error;
  const char* env_iso = std::getenv("GOW_ISO");
  const fs::path forced = !g_arg_iso.empty() ? g_arg_iso : env_iso && *env_iso ? fs::path(to_wide(env_iso)) : fs::path();
  if (!forced.empty() && !try_iso(forced, &error)) {
    tell(L"Não foi possível usar a imagem " + forced.wstring() + L": " + to_wide(error));
    return false;
  }
  const std::string& saved = config::get().iso_path;
  if (forced.empty() && !saved.empty() && !try_iso(to_wide(saved), &error))
    std::fprintf(stderr, "[gow] IsoPath ignorado (%s): %s\n", saved.c_str(), error.c_str());

  if (!iso::mounted() && !loose_game_complete()) {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec), exe = exe_dir();
    if (!search_folder(cwd) && (exe == cwd || !search_folder(exe)) && !choose_with_dialog()) {
      tell(L"O jogo não foi encontrado. Coloque a ISO do Gears of War 1 na pasta do gow.exe, ou selecione-a ao abrir o jogo.");
      return false;
    }
  }

  // GOW_ISO_VERIFY=pasta: ferramenta de desenvolvimento, sai logo a seguir.
  if (const char* v = std::getenv("GOW_ISO_VERIFY"); v && iso::mounted()) std::_Exit(iso::verify_against(v) ? 4 : 0);

  const fs::path loose_image = vfs::game_root() / "base_image.bin";
  std::error_code ec;
  if (!g_arg_image.empty() || fs::is_regular_file(loose_image, ec)) {
    const fs::path& p = !g_arg_image.empty() ? g_arg_image : loose_image;
    if (read_file(p, image)) return true;
    tell(L"Não foi possível ler " + p.wstring());
    return false;
  }
  const fs::path cache = vfs::user_root() / "base_image.bin";
  if (read_file(cache, image) && image_valid(*image)) {
    std::fprintf(stderr, "[gow] imagem base: %s\n", cache.string().c_str());
    return true;
  }
  const ULONGLONG t0 = GetTickCount64();
  std::vector<uint8_t> xex;
  if (!vfs::read_game_file("default.xex", &xex, ~size_t(0)) || !xex_image::decode(xex, image, &error) || !image_valid(*image)) {
    tell(L"O default.xex não é a versão suportada do jogo. " + to_wide(error));
    return false;
  }
  write_cache(cache, *image);
  std::fprintf(stderr, "[gow] imagem base gerada em %s (%llu ms)\n", cache.string().c_str(), GetTickCount64() - t0);
  return true;
}

}  // namespace game_source
