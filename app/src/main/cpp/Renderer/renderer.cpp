#include "renderer.hpp"
#include "renderer_config.hpp"
#include "log_config.hpp"
#include "opengl/opengl_renderer.hpp"
#include "vulkan/vulkan_renderer.hpp"

#include <dlfcn.h>
#include <android/log.h>
#include <mutex>
#include <vector>
#include <algorithm>
#include <chrono>
#include "imgui.h"
#include "imgui_internal.h"

#define LOG_TAG "UniversalRenderer"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_UNIVERSAL_RENDERER, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_UNIVERSAL_RENDERER, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static constexpr int GRAPHIC_AUTO = AUTO;
static constexpr int GRAPHIC_VULKAN = VULKAN;
static constexpr int GRAPHIC_OPENGL = OPENGL;
static constexpr int SELECTED_GRAPHIC = SELECT_GRAPHIC;

#undef AUTO
#undef VULKAN
#undef OPENGL



namespace Renderer {

    static std::atomic<API> g_ActiveAPI{API::NONE};
    static DrawCallback g_DrawCallback = nullptr;

    static std::atomic<uint64_t> g_LastOpenGLCall{0};
    static std::atomic<uint64_t> g_LastVulkanCall{0};

    static uint64_t GetCurrentTimeMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }

    void OnFrameRendered(API api) {
        uint64_t now = GetCurrentTimeMs();
        if (api == API::OPENGL_ES) {
            g_LastOpenGLCall.store(now);
        } else if (api == API::VULKAN) {
            g_LastVulkanCall.store(now);
        }
    }

    struct TouchEvent {
        int action;
        float x;
        float y;
    };

    struct InputRect {
        float minX;
        float minY;
        float maxX;
        float maxY;
    };

    static std::mutex g_InputMutex;
    static std::vector<TouchEvent> g_PendingTouches;
    static std::vector<InputRect> g_InputCaptureRects;
    static int g_InputSurfaceWidth = 0;
    static int g_InputSurfaceHeight = 0;
    static int g_InputTransformLogCount = 0;
    static bool g_BlockTouchSequence = false;

    static void MapInputToRenderLocked(float& x, float& y) {
        int renderWidth = GetScreenWidth();
        int renderHeight = GetScreenHeight();

        if (g_InputSurfaceWidth > 0 && g_InputSurfaceHeight > 0 &&
            renderWidth > 0 && renderHeight > 0 &&
            (g_InputSurfaceWidth != renderWidth || g_InputSurfaceHeight != renderHeight)) {
            x *= (float)renderWidth / (float)g_InputSurfaceWidth;
            y *= (float)renderHeight / (float)g_InputSurfaceHeight;
        }

        if (renderWidth > 0)
            x = std::clamp(x, 0.0f, (float)renderWidth - 1.0f);
        if (renderHeight > 0)
            y = std::clamp(y, 0.0f, (float)renderHeight - 1.0f);
    }

    bool ClaimAPI(API api) {
        API current = g_ActiveAPI.load();
        if (current == api) {
            return true;
        }

        uint64_t now = GetCurrentTimeMs();

        if (current == API::NONE) {
            // First time claim
            if (g_ActiveAPI.compare_exchange_strong(current, api)) {
                const char* name = (api == API::OPENGL_ES) ? "OpenGL ES" : "Vulkan";
                LOGI(">>> %s claimed as active renderer (initial) <<<", name);
                return true;
            }
            current = g_ActiveAPI.load();
        }

        // If another API is already claimed, check if we should override/switch.
        // We switch if the currently active API has not been updated for a timeout period (1000ms)
        if (current != api && current != API::NONE) {
            if (api == API::VULKAN && current == API::OPENGL_ES) {
                if (g_ActiveAPI.compare_exchange_strong(current, api)) {
                    LOGI(">>> Active renderer switched from OpenGL ES to Vulkan (Vulkan present) <<<");
                    return true;
                }
                current = g_ActiveAPI.load();
            }

            uint64_t lastActiveTime = 0;
            if (current == API::OPENGL_ES) {
                lastActiveTime = g_LastOpenGLCall.load();
            } else if (current == API::VULKAN) {
                lastActiveTime = g_LastVulkanCall.load();
            }

            // If the active API has not rendered in the last 1000ms, or if it has never rendered
            if (lastActiveTime == 0 || (now - lastActiveTime) > 1000) {
                if (g_ActiveAPI.compare_exchange_strong(current, api)) {
                    const char* oldName = (current == API::OPENGL_ES) ? "OpenGL ES" : "Vulkan";
                    const char* newName = (api == API::OPENGL_ES) ? "OpenGL ES" : "Vulkan";
                    LOGI(">>> Active renderer switched from %s to %s (timeout/preemption) <<<", oldName, newName);
                    return true;
                }
            }
        }

        return g_ActiveAPI.load() == api;
    }

    bool Init() {
        if (SELECTED_GRAPHIC == GRAPHIC_OPENGL) {
            LOGI("Initializing Universal Renderer (OpenGL ES only)...");
            if (!OpenGL::Init()) {
                LOGE("Failed to hook OpenGL ES renderer!");
                return false;
            }
            LOGI("OpenGL ES hook installed (forced backend, waiting for first call...)");
            return true;
        }

        if (SELECTED_GRAPHIC == GRAPHIC_VULKAN) {
            LOGI("Initializing Universal Renderer (Vulkan only)...");
            if (!Vulkan::Init()) {
                LOGE("Failed to hook Vulkan renderer!");
                return false;
            }
            LOGI("Vulkan hooks installed (forced backend, waiting for first call...)");
            return true;
        }

        LOGI("Initializing Universal Renderer (auto graphics strategy)...");

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

        LOGI("Graphics hooks installed, waiting for first frame...");
        return true;
    }

    void Shutdown() {
        if (SELECTED_GRAPHIC == GRAPHIC_OPENGL) {
            OpenGL::Shutdown();
            g_ActiveAPI.store(API::NONE);
            g_DrawCallback = nullptr;
            return;
        }

        if (SELECTED_GRAPHIC == GRAPHIC_VULKAN) {
            Vulkan::Shutdown();
            g_ActiveAPI.store(API::NONE);
            g_DrawCallback = nullptr;
            return;
        }

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
        if (SELECTED_GRAPHIC == GRAPHIC_OPENGL) {
            OpenGL::SetDrawCallback(callback);
            return;
        }
        if (SELECTED_GRAPHIC == GRAPHIC_VULKAN) {
            Vulkan::SetDrawCallback(callback);
            return;
        }
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
            bool logMappedTouch =
                g_InputSurfaceWidth > 0 && g_InputSurfaceHeight > 0 &&
                renderWidth > 0 && renderHeight > 0 &&
                (g_InputSurfaceWidth != renderWidth || g_InputSurfaceHeight != renderHeight) &&
                g_InputTransformLogCount < 8;

            MapInputToRenderLocked(x, y);
            if (logMappedTouch) {
                LOGI("Touch map raw=%.1f,%.1f input=%dx%d render=%dx%d mapped=%.1f,%.1f",
                     event.x, event.y, g_InputSurfaceWidth, g_InputSurfaceHeight,
                     renderWidth, renderHeight, x, y);
                g_InputTransformLogCount++;
            }

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

    void UpdateInputCaptureState() {
        std::vector<InputRect> rects;

        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx) {
            ImGuiContext& g = *ctx;
            rects.reserve(g.Windows.Size);

            for (int i = 0; i < g.Windows.Size; i++) {
                ImGuiWindow* window = g.Windows[i];
                if (!window || !window->WasActive || window->Hidden)
                    continue;
                if (window->Flags & ImGuiWindowFlags_NoInputs)
                    continue;

                ImRect rect = window->OuterRectClipped;
                if (rect.IsInverted())
                    rect = window->Rect();
                if (rect.GetWidth() <= 0.0f || rect.GetHeight() <= 0.0f)
                    continue;

                rects.push_back({rect.Min.x, rect.Min.y, rect.Max.x, rect.Max.y});
            }
        }

        std::lock_guard<std::mutex> lock(g_InputMutex);
        g_InputCaptureRects.swap(rects);
        if (g_InputCaptureRects.empty())
            g_BlockTouchSequence = false;
    }

    bool ShouldConsumeTouch(int action, float x, float y) {
        std::lock_guard<std::mutex> lock(g_InputMutex);

        MapInputToRenderLocked(x, y);

        bool hitImGui = false;
        for (const InputRect& rect : g_InputCaptureRects) {
            if (x >= rect.minX && x <= rect.maxX &&
                y >= rect.minY && y <= rect.maxY) {
                hitImGui = true;
                break;
            }
        }

        if (action == 0) {
            g_BlockTouchSequence = hitImGui;
            return g_BlockTouchSequence;
        }

        bool consume = g_BlockTouchSequence;
        if (action == 1)
            g_BlockTouchSequence = false;
        return consume;
    }

    bool WantsCaptureInput() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (ctx) {
            return ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard;
        }
        return false;
    }

} // namespace Renderer
