cmake_minimum_required(VERSION 3.10)
project(gw2browser VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(
    include
    ../gw2mcp/native                 # gw2model.hpp
    ../gw2mcp/native/third_party     # nlohmann/json.hpp
)
add_compile_definitions(UNICODE _UNICODE)

add_executable(gw2browser WIN32
    src/main.cpp
    src/gw2dat.cpp
    src/entry_extractor.cpp
    src/dds.cpp
    src/wic_image.cpp
    src/splitter.cpp
    src/info_panel.cpp
    src/hexview.cpp
    src/mft_listview.cpp
    src/d3d_renderer.cpp
    src/model_renderer.cpp
    src/strs_view.cpp
    src/struct_template.cpp
)

target_link_libraries(gw2browser PRIVATE
    d3d11
    dxgi
    d3dcompiler
    windowscodecs
    ole32
    comctl32
    comdlg32
    gdi32
    user32
    shell32
)

# ---- STATIC LINKING (MinGW) ----
if (MINGW)
    set(BUILD_SHARED_LIBS OFF)

    # Force static libgcc + libstdc++
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")
endif()
