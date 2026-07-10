# Alternative to build.ps1. Produces gw2dat_cli.exe.
#   cmake -S . -B build -G "MinGW Makefiles"
#   cmake --build build
# The build.ps1 one-liner is simpler; this is here for IDE / CMake workflows.
cmake_minimum_required(VERSION 3.20)
project(gw2dat_cli CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gw2dat_cli
    main.cpp
    src/gw2dat.cpp
    src/BinaryParser.cpp
)

target_include_directories(gw2dat_cli PRIVATE
    include
    third_party
    third_party/stb
)

# Static-link the runtime so the .exe runs without MinGW DLLs on PATH.
if (MINGW)
    target_link_options(gw2dat_cli PRIVATE -static -static-libgcc -static-libstdc++)
endif()
