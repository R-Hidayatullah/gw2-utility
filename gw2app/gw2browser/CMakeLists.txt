cmake_minimum_required(VERSION 3.10)

project(gw2browser
    VERSION 0.1.0
    LANGUAGES C CXX
)

# -----------------------------------------------------------------------------
# Compiler
# -----------------------------------------------------------------------------

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(
    UNICODE
    _UNICODE
)

# -----------------------------------------------------------------------------
# Include directories
# -----------------------------------------------------------------------------

include_directories(
    ${PROJECT_SOURCE_DIR}/include

    ${PROJECT_SOURCE_DIR}/../gw2mcp/native
    ${PROJECT_SOURCE_DIR}/../gw2mcp/native/include
    ${PROJECT_SOURCE_DIR}/../gw2mcp/native/third_party     # nlohmann/json.hpp
    ${PROJECT_SOURCE_DIR}/../dr_libs-master/dr_libs-master
    ${PROJECT_SOURCE_DIR}/../gw2index
    ${PROJECT_SOURCE_DIR}/../minieditor/external/stb-master
)

# -----------------------------------------------------------------------------
# Sources
# -----------------------------------------------------------------------------

file(GLOB_RECURSE APP_CPP
    "${PROJECT_SOURCE_DIR}/src/*.cpp"
)

file(GLOB_RECURSE APP_C
    "${PROJECT_SOURCE_DIR}/src/*.c"
)

set(SQLITE_SOURCE
    ${PROJECT_SOURCE_DIR}/../gw2index/sqlite3.c
)



add_executable(gw2browser WIN32
    ${SQLITE_SOURCE}
    ${APP_CPP}
    ${APP_C}

)

# -----------------------------------------------------------------------------
# Windows libraries
# -----------------------------------------------------------------------------

target_link_libraries(gw2browser PRIVATE

    d3d11
    dxgi
    d3dcompiler
    dxguid

    windowscodecs

    ole32
    oleaut32
    uuid

    comctl32
    comdlg32

    shell32
    user32
    gdi32
    advapi32

    winmm
)

# ---- STATIC LINKING (MinGW) ----
if (MINGW)
    set(BUILD_SHARED_LIBS OFF)

    # Force static libgcc + libstdc++
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()