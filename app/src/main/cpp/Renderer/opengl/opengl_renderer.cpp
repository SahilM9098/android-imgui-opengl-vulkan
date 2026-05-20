#include "opengl_renderer.hpp"
#include "image_texture.hpp"
#include "imgui_fonts.hpp"
#include "log_config.hpp"
#include "renderer.hpp"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "dobby.h"

#define LOG_TAG "OpenGLRenderer"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_OPENGL_RENDERER, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_OPENGL_RENDERER, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace Renderer {
namespace OpenGL {

    // State
    static bool g_Initialized = false;
    static bool g_Claimed = false;
    static int g_ScreenWidth = 0;
    static int g_ScreenHeight = 0;
    static DrawCallback g_DrawCallback = nullptr;
    static EGLContext g_CurrentContext = EGL_NO_CONTEXT;
    static EGLSurface g_CurrentSurface = EGL_NO_SURFACE;
    static EGLContext g_PendingContext = EGL_NO_CONTEXT;
    static EGLSurface g_PendingSurface = EGL_NO_SURFACE;
    static int g_StableSwapCount = 0;
    static constexpr int kRequiredStableSwaps = 2;

    // Original function pointer
    static EGLBoolean (*orig_eglSwapBuffers)(EGLDisplay display, EGLSurface surface) = nullptr;
    static EGLSurface (*orig_eglCreateWindowSurface)(EGLDisplay display, EGLConfig config,
                                                     EGLNativeWindowType window,
                                                     const EGLint* attrib_list) = nullptr;

    static EGLSurface hook_eglCreateWindowSurface(EGLDisplay display, EGLConfig config,
                                                   EGLNativeWindowType window,
                                                   const EGLint* attrib_list) {
        if (window) {
            auto* nativeWindow = (ANativeWindow*)window;
            int width = ANativeWindow_getWidth(nativeWindow);
            int height = ANativeWindow_getHeight(nativeWindow);
            Renderer::SetInputSurfaceSize(width, height);
            LOGI("eglCreateWindowSurface window=%p size=%dx%d", nativeWindow, width, height);
        }

        return orig_eglCreateWindowSurface(display, config, window, attrib_list);
    }

    static void DestroyImGuiContext(const char* reason) {
        if (!g_Initialized) {
            g_CurrentContext = EGL_NO_CONTEXT;
            g_CurrentSurface = EGL_NO_SURFACE;
            return;
        }

        LOGI("Destroying ImGui OpenGL ES context: %s", reason ? reason : "unknown");
        ImGui_ImplOpenGL3_Shutdown();
        Renderer::Images::OnImGuiContextDestroyed();
        ImGui::DestroyContext();
        g_Initialized = false;
        g_CurrentContext = EGL_NO_CONTEXT;
        g_CurrentSurface = EGL_NO_SURFACE;
    }

    static void SetupImGui() {
        if (g_Initialized) return;

        LOGI("Setting up ImGui for OpenGL ES...");

        g_CurrentContext = eglGetCurrentContext();
        g_CurrentSurface = eglGetCurrentSurface(EGL_DRAW);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_ScreenWidth, (float)g_ScreenHeight);
        io.IniFilename = nullptr;
        Renderer::SetupImGuiFonts();

        // Configure style
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.ScaleAllSizes(3.0f);

        ImGui_ImplOpenGL3_Init("#version 300 es");

        g_Initialized = true;
        LOGI("ImGui OpenGL ES initialized (screen: %dx%d)", g_ScreenWidth, g_ScreenHeight);
    }

    static void RenderFrame() {
        if (!g_Initialized) return;

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_ScreenWidth, (float)g_ScreenHeight);

        ImGui_ImplOpenGL3_NewFrame();
        Renderer::DrainInputEvents();
        ImGui::NewFrame();

        if (g_DrawCallback) {
            g_DrawCallback();
        }
        Renderer::UpdateInputCaptureState();

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        Renderer::Images::UpdateLifecycle();
    }

    static bool QuerySurfaceSize(EGLDisplay display, EGLSurface surface, int& width, int& height) {
        EGLint eglWidth = 0;
        EGLint eglHeight = 0;
        if (!eglQuerySurface(display, surface, EGL_WIDTH, &eglWidth) ||
            !eglQuerySurface(display, surface, EGL_HEIGHT, &eglHeight) ||
            eglWidth <= 0 || eglHeight <= 0) {
            return false;
        }

        width = eglWidth;
        height = eglHeight;
        return true;
    }

    static bool IsStableSwapTarget(EGLContext ctx, EGLSurface surface) {
        if (ctx == g_PendingContext && surface == g_PendingSurface) {
            ++g_StableSwapCount;
        } else {
            g_PendingContext = ctx;
            g_PendingSurface = surface;
            g_StableSwapCount = 1;
        }

        return g_StableSwapCount >= kRequiredStableSwaps;
    }

    // Hooked eglSwapBuffers
    static EGLBoolean hook_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
        EGLContext ctx = eglGetCurrentContext();
        EGLSurface drawSurface = eglGetCurrentSurface(EGL_DRAW);
        if (ctx == EGL_NO_CONTEXT || drawSurface == EGL_NO_SURFACE || drawSurface != surface) {
            return orig_eglSwapBuffers(display, surface);
        }

        int width = 0;
        int height = 0;
        if (!QuerySurfaceSize(display, surface, width, height)) {
            return orig_eglSwapBuffers(display, surface);
        }

        if (g_Initialized && (ctx != g_CurrentContext || surface != g_CurrentSurface)) {
            LOGI("Detected EGL target change ctx %p->%p surface %p->%p. Re-initializing ImGui...",
                 g_CurrentContext, ctx, g_CurrentSurface, surface);
            DestroyImGuiContext("EGL target changed");
        }

        // First call: try to claim as the active renderer
        if (!g_Claimed) {
            if (!IsStableSwapTarget(ctx, surface)) {
                return orig_eglSwapBuffers(display, surface);
            }

            if (Renderer::ClaimAPI(API::OPENGL_ES)) {
                g_Claimed = true;
                LOGI("eglSwapBuffers called! OpenGL ES is the active API.");
            } else {
                // Vulkan already claimed, just passthrough from now on
                return orig_eglSwapBuffers(display, surface);
            }
        }

        // If we're not the claimed API, passthrough
        if (Renderer::GetActiveAPI() != API::OPENGL_ES) {
            return orig_eglSwapBuffers(display, surface);
        }

        Renderer::OnFrameRendered(API::OPENGL_ES);

        g_ScreenWidth = width;
        g_ScreenHeight = height;

        // Initialize ImGui on first valid frame
        if (!g_Initialized && g_ScreenWidth > 0 && g_ScreenHeight > 0) {
            SetupImGui();
        }

        // Render ImGui overlay
        RenderFrame();

        return orig_eglSwapBuffers(display, surface);
    }

    bool Init() {
        void* egl_handle = dlopen("libEGL.so", RTLD_LAZY);
        if (!egl_handle) {
            LOGE("Failed to open libEGL.so");
            return false;
        }

        void* swap_addr = dlsym(egl_handle, "eglSwapBuffers");
        void* create_surface_addr = dlsym(egl_handle, "eglCreateWindowSurface");
        dlclose(egl_handle);

        if (!swap_addr) {
            LOGE("Failed to find eglSwapBuffers");
            return false;
        }

        LOGI("eglSwapBuffers at %p", swap_addr);

        int result = DobbyHook(swap_addr, (void*)hook_eglSwapBuffers, (void**)&orig_eglSwapBuffers);
        if (result != 0) {
            LOGE("DobbyHook failed for eglSwapBuffers (result: %d)", result);
            return false;
        }

        if (create_surface_addr) {
            int createResult = DobbyHook(create_surface_addr,
                                         (void*)hook_eglCreateWindowSurface,
                                         (void**)&orig_eglCreateWindowSurface);
            if (createResult == 0) {
                LOGI("eglCreateWindowSurface hooked");
            } else {
                LOGE("DobbyHook failed for eglCreateWindowSurface (result: %d)", createResult);
            }
        } else {
            LOGE("Failed to find eglCreateWindowSurface");
        }

        LOGI("eglSwapBuffers hooked (waiting for actual call...)");
        return true;
    }

    void Shutdown() {
        DestroyImGuiContext("OpenGL renderer shutdown");
        g_PendingContext = EGL_NO_CONTEXT;
        g_PendingSurface = EGL_NO_SURFACE;
        g_StableSwapCount = 0;

        if (orig_eglSwapBuffers) {
            void* egl_handle = dlopen("libEGL.so", RTLD_LAZY);
            if (egl_handle) {
                void* swap_addr = dlsym(egl_handle, "eglSwapBuffers");
                dlclose(egl_handle);
                if (swap_addr) DobbyDestroy(swap_addr);
            }
            orig_eglSwapBuffers = nullptr;
        }
    }

    void SetDrawCallback(DrawCallback callback) {
        g_DrawCallback = callback;
    }

    int GetScreenWidth() { return g_ScreenWidth; }
    int GetScreenHeight() { return g_ScreenHeight; }

    void HandleTouch(int action, float x, float y) {
        if (!g_Initialized) return;

        ImGuiIO& io = ImGui::GetIO();
        switch (action) {
            case 0: io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, true); break;
            case 1: io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, false); break;
            case 2: io.AddMousePosEvent(x, y); break;
            default: break;
        }
    }

} // namespace OpenGL
} // namespace Renderer
