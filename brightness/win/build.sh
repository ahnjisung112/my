#!/bin/sh
# Linux에서 Windows용 exe/설치파일 빌드 (필요: gcc-mingw-w64-x86-64, nsis)
set -e
export LC_ALL=C.UTF-8
cd "$(dirname "$0")"
mkdir -p out
x86_64-w64-mingw32-windres app.rc -O coff -o out/app.res
x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra -municode -mwindows -static \
  brightness.c out/app.res -o out/Brightness.exe \
  -ldxva2 -lwbemuuid -ladvapi32 -lole32 -loleaut32 -lshell32 -lgdi32 -luser32
x86_64-w64-mingw32-strip out/Brightness.exe
makensis -V2 installer.nsi

echo "OK: out/Brightness.exe, out/BrightnessSetup.exe"
