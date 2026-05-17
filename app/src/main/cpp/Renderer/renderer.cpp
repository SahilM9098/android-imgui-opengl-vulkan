#include "renderer.hpp"
#include "opengl/opengl_renderer.hpp"
#include "vulkan/vulkan_renderer.hpp"

#include <dlfcn.h>
#include <android/log.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include "imgui.h"

#define LOG_TAG "UniversalRenderer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace Renderer {

    static std::atomic<API> g_ActiveAPI{API::NONE};
    static DrawCallback g_DrawCallback = nullptr;

    struct TouchEvent {
        int action;
        float x;
        float y;
    };

    static std::mutex g_InputMutex;
    static std::vector<TouchEvent> g_PendingTouches;
    static int g_InputSurfaceWidth = 0;
    static int g_InputSurfaceHeight = 0;
    static int g_InputTransformLogCount = 0;

    bool ClaimAPI(API api) {
        API expected = API::NONE;
        if (g_ActiveAPI.compare_exchange_strong(expected, api)) {
            const char* name = (api == API::OPENGL_ES) ? "OpenGL ES" : "Vulkan";
            LOGI(">>> %s claimed as active renderer <<<", name);
            return true;
        }
        return false; // Another API already claimed
    }

    bool Init() {
        LOGI("Initializing Universal Renderer (hook-both strategy)...");

        bool anyHooked = false;

        // Hook OpenGL ES (eglSwapBuffers) - will only activate if actually called
        if (OpenGL::Init()) {
            LOGI("OpenGL ES hook installed (waiting for first call...)");
            anyHooked = true;
        }

        // Hook Vulkan (vkQueuePresentKHR + supporting hooks) - will only activate if actually called
        if (Vulkan::Init()) {
            LOGI("Vulkan hooks installed (waiting for first call...)");
            anyHooked = true;
        }

        if (!anyHooked) {
            LOGE("Failed to hook any graphics API!");
            return false;
        }

        LOGI("Hooks installed, waiting for game to render first frame...");
        return true;
    }

    void Shutdown() {
        API active = g_ActiveAPI.load();
        switch (active) {
            case API::OPENGL_ES:
                OpenGL::Shutdown();
                break;
            case API::VULKAN:
                Vulkan::Shutdown();
                break;
            default:
                // Shutdown both if neither claimed yet
                OpenGL::Shutdown();
                Vulkan::Shutdown();
                break;
        }
        g_ActiveAPI.store(API::NONE);
        g_DrawCallback = nullptr;
    }

    void SetDrawCallback(DrawCallback callback) {
        g_DrawCallback = callback;
        OpenGL::SetDrawCallback(callback);
        Vulkan::SetDrawCallback(callback);
    }

    API GetActiveAPI() {
        return g_ActiveAPI.load();
    }

    int GetScreenWidth() {
        switch (g_ActiveAPI.load()) {
            case API::OPENGL_ES: return OpenGL::GetScreenWidth();
            case API::VULKAN:    return Vulkan::GetScreenWidth();
            default:             return 0;
        }
    }

    int GetScreenHeight() {
        switch (g_ActiveAPI.load()) {
            case API::OPENGL_ES: return OpenGL::GetScreenHeight();
            case API::VULKAN:    return Vulkan::GetScreenHeight();
            default:             return 0;
        }
    }

    void SetInputSurfaceSize(int width, int height) {
        if (width <= 0 || height <= 0)
            return;

        std::lock_guard<std::mutex> lock(g_InputMutex);
        if (g_InputSurfaceWidth == width && g_InputSurfaceHeight == height)
            return;

        g_InputSurfaceWidth = width;
        g_InputSurfaceHeight = height;
        g_InputTransformLogCount = 0;
        LOGI("Input source surface size: %dx%d", width, height);
    }

    void HandleTouch(int action, float x, float y) {
        std::lock_guard<std::mutex> lock(g_InputMutex);
        g_PendingTouches.push_back({action, x, y});
    }

    void DrainInputEvents() {
        std::vector<TouchEvent> events;
        {
            std::lock_guard<std::mutex> lock(g_InputMutex);
            events.swap(g_PendingTouches);
        }

        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx)
            return;

        ImGuiIO& io = ImGui::GetIO();
        for (const TouchEvent& event : events) {
            float x = event.x;
            float y = event.y;
            int renderWidth = GetScreenWidth();
            int renderHeight = GetScreenHeight();

            if (g_InputSurfaceWidth > 0 && g_InputSurfaceHeight > 0 &&
                renderWidth > 0 && renderHeight > 0 &&
                (g_InputSurfaceWidth != renderWidth || g_InputSurfaceHeight != renderHeight)) {
                float scaleX = (float)renderWidth / (float)g_InputSurfaceWidth;
                float scaleY = (float)renderHeight / (float)g_InputSurfaceHeight;
                x *= scaleX;
                y *= scaleY;

                if (g_InputTransformLogCount < 8) {
                    LOGI("Touch map raw=%.1f,%.1f input=%dx%d render=%dx%d mapped=%.1f,%.1f",
                         event.x, event.y, g_InputSurfaceWidth, g_InputSurfaceHeight,
                         renderWidth, renderHeight, x, y);
                    g_InputTransformLogCount++;
                }
            }

            x = std::clamp(x, 0.0f, (float)std::max(renderWidth - 1, 0));
            y = std::clamp(y, 0.0f, (float)std::max(renderHeight - 1, 0));

            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            io.AddMousePosEvent(x, y);
            switch (event.action) {
                case 0: io.AddMouseButtonEvent(0, true); break;
                case 1: io.AddMouseButtonEvent(0, false); break;
                case 2: break;
                default: break;
            }
        }
    }

    bool WantsCaptureInput() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx) {
            return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
        }
        return false;
    }

} // namespace Renderer
