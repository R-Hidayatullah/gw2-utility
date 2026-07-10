cmake_minimum_required(VERSION 3.10.0)
project(minieditor VERSION 0.1.0 LANGUAGES C CXX)

# Set C++ standard
set(CXX_STANDARD 20)
set(CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)
include_directories(external/glad33extcore/include)
include_directories(external/imgui-docking)
include_directories(external/imgui-docking/backends)
include_directories(external/glfw-3.4.bin.WIN64/include)
include_directories(external/glm-1.0.3)
include_directories(external/ImGuizmo-master/src)
include_directories(external/stb-master/)
include_directories(external/half-rocm-7.2.4/include)
include_directories(external/nlohmann-json/single_include/)


set(IMGUIZMO_SOURCES
    external/ImGuizmo-master/src/ImGuizmo.cpp
)

set(IMGUI_SOURCES
    external/imgui-docking/imgui.cpp
    external/imgui-docking/imgui_demo.cpp
    external/imgui-docking/imgui_draw.cpp
    external/imgui-docking/imgui_widgets.cpp
    external/imgui-docking/imgui_tables.cpp
    external/imgui-docking/backends/imgui_impl_opengl3.cpp
    external/imgui-docking/backends/imgui_impl_glfw.cpp
)

set(STB_SOURCES
    external/stb-master/stb_vorbis.c
)

set(GLAD_SOURCES
    external/glad33extcore/src/glad.c
)

set(APP_SOURCES
    src/BinaryParser.cpp
    src/TemplateManager.cpp
    src/HexViewer.cpp
    src/TreeView.cpp
    src/JsonEditor.cpp
    src/Application.cpp
    src/main.cpp
)



link_directories(
    external/glfw-3.4.bin.WIN64/lib-mingw-w64
)


set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static")


add_executable (${PROJECT_NAME}
    ${IMGUI_SOURCES}
    ${IMGUIZMO_SOURCES}
    ${STB_SOURCES}
    ${GLAD_SOURCES}
    ${APP_SOURCES}
)

# Link libraries to your executable
target_link_libraries(${PROJECT_NAME} glfw3 opengl32 gdi32 user32 winmm)
