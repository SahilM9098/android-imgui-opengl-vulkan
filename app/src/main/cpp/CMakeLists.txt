cmake_minimum_required(VERSION 3.22.1)

project("menu")

# ─── Dobby (inline hooking library) ──────────────────────────────────────────────

add_library(dobby STATIC IMPORTED)
set_target_properties(dobby PROPERTIES
        IMPORTED_LOCATION ${CMAKE_SOURCE_DIR}/Dobby/libs/${ANDROID_ABI}/libdobby.a)

# ─── Include directories ─────────────────────────────────────────────────────────

include_directories(${CMAKE_SOURCE_DIR}/ImGui)
include_directories(${CMAKE_SOURCE_DIR}/ImGui/backends)
include_directories(${CMAKE_SOURCE_DIR}/Dobby)
include_directories(${CMAKE_SOURCE_DIR}/Font)
include_directories(${CMAKE_SOURCE_DIR}/Renderer)
include_directories(${CMAKE_SOURCE_DIR}/Renderer/images)
include_directories(${CMAKE_SOURCE_DIR}/Renderer/input)
include_directories(${CMAKE_SOURCE_DIR}/Renderer/opengl)
include_directories(${CMAKE_SOURCE_DIR}/Renderer/vulkan)

# ─── ImGui core sources ──────────────────────────────────────────────────────────

set(IMGUI_SOURCES
        ImGui/imgui.cpp
        ImGui/imgui_draw.cpp
        ImGui/imgui_tables.cpp
        ImGui/imgui_widgets.cpp)

# ─── ImGui backend sources ───────────────────────────────────────────────────────

set(IMGUI_BACKEND_SOURCES
        ImGui/backends/imgui_impl_android.cpp
        ImGui/backends/imgui_impl_opengl3.cpp
        ImGui/backends/imgui_impl_vulkan.cpp)

# ─── Renderer sources ────────────────────────────────────────────────────────────

set(RENDERER_SOURCES
        Renderer/imgui_fonts.cpp
        Renderer/images/image_texture.cpp
        Renderer/renderer.cpp
        Renderer/input/input_handler.cpp
        Renderer/opengl/opengl_renderer.cpp
        Renderer/vulkan/vulkan_renderer.cpp)

# ─── Build shared library ────────────────────────────────────────────────────────

add_library(${CMAKE_PROJECT_NAME} SHARED
        ${IMGUI_SOURCES}
        ${IMGUI_BACKEND_SOURCES}
        ${RENDERER_SOURCES}
        main.cpp)

# ─── Link libraries ──────────────────────────────────────────────────────────────

target_link_libraries(${CMAKE_PROJECT_NAME}
        dobby
        EGL
        GLESv3
        vulkan
        android
        log)
