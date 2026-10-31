// vfs.h — caminhos do convidado -> ficheiros do host.
//   game:\  d:\  \Device\Cdrom0\                -> extracted/ (GOW_GAME_DIR) e, para o que lá não estiver solto, a
//                                                  imagem de disco montada (kernel/iso.h); só leitura (DVD)
//   outros "<dev>:\..." e "\Device\..."           -> user_data/<dev>/... (GOW_USER_DIR), leitura/escrita
//   (cache:\, hdd:\, e:\, \Device\Harddisk0\Partition1\…). Nada é criado até o jogo criar um ficheiro, por isso
//   ficheiros só de devkit (xe:\CompareBackEnds) continuam "não encontrado".
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "kernel/iso.h"

namespace vfs {

struct Resolved {
  std::filesystem::path host;
  bool writable = false;
  const iso::Node* iso = nullptr;  // na imagem de disco; "host" pode não existir. Pasta em ambos: lista-se a união
  bool iso_parent = false;         // a pasta-mãe existe na imagem de disco
};

std::filesystem::path game_root();
std::filesystem::path user_root();
std::optional<Resolved> resolve(const std::string& guest_path);
// Ficheiro do jogo (solto ou na imagem de disco), até max bytes.
bool read_game_file(const std::string& rel, std::vector<uint8_t>* out, size_t max);
// Dispositivos montados por XamContentCreate ("save:" -> user_data/content/...); têm precedência sobre o resto.
void mount(const std::string& device, const std::filesystem::path& host);
bool unmount(const std::string& device);
// Lê o ObjectName (ANSI_STRING*) de um OBJECT_ATTRIBUTES convidado; vazio se não houver.
std::string object_name(uint8_t* m, uint32_t object_attributes);
// Chamado por NtClose depois de remover um handle: liberta o ficheiro do host se era o último handle.
void on_handle_closed(uint32_t object);

}  // namespace vfs
