#pragma once

/**
 * Universal ImGui Renderer
 * 
 * By default, hooks BOTH eglSwapBuffers AND Vulkan present simultaneously.
 * Whichever hook gets CALLED first wins and becomes the active renderer.
 * You can force one graphics backend in renderer_config.hpp.
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

    // Initialize graphics hooks according to renderer_config.hpp.
    bool Init();

    // Shutdown and unhook everything
    void Shutdown();

    // Set the user draw callback
    void SetDrawCallback(DrawCallback callback);

    // Get the currently active rendering API
    API GetActiveAPI();

    // Race condition resolver - called by whichever hook fires first
    bool ClaimAPI(API api);

    // Track frame present heartbeat for each API
    void OnFrameRendered(API api);

    // Get screen dimensions
    int GetScreenWidth();
    int GetScreenHeight();

    // Touch input forwarding
    void HandleTouch(int action, float x, float y);

    // Source coordinate bounds for native input events before mapping to render coordinates.
    void SetInputSurfaceSize(int width, int height);

    // Drains queued touch events into the current ImGui context on the render thread.
    void DrainInputEvents();

    // Publishes current ImGui input hit regions from the render thread.
    void UpdateInputCaptureState();

    // Returns true when the touch should be consumed by the overlay instead of
    // being forwarded to the target application.
    bool ShouldConsumeTouch(int action, float x, float y);

    // Check if ImGui wants to capture input
    bool WantsCaptureInput();

} // namespace Renderer
