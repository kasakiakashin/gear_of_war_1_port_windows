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
#include <cstring>
#include <fstream>
#include <map>
#include <string>

#include "config.h"
#include "kernel/iso.h"
#include "kernel/vfs.h"
#include "kernel/xex_image.h"

namespace fs = std::filesystem;

namespace game_source {
namespace {

fs::path g_arg_iso, g_arg_image;
std::vector<std::wstring> g_args;  // linha de comandos sem o nome do programa
bool g_located = false;

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

void tell(const std::wstring& text, UINT icon = MB_ICONWARNING) {
  std::fprintf(stderr, "[gow] %s\n", to_utf8(text).c_str());
  if (dialogs_allowed()) MessageBoxW(nullptr, text.c_str(), L"Gears of War", MB_OK | icon);
}

bool read_file(const fs::path& path, std::vector<uint8_t>* out);

bool has_wargame(const fs::path& dir) {
  std::error_code ec;
  return fs::is_regular_file(dir / "WarGame" / "Xbox360TOC.txt", ec);
}

// default.xex solto de uma pasta: é a versão para a qual o código foi traduzido? `version` = versão do título
// (EXECUTION_INFO, ex.: "0.0.5.1" com a title update 5; o disco é 0.0.0.1). Em cache por pasta (o launcher pergunta a cada pintura).
struct XexInfo {
  bool present = false, supported = false;
  std::wstring version;
  fs::path sibling;  // outra versão traduzida: o gow*.exe dela, ao lado deste (vazio se não houver)
};
fs::path exe_dir();
fs::path g_iso_sibling;  // a imagem de disco montada é da outra versão (abre-se o executável irmão)

fs::path sibling_for(int known) {
  if (known < 0 || known == xex_image::kSelf) return {};
  std::error_code ec;
  const fs::path p = exe_dir() / xex_image::kKnown[known].exe;
  return fs::is_regular_file(p, ec) ? p : fs::path();
}
const XexInfo& loose_xex(const fs::path& dir) {
  static std::map<fs::path, XexInfo> cache;
  auto [it, fresh] = cache.try_emplace(dir);
  if (!fresh) return it->second;
  XexInfo& info = it->second;
  std::vector<uint8_t> d;
  if (!read_file(dir / "default.xex", &d) || d.size() < 0x18 || std::memcmp(d.data(), "XEX2", 4) != 0) return info;
  info.present = true;
  info.version = L"desconhecida";
  const int known = xex_image::identify(d);
  info.supported = known == xex_image::kSelf;
  info.sibling = sibling_for(known);
  auto be32 = [&](size_t o) { return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3] : 0u; };
  for (uint32_t i = 0, n = std::min<uint32_t>(be32(0x14), 0x100); i < n; i++) {
    if (be32(0x18 + 8 * i) != 0x00040006u) continue;
    const uint32_t v = be32(be32(0x18 + 8 * i + 4) + 4);
    wchar_t b[32];
    std::swprintf(b, 32, L"%u.%u.%u.%u", v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF);
    info.version = b;
  }
  return info;
}

// Pasta escolhida no launcher (GameDir): os ficheiros dela passam à frente da ISO. Com um default.xex de outra versão o
// executável tem de vir da ISO, e o solto fica escondido (o gow.exe só tem o código do do disco).
void apply_game_dir() {
  const char* env = std::getenv("GOW_GAME_DIR");
  const std::string& dir = config::get().game_dir;
  if ((!env || !*env) && !dir.empty()) {
    if (has_wargame(to_wide(dir))) vfs::set_game_root(to_wide(dir));
    else std::fprintf(stderr, "[gow] GameDir ignorado (sem WarGame\\Xbox360TOC.txt): %s\n", dir.c_str());
  }
  const XexInfo& x = loose_xex(vfs::game_root());
  if (x.present)
    std::fprintf(stderr, "[gow] pasta do jogo %s: default.xex %s (este exe: %s)%s\n", vfs::game_root().string().c_str(),
                 to_utf8(x.version).c_str(), xex_image::kVersion,
                 x.supported ? "" : x.sibling.empty() ? ", outra versão sem executável irmão" : ", abre o executável irmão");
  // Da outra versão com o irmão presente: abre-se esse executável com a pasta inteira (preparar); só sem irmão é que o
  // executável tem de vir da ISO.
  vfs::hide_loose_xex(x.present && !x.supported && x.sibling.empty());
}

bool custom_game_dir() {
  const std::string& dir = config::get().game_dir;
  return !dir.empty() && vfs::game_root() == fs::path(to_wide(dir));
}

// Tudo o que é preciso para jogar sem imagem de disco; só alguns ficheiros soltos (mods) não chegam.
bool loose_game_complete() {
  const fs::path root = vfs::game_root();
  const XexInfo& x = loose_xex(root);
  return has_wargame(root) && (x.supported || !x.sibling.empty());
}

bool try_iso(const fs::path& path, std::string* error) {
  if (!iso::mount(path, error)) return false;
  const iso::Node* xex = iso::find("default.xex");
  std::vector<uint8_t> data;
  if (!xex || !iso::read_all(*xex, &data)) {
    *error = "a imagem não tem default.xex";
  } else if (const int known = xex_image::identify(data); known == xex_image::kSelf || loose_xex(vfs::game_root()).supported) {
    // Com o executável na pasta do jogo (ex.: o da title update), a imagem de disco só dá os dados.
    g_iso_sibling.clear();
    return true;
  } else if (!sibling_for(known).empty()) {
    g_iso_sibling = sibling_for(known);  // é da outra versão: preparar() abre o executável irmão
    return true;
  } else {
    *error = std::string("o default.xex não é a versão ") + xex_image::kVersion + " do Gears of War 1 (Xbox 360), a deste gow.exe";
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
    o.lpstrFilter =
        L"Jogo do Xbox 360 (*.iso; default.xex)\0*.iso;default.xex\0Imagem de disco (*.iso)\0*.iso\0Todos os ficheiros (*.*)\0*.*\0";
    o.lpstrFile = file;
    o.nMaxFile = (DWORD)(sizeof file / sizeof file[0]);
    o.lpstrTitle = L"Selecione a ISO ou o default.xex de uma pasta do Gears of War 1 (Xbox 360)";
    // NOCHANGEDIR: user_data/ e o gow_config.ini são relativos à pasta de trabalho.
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&o)) return false;
    // Pasta do jogo (dump do disco, RGH, tradução): default.xex + WarGame\.
    if (const fs::path picked(file); _wcsicmp(picked.filename().c_str(), L"default.xex") == 0) {
      const fs::path dir = picked.parent_path();
      if (!has_wargame(dir)) {
        tell(L"Esta pasta não tem o jogo: falta WarGame\\Xbox360TOC.txt ao lado do default.xex.");
        continue;
      }
      config::edit().game_dir = to_utf8(dir.wstring());
      config::save();
      apply_game_dir();
      const XexInfo& x = loose_xex(dir);
      if (x.supported || !x.sibling.empty() || iso::mounted()) {
        if (!x.supported && x.sibling.empty())
          tell(L"O default.xex desta pasta é a versão " + x.version + L", e este gow.exe corre a " + to_wide(xex_image::kVersion) +
                   L": o executável vem da ISO, e os ficheiros da pasta (incluindo traduções) são usados por cima dela.",
               MB_ICONINFORMATION);
        return true;
      }
      tell(L"O default.xex desta pasta é a versão " + x.version + L", e este gow.exe corre a " + to_wide(xex_image::kVersion) +
           L".\n\nOs ficheiros da pasta, incluindo traduções, vão ser usados, mas falta o executável certo: selecione a seguir "
           L"uma ISO com essa versão.");
      continue;
    }
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
  return xex_image::image_supported(image);
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
    for (int i = 1; i < argc; i++) g_args.push_back(argv[i]);
    LocalFree(argv);
  }
  // O 1º argumento que não é uma opção (--…) é a ISO ou a imagem base; o valor de --pack também não conta.
  for (size_t i = 0; i < g_args.size(); i++) {
    if (g_args[i].rfind(L"--", 0) == 0) {
      if (g_args[i] == L"--pack") i++;
      continue;
    }
    std::error_code ec;
    const fs::path p = fs::absolute(g_args[i], ec);
    (_wcsicmp(p.extension().c_str(), L".iso") == 0 ? g_arg_iso : g_arg_image) = p;
    break;
  }
  std::error_code ec;
  if (fs::exists("gow_config.ini", ec) || fs::exists(vfs::game_root(), ec)) return;
  fs::current_path(exe_dir(), ec);
}

bool locate(bool interactive) {
  if (g_located) return true;
  apply_game_dir();
  std::string error;
  const char* env_iso = std::getenv("GOW_ISO");
  const fs::path forced = !g_arg_iso.empty() ? g_arg_iso : env_iso && *env_iso ? fs::path(to_wide(env_iso)) : fs::path();
  if (!forced.empty() && !try_iso(forced, &error)) {
    if (interactive) tell(L"Não foi possível usar a imagem " + forced.wstring() + L": " + to_wide(error));
    return false;
  }
  const std::string& saved = config::get().iso_path;
  if (forced.empty() && !saved.empty() && !try_iso(to_wide(saved), &error))
    std::fprintf(stderr, "[gow] IsoPath ignorado (%s): %s\n", saved.c_str(), error.c_str());
  if (!iso::mounted() && !loose_game_complete()) {
    std::error_code ec;
    const fs::path cwd = fs::current_path(ec), exe = exe_dir();
    if (!search_folder(cwd) && (exe == cwd || !search_folder(exe)) && !(interactive && choose_with_dialog())) return false;
  }
  g_located = true;
  return true;
}

bool choose_iso() {
  if (!choose_with_dialog()) return false;
  g_located = true;
  return true;
}

std::wstring status() {
  const std::wstring dir = custom_game_dir() ? L"Pasta: " + vfs::game_root().wstring() : L"";
  const XexInfo& x = loose_xex(vfs::game_root());
  const std::wstring by_sibling =
      x.present && !x.supported && !x.sibling.empty() ? L" (versão " + x.version + L", abre com " + x.sibling.filename().wstring() + L")" : L"";
  if (iso::mounted()) {
    std::wstring iso =
        L"ISO: " + (config::get().iso_path.empty() ? std::wstring(L"encontrada na pasta do jogo") : to_wide(config::get().iso_path));
    if (!x.supported && !g_iso_sibling.empty() && by_sibling.empty()) iso += L" (abre com " + g_iso_sibling.filename().wstring() + L")";
    if (dir.empty()) return iso;
    return dir + (!by_sibling.empty() ? by_sibling : x.present && !x.supported ? L" (executável da ISO)" : L"") + L"  +  " + iso;
  }
  if (loose_game_complete()) return dir.empty() ? L"Pasta extracted\\ (ficheiros soltos)" + by_sibling : dir + by_sibling;
  if (!dir.empty() && x.present)
    return L"O default.xex da pasta é a versão " + x.version + L" (este gow.exe corre a " + to_wide(xex_image::kVersion) +
           L"): selecione também uma ISO com essa versão.";
  return L"Jogo não encontrado: selecione a ISO ou o default.xex de uma pasta do jogo.";
}

bool has_flag(const wchar_t* name) {
  for (const std::wstring& a : g_args)
    if (_wcsicmp(a.c_str(), name) == 0) return true;
  return false;
}

std::wstring option(const wchar_t* name) {
  for (size_t i = 0; i + 1 < g_args.size(); i++)
    if (_wcsicmp(g_args[i].c_str(), name) == 0) return g_args[i + 1];
  return {};
}

bool prepare(std::vector<uint8_t>* image) {
  std::string error;
  if (!locate(true)) {
    const XexInfo& x = loose_xex(vfs::game_root());
    tell(x.present && !x.supported
             ? L"O default.xex da pasta do jogo é a versão " + x.version + L", e este gow.exe corre a " +
                   to_wide(xex_image::kVersion) + L". Selecione também uma ISO com essa versão."
             : L"O jogo não foi encontrado. Coloque a ISO do Gears of War 1 na pasta do gow.exe, ou selecione-a (ou o default.xex de "
               L"uma pasta do jogo) ao abrir o jogo.");
    return false;
  }

  // GOW_ISO_VERIFY=pasta: ferramenta de desenvolvimento, sai logo a seguir.
  if (const char* v = std::getenv("GOW_ISO_VERIFY"); v && iso::mounted()) std::_Exit(iso::verify_against(v) ? 4 : 0);

  // O jogo escolhido é da outra versão traduzida: abre-se o executável dela, com os mesmos argumentos, já sem launcher.
  {
    const XexInfo& x = loose_xex(vfs::game_root());
    const fs::path other = x.present && !x.supported ? x.sibling : !x.supported && iso::mounted() ? g_iso_sibling : fs::path();
    if (!other.empty()) {
      std::wstring cmd = L"\"" + other.wstring() + L"\"";
      for (const std::wstring& a : g_args) cmd += L" \"" + a + L"\"";
      if (!has_flag(L"--skip-launcher")) cmd += L" --skip-launcher";
      // O log do irmão vai para o mesmo sítio que o deste (consola ou ficheiro redirecionado).
      STARTUPINFOW si{};
      si.cb = sizeof si;
      si.dwFlags = STARTF_USESTDHANDLES;
      si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
      si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
      si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
      for (HANDLE h : {si.hStdOutput, si.hStdError})
        if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
      PROCESS_INFORMATION pi{};
      std::fprintf(stderr, "[gow] o jogo é de outra versão: a abrir %s\n", other.string().c_str());
      std::fflush(stderr);
      if (CreateProcessW(other.c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        ExitProcess(0);
      }
      tell(L"Não foi possível abrir " + other.wstring());
      return false;
    }
  }

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
