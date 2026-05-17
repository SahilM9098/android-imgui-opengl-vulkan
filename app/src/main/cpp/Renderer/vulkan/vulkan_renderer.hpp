#pragma once

/**
 * Vulkan Renderer
 * 
 * Hooks vkQueuePresentKHR (and supporting functions) to inject ImGui
 * rendering into Vulkan-based games.
 * 
 * Hook chain:
 *   vkCreateDevice         -> capture VkDevice, VkQueue
 *   vkCreateSwapchainKHR   -> capture swapchain images, format, extent
 *   vkAcquireNextImageKHR  -> capture current image index
 *   vkQueuePresentKHR      -> inject ImGui render commands
 */

#include <functional>

namespace Renderer {
namespace Vulkan {

    using DrawCallback = std::function<void()>;

    // Initialize Vulkan hooks
    bool Init();

    // Shutdown and restore original functions
    void Shutdown();

    // Set the user draw callback
    void SetDrawCallback(DrawCallback callback);

    // Get screen dimensions from the swapchain extent
    int GetScreenWidth();
    int GetScreenHeight();

    // Forward touch input to ImGui
    void HandleTouch(int action, float x, float y);

} // namespace Vulkan
} // namespace Renderer
