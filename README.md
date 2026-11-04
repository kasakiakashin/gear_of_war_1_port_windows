# Gears of War 1 — PC port by static recompilation

![Platform: Windows x64](https://img.shields.io/badge/Platform-Windows%20x64-0078D6)
![Graphics: Direct3D 12](https://img.shields.io/badge/Graphics-Direct3D%2012-107C10)
![Architecture: x86_64 AVX2](https://img.shields.io/badge/Architecture-x86__64%20AVX2-555555)

**[Português](#português) · [English](#english)**

---

## Português

Port experimental de **Gears of War (Xbox 360, 2006)** para Windows x86-64. O executável PowerPC do jogo é traduzido
para C++ por recompilação estática, e o kernel do Xbox 360, a GPU Xenos e o áudio XMA são reimplementados
(D3D12, WASAPI). Não é um emulador nem usa o código-fonte original.

**Este repositório não contém dados do jogo.** É preciso uma cópia própria do disco, em imagem ISO.

### Funcionalidades

- **Recompilação estática:** 2,4 milhões de instruções PowerPC (VMX/VMX128 incluído) traduzidas para C++.
- **Direct3D 12:** resolução interna até 4K e FSR 1.0 (EASU + RCAS).
- **Taxa de quadros:** 30 (como no Xbox 360), 60, 120 FPS ou Ilimitado, com o tempo do jogo real.
- **Tradutor de shaders Xenos:** microcódigo da GPU do Xbox 360 convertido em HLSL, com cache em disco.
- **Launcher com pré-compilação:** antes do jogo, procura os 17 mil shaders nos pacotes do disco (com descompressão LZO) e
  compila-os com todos os núcleos, com barra de progresso; o pacote traz-os já compilados (`prebuilt_shaders.bin`).
- **Co-op e Versus em System Link por túnel UDP P2P:** no menu **F8**, com um link de convite.
- **Leitura direta da ISO:** sem extrair 6,22 GB; ficheiros soltos em `extracted/` têm prioridade (mods).
- **Som:** WASAPI, com áudio XMA descodificado pelo FFmpeg.
- **Menu F8 com 4 abas:** vídeo, áudio e idioma, controlos e Co-op, em tempo real.
- **Entrada:** comando XInput ou teclado e rato, com todas as teclas remapeáveis.
- **Idiomas do disco:** inglês, alemão, francês, italiano e espanhol (Espanha e América Latina); coreano e chinês só em
  texto. O disco não tem português.

### Estado

| Área | Situação |
|---|---|
| Arranque, menus, gravação e Continuar | A funcionar |
| 1.º nível da campanha (prisão) | Jogável, com comando ou teclado e rato; a 30 fps na máquina de desenvolvimento |
| Resto da campanha | **Por testar** |
| Co-op e Versus (túnel P2P) | Testados entre duas instâncias no mesmo PC; **por testar pela internet** |
| MSAA e EDRAM fiéis | Por fazer (MSAA ignorado) |
| Áudio | Depende das DLLs do FFmpeg; sem elas o XMA fica em silêncio |

Requisitos: Windows 10/11 de 64 bits, CPU com AVX2, placa gráfica com Direct3D 12.

### Jogar

1. Compile o projeto (ver abaixo) ou use um pacote gerado por `tools/package_release.py`.
2. Abra o `gow.exe`. No launcher, carregue em **Selecionar ISO** se a ISO não estiver na pasta, e depois em
   **INICIAR JOGO**. **Não é preciso extrair nada:** os ficheiros são lidos da ISO em tempo real.

O launcher tem as mesmas configurações do F8 (vídeo, áudio e idioma, controlos, Co-op: pode hospedar ou colar um convite
antes de entrar). Os shaders vêm compilados em `prebuilt_shaders.bin` e são lidos diretamente dele; sem esse ficheiro, o
launcher compila-os a partir do disco (cerca de 15 minutos num CPU de 12 threads, só uma vez). Se carregar em INICIAR JOGO antes do fim, a barra passa para o ecrã
do jogo. `gow.exe --skip-launcher` salta o launcher; `gow.exe --prepare-shaders [--pack ficheiro]` só prepara os shaders.

Só é aceite a versão para a qual o código foi traduzido. O `gow.exe` compara o SHA-1 da imagem do executável com
`083d7d0ab2ae1ee6d9d536bc5ca987862ad8c8f7` e recusa outras versões.

| Tecla | Ação |
|---|---|
| **F8** | Menu: vídeo, áudio e idioma, controlos, Co-op |
| **F9** | Copiar o código de ligação do Co-op |
| **Alt+Enter** | Ecrã inteiro |
| WASD / rato | Andar / câmara (clique para capturar o rato; Esc ou F10 solta) |
| Botão esquerdo / direito | Disparar / mirar |
| Espaço, E ou F, C, Q | A, B, X, Y |
| R, Shift, Ctrl | RB, LB, L3 |
| 1–4 ou setas, Enter, Backspace | Direcional, Start, Back |

### De onde vêm os ficheiros

1. **Solto em `extracted/`**, se existir (serve para desenvolvimento e mods).
2. **Dentro da ISO**: `gow.exe x.iso` ou `GOW_ISO`, depois `IsoPath=` no `gow_config.ini`, depois qualquer `.iso` (ou um
   ficheiro chamado `Gears of War`) na pasta do jogo, e por fim o seletor de ficheiros.

Um `extracted/` completo dispensa a ISO. Um `extracted/` parcial soma-se à ISO: basta colocar lá os ficheiros
alterados. Na 1.ª execução, a imagem do executável é descodificada da ISO (cerca de 0,1 s) e guardada em
`user_data/base_image.bin`.

### Compilar

Precisa de Python 3 e da sua ISO. Toda a toolchain vem do pip:

```bash
pip install cmake ninja ziglang cryptography
```

Extraia só o executável da sua ISO e descodifique-o:

```bash
python tools/xdvdfs.py "Gears of War.iso" --only default.xex -o extracted
```

```bash
python tools/xex2.py extracted/default.xex
```

Gere o C++ em `generated_src/` (~40 s):

```bash
python tools/ppc_recomp.py
```

Configure e compile (~3 min):

```bash
cmake -G Ninja -S . -B build
```

```bash
cmake --build build -- -j 8
```

Confirme com os testes de tradução e empacote, se quiser:

```bash
./build/generated_src/ppc_smoke.exe extracted/base_image.bin --difftest analysis/difftest_cases.txt
```

```bash
python tools/package_release.py
```

O resultado fica em `build/runtime/gow.exe` e em `dist/`. Para o áudio, `-DGOW_FFMPEG_SOURCE=<pasta>` indica uma pasta
com `avcodec-60.dll` e as suas dependências, que são copiadas para `build/runtime/ffmpeg/`.

`generated_src/` **não é versionado**, de propósito: é o código do jogo traduzido (cerca de 210 MB) e sai sempre da ISO
de quem compila.

### Direitos de autor

Gears of War pertence aos seus titulares. Este projeto não tem qualquer ligação à Microsoft, à Epic Games ou à
The Coalition, nem conta com a sua autorização.

- **O repositório** não contém a ISO, os ficheiros do disco, a imagem do executável nem `generated_src/`. Contém
  ferramentas, o runtime escrito para este projeto, análises derivadas do executável (endereços de funções, lista de
  ficheiros do disco) e a chave pública de descodificação XEX usada por `tools/xex2.py` e pelo runtime.
- **Um `gow.exe` compilado contém o código do jogo, traduzido para x86-64.** Mesmo sem nenhum ficheiro da ISO, um pacote
  binário distribui portanto uma obra derivada. Quem o publica assume esse risco. Compilar a partir da sua própria
  cópia é a via mais prudente.
- **`prebuilt_shaders.bin`** também é derivado do jogo: é o microcódigo dos shaders do disco, traduzido e compilado para
  DXBC. Um pacote sem ele (`package_release.py --no-shaders`) compila os mesmos shaders na máquina de quem joga.

### Estrutura

| Pasta | Conteúdo |
|---|---|
| `tools/` | Extração, recompilador PPC → C++, testes, empacotamento |
| `runtime/` | `gow.exe`: loader, kernel e XAM, sistema de ficheiros, GPU, áudio, rede |
| `analysis/` | Mapas e relatórios do executável |

### Licenças de terceiros

As DLLs do FFmpeg são LGPL 2.1+ (<https://ffmpeg.org>) e são carregadas em runtime. O código deste repositório ainda não
tem licença definida.

---

## English

An experimental port of **Gears of War (Xbox 360, 2006)** to Windows x86-64. The game's PowerPC executable is translated
to C++ by static recompilation, and the Xbox 360 kernel, Xenos GPU and XMA audio are reimplemented (D3D12, WASAPI).
It is not an emulator and does not use the original source code.

**This repository contains no game data.** You need your own copy of the disc, as an ISO image.

### Features

- **Static recompilation:** 2.4 million PowerPC instructions (including VMX/VMX128) translated to C++.
- **Direct3D 12:** internal resolution up to 4K and FSR 1.0 (EASU + RCAS).
- **Frame rate:** 30 (as on Xbox 360), 60, 120 FPS or Unlimited, with real game time.
- **Xenos shader translator:** Xbox 360 GPU microcode converted to HLSL, with a disk cache.
- **Launcher with shader precompilation:** before the game starts, it finds the 17,000 shaders in the disc packages
  (LZO-decompressing them) and compiles them on every core, with a progress bar; the package ships them precompiled
  (`prebuilt_shaders.bin`).
- **Co-op and Versus over System Link through a P2P UDP tunnel:** in the **F8** menu, with an invite link.
- **Direct ISO reading:** no need to extract 6.22 GB; loose files in `extracted/` take priority (mods).
- **Sound:** WASAPI, with XMA audio decoded by FFmpeg.
- **Four-tab F8 menu:** video, audio and language, controls and co-op, in real time.
- **Input:** XInput gamepad or keyboard and mouse, with every key remappable.
- **Disc languages:** English, German, French, Italian and Spanish (Spain and Latin America); Korean and Chinese are text
  only. The disc has no Portuguese.

### Status

| Area | State |
|---|---|
| Boot, menus, saving and Continue | Working |
| First campaign level (prison) | Playable with a gamepad or keyboard and mouse; 30 fps on the development machine |
| Rest of the campaign | **Untested** |
| Co-op and Versus (P2P tunnel) | Tested between two instances on one PC; **untested over the internet** |
| Accurate MSAA and EDRAM | Not done (MSAA is ignored) |
| Audio | Needs the FFmpeg DLLs; without them XMA is silent |

Requirements: 64-bit Windows 10/11, a CPU with AVX2, a Direct3D 12 graphics card.

### Playing

1. Build the project (see below) or use a package made by `tools/package_release.py`.
2. Open `gow.exe`. In the launcher, click **Selecionar ISO** if the ISO is not in the folder, then **INICIAR JOGO**.
   **Nothing needs extracting:** files are read from the ISO in real time.

The launcher has the same settings as F8 (video, audio and language, controls, co-op: you can host or paste an invite before
starting). The shaders ship compiled in `prebuilt_shaders.bin` and are read straight from it; without that file, the
launcher compiles them from the disc (about 15 minutes on a 12-thread CPU, once). If you click INICIAR JOGO before it finishes, the bar moves to the game screen.
`gow.exe --skip-launcher` skips the launcher; `gow.exe --prepare-shaders [--pack file]` only prepares the shaders.

Only the version the code was translated from is accepted. `gow.exe` compares the SHA-1 of the executable image with
`083d7d0ab2ae1ee6d9d536bc5ca987862ad8c8f7` and refuses other versions.

| Key | Action |
|---|---|
| **F8** | Menu: video, audio and language, controls, co-op |
| **F9** | Copy the co-op connection code |
| **Alt+Enter** | Fullscreen |
| WASD / mouse | Move / camera (click to capture the mouse; Esc or F10 releases it) |
| Left / right button | Fire / aim |
| Space, E or F, C, Q | A, B, X, Y |
| R, Shift, Ctrl | RB, LB, L3 |
| 1–4 or arrows, Enter, Backspace | D-pad, Start, Back |

### Where files come from

1. **Loose in `extracted/`**, if present (for development and mods).
2. **Inside the ISO**: `gow.exe x.iso` or `GOW_ISO`, then `IsoPath=` in `gow_config.ini`, then any `.iso` (or a file named
   `Gears of War`) in the game folder, and finally the file picker.

A complete `extracted/` makes the ISO optional. A partial `extracted/` is layered over the ISO, so it only needs the
files you changed. On the first run, the executable image is decoded from the ISO (about 0.1 s) and saved to
`user_data/base_image.bin`.

### Building

You need Python 3 and your ISO. The whole toolchain comes from pip:

```bash
pip install cmake ninja ziglang cryptography
```

Extract only the executable from your ISO and decode it:

```bash
python tools/xdvdfs.py "Gears of War.iso" --only default.xex -o extracted
```

```bash
python tools/xex2.py extracted/default.xex
```

Generate the C++ into `generated_src/` (~40 s):

```bash
python tools/ppc_recomp.py
```

Configure and build (~3 min):

```bash
cmake -G Ninja -S . -B build
```

```bash
cmake --build build -- -j 8
```

Check with the translation tests, and package if you want to:

```bash
./build/generated_src/ppc_smoke.exe extracted/base_image.bin --difftest analysis/difftest_cases.txt
```

```bash
python tools/package_release.py
```

The output is in `build/runtime/gow.exe` and `dist/`. For audio, `-DGOW_FFMPEG_SOURCE=<folder>` names a folder with
`avcodec-60.dll` and its dependencies, which are copied to `build/runtime/ffmpeg/`.

`generated_src/` is **deliberately not versioned**: it is the game's code in translated form (about 210 MB) and always
comes from the builder's own ISO.

### Copyright

Gears of War belongs to its rights holders. This project is not affiliated with or authorised by Microsoft, Epic Games or
The Coalition.

- **The repository** contains no ISO, disc files, executable image or `generated_src/`. It does contain tools, the runtime
  written for this project, analyses derived from the executable (function addresses, the disc's file list) and the publicly known XEX decryption key used by `tools/xex2.py` and the runtime.
- **A built `gow.exe` contains the game's code, translated to x86-64.** Even with no file from the ISO, a binary package
  therefore distributes a derivative work, and whoever publishes it takes on that risk. Building from your own copy is the
  more cautious route.
- **`prebuilt_shaders.bin`** is derived from the game too: it is the disc's shader microcode, translated and compiled to
  DXBC. A package without it (`package_release.py --no-shaders`) compiles the same shaders on the player's machine.

### Layout

| Folder | Contents |
|---|---|
| `tools/` | Extraction, PPC → C++ recompiler, tests, packaging |
| `runtime/` | `gow.exe`: loader, kernel and XAM, file system, GPU, audio, network |
| `analysis/` | Maps and reports about the executable |

### Third-party licences

The FFmpeg DLLs are LGPL 2.1+ (<https://ffmpeg.org>) and are loaded at runtime. The code in this repository has no licence
chosen yet.
