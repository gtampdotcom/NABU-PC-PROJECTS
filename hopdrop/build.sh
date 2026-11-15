#!/bin/sh
# HOP OR DROP build: z88dk (zcc on PATH, ZCCCFG pointing at z88dk's lib/config/). Output: HOPDROP.NABU
#   size options (none of them slows the game): BSS left out of the file and zeroed at start-up,
#   z88dk's text console left out (fputc_cons_stub in main.c)
set -e
cd "$(dirname "$0")"
zcc +nabu -vn -m -create-app -compiler=sdcc \
  -pragma-define:CRT_INITIALIZE_BSS=1 -pragma-define:CRT_TRIM_BSS=1 \
  -pragma-define:CLIB_DEFAULT_SCREEN_MODE=-1 -pragma-define:CRT_ENABLE_STDIO=0 \
  -pragma-redirect:fputc_cons=_fputc_cons_stub \
  main.c -o HOPDROP
