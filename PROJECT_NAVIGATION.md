# Dual Render ImGui Engine - Project Navigation Guide

## Project Overview

**Name:** Dual Render ImGui Engine  
**Package:** `com.sahilm9098.dualrenderimgui`  
**Type:** Android native C++ ImGui rendering library  
**Purpose:** Inject and render Dear ImGui on Android targets using either OpenGL ES or Vulkan, with runtime API detection.

## Source Map

| Component | Path |
|-----------|------|
| Java entry | `app/src/main/java/com/sahilm9098/dualrenderimgui/MainActivity.java` |
| Native entry | `app/src/main/cpp/main.cpp` |
| Renderer router | `app/src/main/cpp/Renderer/renderer.cpp` |
| OpenGL renderer | `app/src/main/cpp/Renderer/opengl/opengl_renderer.cpp` |
| Vulkan renderer | `app/src/main/cpp/Renderer/vulkan/vulkan_renderer.cpp` |
| ImGui sources | `app/src/main/cpp/ImGui/` |
| Hooking library | `app/src/main/cpp/Dobby/` |

## Runtime Flow

1. `libmenu.so` loads and runs `onLibraryLoad()`.
2. The init thread installs OpenGL ES, Vulkan, and input hooks.
3. OpenGL ES path hooks `eglSwapBuffers`.
4. Vulkan path intercepts Vulkan symbol loading and hooks present/device/swapchain functions.
5. The first graphics API that presents a frame claims the renderer.
6. Touch input is captured from Android native input, mapped to the active render surface, and drained on the render thread.
7. The draw callback renders the ImGui interface each frame.

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
| `Renderer::Init()` | Installs both renderer paths. |
| `Renderer::ClaimAPI()` | Selects the active graphics API. |
| `Renderer::SetDrawCallback()` | Registers the ImGui draw function. |
| `Renderer::HandleTouch()` | Queues touch events. |
| `Renderer::DrainInputEvents()` | Applies touch events inside the current ImGui frame. |

## Current Scope

- OpenGL ES ImGui rendering through `eglSwapBuffers`.
- Vulkan ImGui rendering through Vulkan device/swapchain/present hooks.
- Runtime API selection.
- Android native touch input mapping.
- Minimal sample UI plus optional ImGui demo window.
