// iso.h — leitura direta de uma imagem de disco Xbox 360 (XDVDFS), sem extrair nada.
// Uma imagem por processo, montada no arranque (antes de haver threads convidadas) e nunca desmontada depois:
// os Node* são estáveis e podem ser usados por qualquer thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace iso {

struct Node {
  std::string name;
  uint32_t sector = 0;
  uint64_t size = 0;
  bool dir = false;
  std::vector<Node> children;  // ordenados pelo nome em minúsculas
};

bool mount(const std::filesystem::path& image, std::string* error);
void unmount();
bool mounted();

// rel = "WarGame/CookedXenon/Core.xxx" (sem distinguir maiúsculas; "" = raiz). Nulo se não existir ou sem imagem.
const Node* find(const std::string& rel);
// Limitado ao tamanho do ficheiro; ler depois do fim dá sucesso com 0 bytes. Em erro, GetLastError() indica a causa.
bool read(const Node& node, uint64_t offset, void* dst, uint32_t len, uint32_t* got);
bool read_all(const Node& node, std::vector<uint8_t>* out);
// FILETIME de masterização do volume: o XDVDFS não guarda datas por ficheiro.
uint64_t volume_time();

// GOW_ISO_VERIFY=pasta: compara cada ficheiro da imagem com a cópia solta. Devolve o número de diferenças.
int verify_against(const std::filesystem::path& dir);

}  // namespace iso
