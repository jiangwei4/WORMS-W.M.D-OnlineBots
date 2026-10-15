@echo off
rem WMD-Mod : version finale (sans log ni bip) et version de diagnostic (avec log).
rem MinGW 32 bits requis.
set GCC=gcc
set FLAGS=-std=gnu99 -O2 -Wall -Wno-unused-parameter -Wno-unused-function -Wno-format -shared -s
set LINK=dinput8.def -Wl,--kill-at,--enable-stdcall-fixup -static-libgcc
%GCC% %FLAGS% -DWMD_NOLOG -DWMD_NOBEEP -o dinput8.dll wmdmod.c %LINK%
%GCC% %FLAGS% -o dinput8_log.dll wmdmod.c %LINK%
