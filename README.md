# Dual Render ImGui Engine

Dual Render ImGui Engine is an Android native C++ project for rendering Dear ImGui inside targets that may use either OpenGL ES or Vulkan.

The library installs both renderer paths at startup, detects which graphics API is actually presenting frames, and routes ImGui rendering through the active backend.

## Features

- OpenGL ES rendering through `eglSwapBuffers`.
- Vulkan rendering through Vulkan symbol interception and `vkQueuePresentKHR`.
- Runtime selection between OpenGL ES and Vulkan.
- Native Android touch input capture.
- Touch coordinate mapping between input surface and render surface.
- Shared ImGui draw callback for both graphics APIs.

## Architecture

`Renderer::Init()` installs both rendering backends. The first backend that reaches a frame-present call claims the active renderer with `Renderer::ClaimAPI()`.

The OpenGL ES backend initializes ImGui on the first valid `eglSwapBuffers` call.

The Vulkan backend captures instance/device/queue/swapchain state, creates ImGui Vulkan resources, and injects draw commands before present.

Touch input is captured from Android native input, queued safely, and drained on the render thread before `ImGui::NewFrame()`.

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
app/src/main/cpp/Renderer/renderer.cpp
app/src/main/cpp/Renderer/opengl/opengl_renderer.cpp
app/src/main/cpp/Renderer/vulkan/vulkan_renderer.cpp
app/src/main/cpp/ImGui/
app/src/main/cpp/Dobby/
```

## Notes

This project is focused only on ImGui rendering and input across two Android graphics APIs. It does not include game-specific tools.
