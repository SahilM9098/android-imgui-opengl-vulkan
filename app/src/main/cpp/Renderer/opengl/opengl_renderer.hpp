#pragma once

/**
 * OpenGL ES Renderer
 * 
 * Hooks eglSwapBuffers to inject ImGui rendering into OpenGL ES games.
 * Works with any game using EGL + GLES2/GLES3.
 */

#include <functional>

namespace Renderer {
namespace OpenGL {

    using DrawCallback = std::function<void()>;

    // Initialize OpenGL ES hooks (eglSwapBuffers)
    bool Init();

    // Shutdown and restore original functions
    void Shutdown();

    // Set the user draw callback
    void SetDrawCallback(DrawCallback callback);

    // Get screen dimensions from the EGL surface
    int GetScreenWidth();
    int GetScreenHeight();

    // Forward touch input to ImGui
    void HandleTouch(int action, float x, float y);

} // namespace OpenGL
} // namespace Renderer
