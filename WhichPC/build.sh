#!/usr/bin/env bash
# Cross-builds WhichPC.exe and the installer on Linux.
#   needs: python3 + Pillow, mingw-w64 (x86_64-w64-mingw32-g++), nsis (makensis)
set -euo pipefail
cd "$(dirname "$0")"
VERSION=1.0.0
CXX=${CXX:-x86_64-w64-mingw32-g++}
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}

python3 tools/gen_assets.py
mkdir -p build dist
$WINDRES -c 65001 -I src -I assets src/app.rc -O coff -o build/app.res.o
$CXX -std=c++17 -O2 -municode -mwindows -Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type \
    -Iassets src/main.cpp src/render.cpp src/settings.cpp src/system.cpp src/overlay.cpp build/app.res.o \
    -o build/WhichPC.exe -static -static-libgcc -static-libstdc++ -s \
    -lgdiplus -lshlwapi -lwtsapi32 -ldwmapi -lcomctl32 -lshcore -lole32 -luuid -lcomdlg32 -lpowrprof -lmsimg32
makensis -V2 -INPUTCHARSET UTF8 -DVERSION=$VERSION installer/WhichPC.nsi
ls -la build/WhichPC.exe dist/
