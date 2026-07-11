#!/usr/bin/env bash
# Build gw2index.exe (MinGW g++ / gcc). sqlite3.o is cached after the first build.
set -e
export PATH="/c/Users/Ridwan Hidayatullah/Documents/codeblocks-25.03mingw-nosetup/MinGW/bin:$PATH"
cd "$(dirname "$0")"
NAT="../gw2mcp/native"

if [ ! -f sqlite3.o ] || [ sqlite3.c -nt sqlite3.o ]; then
  echo "compiling sqlite3.c (one-time)..."
  gcc -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -c sqlite3.c -o sqlite3.o
fi

g++ -std=c++20 -O2 -I . -I "$NAT/include" -I "$NAT/third_party" \
  gw2index.cpp "$NAT/src/gw2dat.cpp" "$NAT/src/BinaryParser.cpp" sqlite3.o \
  -o gw2index.exe -static -static-libgcc -static-libstdc++
echo "OK -> gw2index.exe"
