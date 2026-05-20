#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdio>
#include <string>
#include <unistd.h>

#include "imgui.h"
#include "Renderer/log_config.hpp"
#include "Renderer/renderer.hpp"
#include "Renderer/input/input_handler.hpp"
#include "image_texture.hpp"

#define LOG_TAG "DualRenderImGui"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_DUAL_RENDER_IMGUI, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_DUAL_RENDER_IMGUI, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Menu State ──────────────────────────────────────────────────────────────────

static bool g_ShowMenu = true;

// ─── ImGui Draw Callback ─────────────────────────────────────────────────────────
// This is where you put your ImGui UI code.
// It gets called every frame by the renderer.

std::string ReadPackageName() {
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd < 0) {
        return {};
    }

    char buffer[256] = {};
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count <= 0) {
        return {};
    }

    std::string package(buffer);
    const size_t process_suffix = package.find(':');
    if (process_suffix != std::string::npos) {
        package.resize(process_suffix);
    }
    return package;
}

static void DrawMenu() {
    if (!g_ShowMenu) return;

    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("Dual Render ImGui Engine", &g_ShowMenu, ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Android ImGui renderer for OpenGL ES and Vulkan targets");
    ImGui::Separator();

    // Display active rendering API
    const char* apiName = "None";
    switch (Renderer::GetActiveAPI()) {
        case Renderer::API::OPENGL_ES: apiName = "OpenGL ES"; break;
        case Renderer::API::VULKAN:    apiName = "Vulkan"; break;
        default: break;
    }
    ImGui::Text("Render API: %s", apiName);
    ImGui::Text("Screen: %dx%d", Renderer::GetScreenWidth(), Renderer::GetScreenHeight());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Renderer")) {
        ImGui::Text("OpenGL ES: eglSwapBuffers hook");
        ImGui::Text("Vulkan: vkQueuePresentKHR hook");
        ImGui::Text("Input: native touch queue mapping");
    }

    if (ImGui::CollapsingHeader("About")) {
        ImGui::Text("Dual Render ImGui Engine");
        ImGui::Text("Runtime API selection for Android OpenGL ES and Vulkan.");
        ImGui::Text("ImGui v%s", ImGui::GetVersion());
    }

    static Renderer::Images::Texture logo;
    static bool logoLoadTried = false;

    if (!logoLoadTried) {
        logoLoadTried = true;
        std::string path = "/sdcard/Android/media/" + ReadPackageName() + "/logo.jpg";
        const bool loaded = Renderer::Images::LoadFromFile(path.c_str(), &logo);
        if (loaded) {
            LOGI("Loaded logo from file: %s", path.c_str());
        } else {
            LOGE("Failed to load logo from file: %s | %s", path.c_str(), Renderer::Images::GetLastError());
        }
    }

    if (logo.IsValid()) {
        Renderer::Images::Render(logo, ImVec2(180.0f, 180.0f));
    }




    ImGui::End();
}

// ─── Initialization Thread ───────────────────────────────────────────────────────
// Runs in background and initializes graphics/input hooks.

static void* InitThread(void*) {
    LOGI("Init thread started, hooking immediately (no delay)...");

    if (Renderer::Init()) {
        LOGI("Graphics hooks installed, waiting for render frames...");

        Renderer::SetDrawCallback(DrawMenu);
        Renderer::Input::Init();
    } else {
        LOGE("Failed to install any renderer hooks!");
    }

    return nullptr;
}

// ─── Library Entry Point ─────────────────────────────────────────────────────────
// Called when the .so is loaded into the target process.

__attribute__((constructor))
static void onLibraryLoad() {
    LOGI("=== Dual Render ImGui Engine Loaded ===");

    // Start initialization in a separate thread to avoid blocking
    pthread_t thread;
    pthread_create(&thread, nullptr, InitThread, nullptr);
    pthread_detach(thread);
}

// ─── JNI Touch Input Bridge ─────────────────────────────────────────────────────
// If injecting via an overlay service, forward touch events through JNI.

extern "C"
JNIEXPORT void JNICALL
Java_com_sahilm9098_dualrenderimgui_MainActivity_nativeOnTouch(
    JNIEnv* env, jobject thiz, jint action, jfloat x, jfloat y) {
    Renderer::HandleTouch(action, x, y);
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_sahilm9098_dualrenderimgui_MainActivity_nativeWantsCaptureInput(
    JNIEnv* env, jobject thiz) {
    return (jboolean)Renderer::WantsCaptureInput();
}
