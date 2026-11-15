@echo off
cd /d "%~dp0"
rem HOP OR DROP build: z88dk (set Z88DK_DIR if it is not in c:\z88dk\). Output: HOPDROP.NABU
rem   size options (none of them slows the game): BSS left out of the file and zeroed at start-up,
rem   z88dk's text console left out (fputc_cons_stub in main.c)
if "%Z88DK_DIR%"=="" SET Z88DK_DIR=c:\z88dk\
SET ZCCCFG=%Z88DK_DIR%lib\config\
SET PATH=%Z88DK_DIR%bin;%PATH%
zcc +nabu -vn -m -create-app -compiler=sdcc -pragma-define:CRT_INITIALIZE_BSS=1 -pragma-define:CRT_TRIM_BSS=1 -pragma-define:CLIB_DEFAULT_SCREEN_MODE=-1 -pragma-define:CRT_ENABLE_STDIO=0 -pragma-redirect:fputc_cons=_fputc_cons_stub main.c -o HOPDROP
