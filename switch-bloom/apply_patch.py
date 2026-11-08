#!/usr/bin/env python3
"""Patch the official 9.3.1-nx2 SDL/OpenGL Switch port. Fails closed if changed."""
from pathlib import Path
import sys, shutil
repo=Path(sys.argv[1]).resolve()
source=Path(__file__).resolve().parent
target=repo/"libultraship/src/fast/backends/gfx_sdl2.cpp"
if not target.is_file():
    raise SystemExit("gfx_sdl2.cpp nao encontrado")
s=target.read_text(encoding="utf-8")
anchor_inc='#include "ship/port/switch/SwitchImpl.h"'
anchor_swap="    SyncFramerateWithTime();\n    SDL_GL_SwapWindow(mWnd);"
if s.count(anchor_inc)!=1 or s.count(anchor_swap)!=1:
    raise SystemExit("ATENCAO: fontes divergentes da versao 9.3.1-nx2. Nenhuma alteracao aplicada.")
if "SwitchBloom::Apply" not in s:
    original=target.with_suffix(".cpp.original")
    shutil.copyfile(target,original)
    s=s.replace(anchor_inc,anchor_inc+'\n#include "switch_bloom.h"')
    s=s.replace(anchor_swap, """    SyncFramerateWithTime();
#ifdef __SWITCH__
    int bloomW=0, bloomH=0;
    SDL_GL_GetDrawableSize(mWnd,&bloomW,&bloomH);
    Fast::SwitchBloom::Apply(bloomW,bloomH);
#endif
    SDL_GL_SwapWindow(mWnd);""")
    target.write_text(s,encoding="utf-8")
shutil.copyfile(source/"switch_bloom.h",target.parent/"switch_bloom.h")
print("Patch aplicado no backend SDL2. Bloom=1.20 (1/4 resolucao).")
