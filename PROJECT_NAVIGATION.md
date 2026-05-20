# Dual Render ImGui Engine - Project Navigation Guide

## Project Overview

**Name:** Dual Render ImGui Engine  
**Package:** `com.sahilm9098.dualrenderimgui`  
**Type:** Android native C++ ImGui rendering library  
**Purpose:** Inject and render Dear ImGui on Android targets using either OpenGL ES or Vulkan, with runtime or forced graphics selection.

## Source Map

| Component | Path |
|-----------|------|
| Java entry | `app/src/main/java/com/sahilm9098/dualrenderimgui/MainActivity.java` |
| Native entry | `app/src/main/cpp/main.cpp` |
| Renderer config | `app/src/main/cpp/Renderer/renderer_config.hpp` |
| Log config | `app/src/main/cpp/Renderer/log_config.hpp` |
| ImGui font setup | `app/src/main/cpp/Renderer/imgui_fonts.cpp` |
| Image textures | `app/src/main/cpp/Renderer/images/image_texture.cpp` |
| Renderer router | `app/src/main/cpp/Renderer/renderer.cpp` |
| Input hooks | `app/src/main/cpp/Renderer/input/input_handler.cpp` |
| OpenGL renderer | `app/src/main/cpp/Renderer/opengl/opengl_renderer.cpp` |
| Vulkan renderer | `app/src/main/cpp/Renderer/vulkan/vulkan_renderer.cpp` |
| ImGui sources | `app/src/main/cpp/ImGui/` |
| Bundled fonts | `app/src/main/cpp/Font/` |
| Hooking library | `app/src/main/cpp/Dobby/` |

## Runtime Flow

1. `libmenu.so` loads and runs `onLibraryLoad()`.
2. The init thread installs graphics hooks based on `SELECT_GRAPHIC` and installs auto input hooks.
3. OpenGL ES path hooks `eglSwapBuffers`.
4. Vulkan path intercepts Vulkan symbol loading and hooks present/device/swapchain functions.
5. In `AUTO` mode, the first graphics API that presents a frame claims the renderer.
6. The active backend creates an ImGui context and loads the shared font atlas.
7. Image helpers can decode or upload RGBA textures and submit them through ImGui's texture update path.
8. Touch input is captured from Android native input, mapped to the active render surface, and consumed when it starts over ImGui.
9. The draw callback renders the ImGui interface each frame.

## Build Notes

| Setting | Value |
|---------|-------|
| Native library | `libmenu.so` |
| ABI | `arm64-v8a` |
| minSdk | 26 |
| compileSdk | 36 |
| CMake | 3.22.1 |

## Important Entry Points

| Function | Purpose |
|----------|---------|
| `Renderer::Init()` | Installs graphics hooks based on `renderer_config.hpp`. |
| `Renderer::SetupImGuiFonts()` | Loads Cascadia Mono and merges Font Awesome icons into the ImGui atlas. |
| `Renderer::Images::LoadFromFile()` | Decodes an image and queues it as an ImGui texture. |
| `Renderer::Images::Render()` | Draws a loaded image texture with ImGui. |
| `Renderer::ClaimAPI()` | Selects the active graphics API. |
| `Renderer::SetDrawCallback()` | Registers the ImGui draw function. |
| `Renderer::Input::Init()` | Installs auto-detected input hooks. |
| `Renderer::HandleTouch()` | Queues touch events. |
| `Renderer::DrainInputEvents()` | Applies touch events inside the current ImGui frame. |
| `Renderer::ShouldConsumeTouch()` | Decides whether a target touch event should be swallowed. |
| `DRI_LOG_*` defines | Enable or disable logs per tag. |

## Current Scope

- OpenGL ES ImGui rendering through `eglSwapBuffers`.
- Vulkan ImGui rendering through Vulkan device/swapchain/present hooks.
- Runtime or forced graphics API selection.
- Android native and app-process touch input mapping.
- Input consumption for touch sequences that start over ImGui.
- Shared bundled font atlas for OpenGL ES and Vulkan.
- Shared image loading and rendering helpers for ImGui.
- Minimal sample UI plus optional ImGui demo window.
