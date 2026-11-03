#!/usr/bin/env python3
"""
package_release.py - gera dist/GearsOfWar1-PC-Port/ e o .zip a partir do build atual.

O pacote leva apenas uma lista fechada de ficheiros (gow.exe, DLLs do FFmpeg, configuração, leia-me). Depois de o
criar, o script recusa continuar se encontrar algo que pareça dados do jogo.

Atenção: gow.exe contém o código do jogo traduzido para x86-64 (generated_src/ compilado). Ver "Direitos de autor" no
README.md antes de publicar este pacote.
"""
import argparse
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAME = "GearsOfWar1-PC-Port"
DEFAULT_VERSION = "0.5.0"
# As mesmas de GOW_FFMPEG_DLLS em runtime/CMakeLists.txt: só as dependências reais do avcodec.
FFMPEG_DLLS = ["avcodec-60.dll", "avutil-58.dll", "libwinpthread-1.dll", "zlib1.dll", "VCRUNTIME140.dll"]
FORBIDDEN = re.compile(r"\.(iso|xex|xxx|bik|sav|bin|pdb|log)$|^base_image|^extracted|^generated_src|^user_data", re.I)

LEIAME = """Gears of War 1 - port PC (recompilacao estatica) v{version}
==========================================================

Este pacote NAO contem os dados do jogo. Precisa da sua propria copia do disco de
Gears of War para Xbox 360, em imagem ISO. Nao e preciso extrair nada.

COMO JOGAR
  1. Coloque a ISO nesta pasta, junto do gow.exe (qualquer nome terminado em .iso),
     ou abra o gow.exe e selecione-a quando ele perguntar (o caminho fica no gow_config.ini).
  2. Abra o gow.exe.

O jogo abre sem janela de consola e sem log. Em caso de crash escreve gow_crash.log nesta pasta.
Para ver o log: defina GOW_VERBOSE=1 e abra o gow.exe a partir de uma linha de comandos.

Na 1a abertura o jogo cria aqui a pasta user_data\\ (gravacoes, perfil, cache de shaders). Na 1a entrada em cada
nivel, o ecra de carregamento fica parado 2 a 3 minutos a preparar os shaders (o titulo da janela mostra quantos
faltam). Da segunda vez ja nao acontece.
Requer Windows 10/11 de 64 bits, CPU com AVX2 e placa grafica com Direct3D 12.

CONTROLOS
  Comando Xbox (XInput), ou teclado e rato:
  Todas as teclas podem ser mudadas em F8 > Controlos. Por omissao:
  WASD = andar                    Clique = capturar o rato
  Rato = camara                   Esquerdo = disparar, Direito = mirar
  Espaco = A    E/F = B           C = X    Q = Y
  R = RB        Shift = LB        Ctrl = L3
  1/2/3/4 ou setas = direcional   Enter = Start    Backspace = Back
  Esc ou F10 = soltar o rato

ATALHOS
  F8         Menu: video, FPS, audio e idioma, controlos e Co-op pela internet
  F9         Copiar o codigo de ligacao do Co-op
  Alt+Enter  Ecra inteiro

CO-OP
  Anfitriao: F8 > Multiplayer Co-op > "Gerar Sala Co-op" > "Copiar Convite", e envie-o ao amigo.
  Convidado: F8 > Multiplayer Co-op, cole o convite na caixa > "Conectar ao Amigo".
  Depois, no menu do jogo: Campaign ou Versus > System Link > crie ou entre na sala da lista.
  Partilhar um convite revela o seu IP publico a quem o receber.

IDIOMA
  F8 > Audio e Idioma (reiniciar para aplicar). Este disco tem ingles, alemao, frances, italiano
  e espanhol (Espanha e America Latina). Nao tem portugues.

MODS
  Crie uma pasta extracted\\ junto do gow.exe. Um ficheiro nela (ex.: extracted\\WarGame\\...)
  substitui o ficheiro com o mesmo caminho na ISO.

Projeto sem qualquer ligacao a Microsoft, Epic Games ou The Coalition.

LICENCAS
  ffmpeg\\ contem bibliotecas do projeto FFmpeg (LGPL, https://ffmpeg.org) e as suas dependencias.
"""


def default_ini():
    """Fonte unica: o texto que o proprio gow.exe escreve (kDefaults em runtime/config.cpp)."""
    src = open(os.path.join(ROOT, "runtime", "config.cpp"), encoding="utf-8").read()
    block = src[src.index("const char kDefaults[]"):]
    block = block[:block.index(";\n")]
    lines = re.findall(r'^\s*"(.*)\\n"', block, re.M)
    if not any(l == "IsoPath=" for l in lines):
        sys.exit("kDefaults sem IsoPath=")
    return "\r\n".join(lines) + "\r\n"


def main():
    ap = argparse.ArgumentParser(description="Empacota o gow.exe para distribuicao")
    ap.add_argument("--version", default=DEFAULT_VERSION)
    ap.add_argument("--build-dir", default=os.path.join(ROOT, "build", "runtime"))
    args = ap.parse_args()

    exe = os.path.join(args.build_dir, "gow.exe")
    if not os.path.isfile(exe):
        sys.exit(f"falta {exe}: compile primeiro (cmake --build build --target gow -- -j 8)")
    missing = [d for d in FFMPEG_DLLS if not os.path.isfile(os.path.join(args.build_dir, "ffmpeg", d))]
    if missing:
        sys.exit(f"faltam DLLs em {args.build_dir}/ffmpeg: {missing}")

    dist = os.path.join(ROOT, "dist")
    out = os.path.join(dist, NAME)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, "ffmpeg"))
    shutil.copy2(exe, out)
    for d in FFMPEG_DLLS:
        shutil.copy2(os.path.join(args.build_dir, "ffmpeg", d), os.path.join(out, "ffmpeg", d))
    with open(os.path.join(out, "gow_config.ini"), "w", encoding="ascii", newline="") as f:
        f.write(default_ini())
    with open(os.path.join(out, "LEIAME.txt"), "w", encoding="ascii", newline="\r\n") as f:
        f.write(LEIAME.format(version=args.version))

    packed = []
    for base, _, names in os.walk(out):
        for n in names:
            rel = os.path.relpath(os.path.join(base, n), out)
            if FORBIDDEN.search(n) or FORBIDDEN.search(rel):
                sys.exit(f"recusado: {rel} parece dados do jogo")
            packed.append(rel)

    archive = os.path.join(dist, f"{NAME}-v{args.version}-win64.zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for rel in sorted(packed):
            z.write(os.path.join(out, rel), os.path.join(NAME, rel))

    for rel in sorted(packed):
        print(f"  {os.path.getsize(os.path.join(out, rel)):>10,}  {rel}")
    print(f"{archive}: {os.path.getsize(archive):,} bytes")


if __name__ == "__main__":
    main()
