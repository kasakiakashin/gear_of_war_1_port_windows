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
- **Direct3D 12:** resolução interna de 0,5× (360p) a 3× (4K) e FSR 1.0 (EASU + RCAS) para ampliar as escalas abaixo de 1×.
- **Presets gráficos** (aba Vídeo do launcher e do F8): PC Fraco (540p + FSR, 30 FPS), Original Xbox 360 (720p, 30 FPS),
  PC Moderno (1080p, 60 FPS, AF 8×, GTAO) e Ultra Next-Gen (1440p, AF 16×, GTAO + SSRTGI + SSR + AO com raios se houver DXR 1.1).
- **Gráficos avançados** (aba "Avançado & RT"): filtro anisotrópico 2–16× só nas texturas 2D com mips e filtro linear;
  GTAO, luz indireta de 1 rebote (SSRTGI) e reflexos (SSR) em espaço de ecrã sobre a cena HDR, antes do pós-processamento e
  do HUD, a meia ou inteira resolução; render targets FP16 sem o limite 7e3 do Xenos; curva fílmica e vibrância. Os efeitos
  de ecrã são aproximações: o jogo não expõe normais nem a matriz de projeção (FOV de 70° assumido).
- **Ray tracing (DXR 1.1, RayQuery):** oclusão ambiente com raios sobre a geometria real da cena. Os vertex shaders traduzidos
  escrevem a posição de cada vértice dos desenhos opacos num buffer (UAV); como para a projeção do jogo w é a profundidade de vista,
  (x, y, w) de recorte é o espaço da câmara com x e y escalados, e a BLAS é construída aí a cada frame, sem precisar das matrizes.
  O raio primário de cada píxel encontra a superfície; os raios de AO veem também geometria escondida. Só com DXR 1.1 (a caixa fica
  desativada e mostra "Indisponível" nas outras GPUs). A BLAS é atualizada por refit quando a geometria tem o mesmo número de
  vértices (reconstruída de 8 em 8 frames) e o resultado passa por um denoiser bilateral separável guiado pela profundidade. No
  corredor do 1.º nível (≈0,9 M triângulos, RX 9060 XT): BLAS+TLAS 1,5 ms e raios 1,3 ms de GPU por frame a meia resolução,
  50–55 FPS contra 53–54 sem ele. O stream-out do D3D12 foi tentado primeiro e pendurava a GPU no driver da AMD.
  Não há sombras de contacto com raios: o jogo não expõe a direção das luzes.
- **Resolves zero-copy** (`ZeroCopyResolves`, desligado por omissão): a textura de um resolve passa a ser o próprio render
  target; quando o jogo o vai escrever, uma limpeza total troca-lhe o recurso (ping-pong, sem cópia) e qualquer outra escrita
  faz a cópia. No corredor só 1 das 9 texturas elegíveis por frame acaba numa limpeza total (16 cópias por frame em vez de 17);
  desde a v1.0.2 as tabelas de descritores são validadas por slot em vez de esvaziadas, e o FPS fica igual (51–54 contra 53–54).
  Sem ganho líquido, fica como opção.
- **Taxa de quadros:** 30 (como no Xbox 360), 60, 120 FPS ou Ilimitado, com o tempo do jogo real. Com "FPS adaptativo"
  (`AdaptiveFps=1`, por omissão), uma cena que não aguenta 60 fica a 30 estáveis em vez de oscilar.
- **Cache de PSOs:** os pipelines compilados pelo driver ficam em `user_data/shader_cache/pipelines.bin` e a lista dos usados em
  `pso_list.bin`; no arranque seguinte são recriados em fundo, por isso os picos ao entrar numa zona só acontecem na 1.ª visita.
- **Tiles da EDRAM fundidos:** o Xbox 360 desenha a cena principal duas vezes (metade de cima e de baixo, por não caber na EDRAM);
  aqui as superfícies têm a altura inteira e cada objeto é desenhado uma só vez. Com isto e um processador de comandos mais
  leve, o corredor mais pesado do 1.º nível (≈6600 desenhos por frame) passou de ~38 a 60 FPS. `GOW_EXACT_TILES=1` volta ao
  modo do hardware.
- **Critical sections com spin:** como o spin count de uma critical section real, o `RtlEnterCriticalSection` tenta o lock
  durante uns microssegundos antes de bloquear. O alocador do UE3 é partilhado pela game thread e pela de render, e bloquear
  logo custava-lhes 16–26% do tempo: Alamo (capítulo 69, ~2900 desenhos por frame) passou de 36–41 a 56–59 FPS.
- **Tradutor de shaders Xenos:** microcódigo da GPU do Xbox 360 convertido em HLSL, com cache em disco.
- **Launcher com pré-compilação:** antes do jogo, procura os 17 mil shaders nos pacotes do disco (com descompressão LZO) e
  compila-os com todos os núcleos, com barra de progresso; o pacote traz-os já compilados (`prebuilt_shaders.bin`).
- **Co-op e Versus em System Link por túnel UDP P2P:** no menu **F8**, com um link de convite.
- **Leitura direta da ISO:** sem extrair 6,22 GB; ficheiros soltos em `extracted/` têm prioridade (mods).
- **Som:** WASAPI, com áudio XMA descodificado pelo FFmpeg.
- **Menu F8 com 5 abas:** vídeo, avançado & RT, áudio e idioma, controlos e Co-op, em tempo real.
- **Entrada:** comando XInput ou teclado e rato, com todas as teclas remapeáveis.
- **Idiomas do disco:** inglês, alemão, francês, italiano e espanhol (Espanha e América Latina); coreano e chinês só em
  texto. O disco não tem português.

### Estado

| Área | Situação |
|---|---|
| Arranque, menus, gravação e Continuar | A funcionar |
| 1.º nível da campanha (prisão) | Jogável, com comando ou teclado e rato; a 30 fps na máquina de desenvolvimento |
| Resto da campanha | Os 33 capítulos (`WarGame_P?LoadChapter=37..69`, o caminho do seletor de capítulos) carregam e desenham o nível: 0 crashes e 0 erros de GPU em 90 s cada, incluindo a passagem automática Station → Tyro. **Por testar:** a campanha jogada de ponta a ponta |
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
- **Direct3D 12:** internal resolution from 0.5× (360p) to 3× (4K) and FSR 1.0 (EASU + RCAS) to upscale the scales below 1×.
- **Graphics presets** (Video tab of the launcher and F8): Low-Spec PC (540p + FSR, 30 FPS), Original Xbox 360 (720p, 30 FPS),
  Modern PC (1080p, 60 FPS, 8× AF, GTAO) and Ultra Next-Gen (1440p, 16× AF, GTAO + SSRTGI + SSR + ray-traced AO with DXR 1.1).
- **Advanced graphics** ("Advanced & RT" tab): 2–16× anisotropic filtering only on 2D textures with mips and linear filtering;
  screen-space GTAO, one-bounce indirect light (SSRTGI) and reflections (SSR) on the HDR scene, before post-processing and the
  HUD, at half or full resolution; FP16 render targets without the Xenos 7e3 limit; filmic curve and vibrance. The screen-space
  effects are approximations: the game exposes neither normals nor the projection matrix (a 70° FOV is assumed).
- **Ray tracing (DXR 1.1, RayQuery):** ray-traced ambient occlusion on the scene's real geometry. The translated vertex shaders
  write each vertex position of the opaque draws to a buffer (UAV); since the game's projection has w equal to view depth, clip
  (x, y, w) is camera space with scaled x and y, and the BLAS is built there every frame without needing the matrices. Each pixel's
  primary ray finds the surface; the AO rays also see hidden geometry. DXR 1.1 only (the checkbox is disabled and shows
  "unavailable" on other GPUs). The BLAS is refitted when the geometry has the same vertex count (rebuilt every 8 frames) and the
  result goes through a depth-guided separable bilateral denoiser. In the first level's corridor (≈0.9 M triangles, RX 9060 XT):
  1.5 ms BLAS+TLAS and 1.3 ms rays of GPU time per frame at half resolution, 50–55 FPS against 53–54 without it. D3D12 stream
  output was tried first and hung the GPU on the AMD driver. There are no ray-traced contact shadows: the game does not expose the
  light directions.
- **Zero-copy resolves** (`ZeroCopyResolves`, off by default): a resolve's texture becomes the render target itself; when the game
  is about to write it, a full clear swaps its resource (ping-pong, no copy) and any other write makes the copy. In the corridor
  only 1 of the 9 eligible textures per frame ends in a full clear (16 copies per frame instead of 17); since v1.0.2 descriptor
  tables are validated per slot instead of being flushed, and the FPS is the same (51–54 against 53–54). With no net gain, it
  stays as an option.
- **Frame rate:** 30 (as on Xbox 360), 60, 120 FPS or Unlimited, with real game time. With "adaptive FPS"
  (`AdaptiveFps=1`, the default), a scene that cannot hold 60 stays at a steady 30 instead of oscillating.
- **PSO cache:** pipelines compiled by the driver are kept in `user_data/shader_cache/pipelines.bin` and the list of used ones in
  `pso_list.bin`; on the next start they are recreated in the background, so hitches when entering an area only happen once.
- **Merged EDRAM tiles:** the Xbox 360 draws the main scene twice (top and bottom halves, because it does not fit in EDRAM);
  here the surfaces have the full height and each object is drawn once. With this and a lighter command processor, the
  heaviest corridor of the first level (≈6,600 draws per frame) went from ~38 to 60 FPS. `GOW_EXACT_TILES=1` restores the
  hardware behaviour.
- **Spinning critical sections:** like the spin count of a real critical section, `RtlEnterCriticalSection` tries the lock for
  a few microseconds before blocking. The UE3 allocator is shared by the game and render threads, and blocking straight away
  cost them 16–26% of their time: Alamo (chapter 69, ~2,900 draws per frame) went from 36–41 to 56–59 FPS.
- **Xenos shader translator:** Xbox 360 GPU microcode converted to HLSL, with a disk cache.
- **Launcher with shader precompilation:** before the game starts, it finds the 17,000 shaders in the disc packages
  (LZO-decompressing them) and compiles them on every core, with a progress bar; the package ships them precompiled
  (`prebuilt_shaders.bin`).
- **Co-op and Versus over System Link through a P2P UDP tunnel:** in the **F8** menu, with an invite link.
- **Direct ISO reading:** no need to extract 6.22 GB; loose files in `extracted/` take priority (mods).
- **Sound:** WASAPI, with XMA audio decoded by FFmpeg.
- **Five-tab F8 menu:** video, advanced & RT, audio and language, controls and co-op, in real time.
- **Input:** XInput gamepad or keyboard and mouse, with every key remappable.
- **Disc languages:** English, German, French, Italian and Spanish (Spain and Latin America); Korean and Chinese are text
  only. The disc has no Portuguese.

### Status

| Area | State |
|---|---|
| Boot, menus, saving and Continue | Working |
| First campaign level (prison) | Playable with a gamepad or keyboard and mouse; 30 fps on the development machine |
| Rest of the campaign | All 33 chapters (`WarGame_P?LoadChapter=37..69`, the chapter-select path) load and render the level: 0 crashes and 0 GPU errors in 90 s each, including the automatic Station → Tyro transition. **Untested:** the campaign played start to finish |
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
