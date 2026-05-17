#pragma once

/**
 * Universal ImGui Renderer
 * 
 * Hooks BOTH eglSwapBuffers AND Vulkan present simultaneously.
 * Whichever hook gets CALLED first by the game wins and becomes
 * the active renderer. The other becomes a passthrough.
 * 
 * This solves the problem where libEGL.so is always loaded on Android
 * even in Vulkan-only games (hooking succeeds but never fires).
 */

#include <functional>
#include <atomic>

namespace Renderer {

    enum class API {
        NONE = 0,
        OPENGL_ES,
        VULKAN
    };

    // Callback type for user draw logic
    using DrawCallback = std::function<void()>;

    // Initialize the renderer - hooks both APIs, first call wins
    bool Init();

    // Shutdown and unhook everything
    void Shutdown();

    // Set the user draw callback
    void SetDrawCallback(DrawCallback callback);

    // Get the currently active rendering API
    API GetActiveAPI();

    // Race condition resolver - called by whichever hook fires first
    bool ClaimAPI(API api);

    // Get screen dimensions
    int GetScreenWidth();
    int GetScreenHeight();

    // Touch input forwarding
    void HandleTouch(int action, float x, float y);

    // Source coordinate bounds for native input events before mapping to render coordinates.
    void SetInputSurfaceSize(int width, int height);

    // Drains queued touch events into the current ImGui context on the render thread.
    void DrainInputEvents();

    // Check if ImGui wants to capture input
    bool WantsCaptureInput();

} // namespace Renderer
