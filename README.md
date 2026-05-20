# Dual Render ImGui Engine

Dual Render ImGui Engine is an Android native C++ project for rendering Dear ImGui inside targets that may use either OpenGL ES or Vulkan.

The library can install both renderer paths at startup, detect which graphics API is actually presenting frames, and route ImGui rendering through the active backend. You can also force a single graphics backend from `Renderer/renderer_config.hpp`.

## Features

- OpenGL ES rendering through `eglSwapBuffers`.
- Vulkan rendering through Vulkan symbol interception and `vkQueuePresentKHR`.
- Runtime selection between OpenGL ES and Vulkan, or forced backend selection.
- Native Android touch input capture.
- App-process touch fallback through Android input consumption hooks.
- Touch consumption when input starts over an ImGui window.
- Touch coordinate mapping between input surface and render surface.
- Shared Cascadia Mono and Font Awesome atlas for both graphics APIs.
- Shared ImGui image texture loading and rendering helpers.
- Shared ImGui draw callback for both graphics APIs.

## Architecture

`Renderer::Init()` installs graphics hooks according to `Renderer/renderer_config.hpp`. In `AUTO` mode, the first backend that reaches a frame-present call claims the active renderer with `Renderer::ClaimAPI()`.

The OpenGL ES backend initializes ImGui on the first valid `eglSwapBuffers` call.

The Vulkan backend captures instance/device/queue/swapchain state, creates ImGui Vulkan resources, and injects draw commands before present.

Touch input is captured from Android native input paths, queued safely, and drained on the render thread before `ImGui::NewFrame()`.

## Graphics Selection

Edit:

```text
app/src/main/cpp/Renderer/renderer_config.hpp
```

Choose one:

```cpp
#define SELECT_GRAPHIC AUTO
#define SELECT_GRAPHIC VULKAN
#define SELECT_GRAPHIC OPENGL
```

`AUTO` installs both graphics backends and lets the first presenting backend become active. Input stays auto-detected in all modes.

## Logging

Edit:

```text
app/src/main/cpp/Renderer/log_config.hpp
```

Set any switch to `0` to disable that log tag:

```cpp
#define DRI_LOG_ALL 1
#define DRI_LOG_DUAL_RENDER_IMGUI 1
#define DRI_LOG_UNIVERSAL_RENDERER 1
#define DRI_LOG_INPUT_HANDLER 1
#define DRI_LOG_OPENGL_RENDERER 1
#define DRI_LOG_VULKAN_RENDERER 1
#define DRI_LOG_IMGUI_ANDROID_BACKEND 1
#define DRI_LOG_IMAGE_TEXTURE 1
```

## Images

Include:

```cpp
#include "Renderer/images/image_texture.hpp"
```

Load and draw from your ImGui draw callback:

```cpp
static Renderer::Images::Texture logo;
static bool logoLoadTried = false;

if (!logoLoadTried) {
    logoLoadTried = true;
    Renderer::Images::LoadFromFile("/data/local/tmp/logo.png", &logo);
}

if (logo.IsValid())
    Renderer::Images::Render(logo, ImVec2(180.0f, 180.0f));
```

`LoadFromFile()` and `LoadFromMemory()` use Android ImageDecoder when available, with `stb_image` as a fallback decoder. `CreateFromRGBA()` can upload an existing RGBA8888 buffer directly.

## Native Library

The generated native library is still named:

```text
libmenu.so
```

You can load it with an existing external/native loader, or inject/load it directly from the target app startup path, such as an `Application` or game `Activity` `onCreate()` method. Keeping the name stable as `libmenu.so` makes both loading styles straightforward.

## Build

```bash
./gradlew :app:assembleDebug
```

The main output library is built for `arm64-v8a`.

## License

Original project code is available under the MIT License. See `LICENSE`.

Bundled third-party code and assets keep their own licenses. See `THIRD_PARTY_NOTICES.md`.

## Project Layout

```text
app/src/main/cpp/main.cpp
app/src/main/cpp/Renderer/renderer_config.hpp
app/src/main/cpp/Renderer/log_config.hpp
app/src/main/cpp/Renderer/imgui_fonts.cpp
app/src/main/cpp/Renderer/renderer.cpp
app/src/main/cpp/Renderer/images/image_texture.cpp
app/src/main/cpp/Renderer/input/input_handler.cpp
app/src/main/cpp/Renderer/opengl/opengl_renderer.cpp
app/src/main/cpp/Renderer/vulkan/vulkan_renderer.cpp
app/src/main/cpp/ImGui/
app/src/main/cpp/Font/
app/src/main/cpp/Dobby/
```

## Notes

This project is focused only on ImGui rendering and input across two Android graphics APIs. It does not include game-specific tools.
