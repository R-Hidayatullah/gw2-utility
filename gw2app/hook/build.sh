#!/usr/bin/env bash
# Build gw2_textkey_hook.dll + inject.exe (MinGW-w64 g++, sesuai toolchain gw2mcp).
set -e
cd "$(dirname "$0")"

# 1) MinHook (trampolin hook yang benar untuk prolog x64)
if [ ! -d minhook ]; then
  git clone --depth 1 https://github.com/TsudaKageyu/minhook.git minhook
fi

# 2) DLL hook (x64). hde64.c untuk x64 (jangan hde32.c).
g++ -shared -O2 -m64 -static -static-libgcc -static-libstdc++ \
    -I minhook/include \
    gw2_textkey_hook.cpp \
    minhook/src/buffer.c minhook/src/hook.c minhook/src/trampoline.c minhook/src/hde/hde64.c \
    -o gw2_textkey_hook.dll -lpsapi
echo "OK -> gw2_textkey_hook.dll"

# 3) injector
g++ -O2 -m64 -municode inject.cpp -o inject.exe
echo "OK -> inject.exe"
