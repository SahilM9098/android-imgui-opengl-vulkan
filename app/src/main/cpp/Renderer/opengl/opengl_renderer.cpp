#include "opengl_renderer.hpp"
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
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace Renderer {
namespace OpenGL {

    // State
    static bool g_Initialized = false;
    static bool g_Claimed = false;
    static int g_ScreenWidth = 0;
    static int g_ScreenHeight = 0;
    static DrawCallback g_DrawCallback = nullptr;

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

    static void SetupImGui() {
        if (g_Initialized) return;

        LOGI("Setting up ImGui for OpenGL ES...");

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_ScreenWidth, (float)g_ScreenHeight);
        io.IniFilename = nullptr;

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

        ImGui::EndFrame();
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    // Hooked eglSwapBuffers
    static EGLBoolean hook_eglSwapBuffers(EGLDisplay display, EGLSurface surface) {
        // First call: try to claim as the active renderer
        if (!g_Claimed) {
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

        // Query surface dimensions
        EGLint width = 0, height = 0;
        eglQuerySurface(display, surface, EGL_WIDTH, &width);
        eglQuerySurface(display, surface, EGL_HEIGHT, &height);

        if (width > 0 && height > 0) {
            g_ScreenWidth = width;
            g_ScreenHeight = height;
        }

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
        if (g_Initialized) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui::DestroyContext();
            g_Initialized = false;
        }

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
