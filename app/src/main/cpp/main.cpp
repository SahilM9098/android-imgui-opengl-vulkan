#include <jni.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <android/log.h>
#include <android/input.h>
#include <dlfcn.h>
#include <atomic>

#include "imgui.h"
#include "Renderer/renderer.hpp"
#include "dobby.h"

#define LOG_TAG "DualRenderImGui"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ─── Menu State ──────────────────────────────────────────────────────────────────

static bool g_ShowMenu = true;

// ─── Native Input Hook ──────────────────────────────────────────────────────────

using AInputQueue_getEvent_t = int32_t (*)(AInputQueue*, AInputEvent**);
static AInputQueue_getEvent_t orig_AInputQueue_getEvent = nullptr;
static std::atomic<int> g_InputLogCount{0};

static void QueueMotionEvent(const AInputEvent* event) {
    if (!event || AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION)
        return;

    int32_t rawAction = AMotionEvent_getAction(event);
    int32_t pointerIndex = (rawAction & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    int32_t action = rawAction & AMOTION_EVENT_ACTION_MASK;

    if (pointerIndex < 0 || (size_t)pointerIndex >= AMotionEvent_getPointerCount(event))
        pointerIndex = 0;

    float x = AMotionEvent_getX(event, pointerIndex);
    float y = AMotionEvent_getY(event, pointerIndex);

    switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
            Renderer::HandleTouch(0, x, y);
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_CANCEL:
            Renderer::HandleTouch(1, x, y);
            break;
        case AMOTION_EVENT_ACTION_MOVE:
        case AMOTION_EVENT_ACTION_HOVER_MOVE:
            Renderer::HandleTouch(2, AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));
            break;
        default:
            break;
    }

    int logCount = g_InputLogCount.fetch_add(1);
    if (logCount < 12) {
        LOGI("Input event action=%d pointer=%d x=%.1f y=%.1f", action, pointerIndex, x, y);
    }
}

static int32_t hook_AInputQueue_getEvent(AInputQueue* queue, AInputEvent** outEvent) {
    int32_t result = orig_AInputQueue_getEvent(queue, outEvent);
    if (result >= 0 && outEvent && *outEvent)
        QueueMotionEvent(*outEvent);
    return result;
}

static void InstallInputHooks() {
    void* libandroid = dlopen("libandroid.so", RTLD_NOW);
    if (!libandroid) {
        LOGE("Failed to open libandroid.so for input hook: %s", dlerror());
        return;
    }

    void* getEvent = dlsym(libandroid, "AInputQueue_getEvent");
    if (!getEvent) {
        LOGE("Failed to resolve AInputQueue_getEvent");
        return;
    }

    if (DobbyHook(getEvent, (void*)hook_AInputQueue_getEvent,
                  (void**)&orig_AInputQueue_getEvent) != 0) {
        LOGE("Failed to hook AInputQueue_getEvent at %p", getEvent);
        return;
    }

    LOGI("AInputQueue_getEvent hooked for ImGui touch input");
}

// ─── ImGui Draw Callback ─────────────────────────────────────────────────────────
// This is where you put your ImGui UI code.
// It gets called every frame by the renderer.

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

    ImGui::End();
}

// ─── Initialization Thread ───────────────────────────────────────────────────────
// Runs in background, waits for the game to load its graphics library,
// then initializes the renderer hooks.

static void* InitThread(void*) {
    LOGI("Init thread started, hooking immediately (no delay)...");

    // NO SLEEP - hook immediately so we catch vkCreateDevice/vkCreateSwapchainKHR
    // before the game calls them. The bootstrap already handles timing.

    // Initialize the universal renderer (hooks both OpenGL ES and Vulkan, first call wins)
    if (Renderer::Init()) {
        LOGI("Renderer hooks installed, waiting for game to render...");

        // Set our draw callback
        Renderer::SetDrawCallback(DrawMenu);
        InstallInputHooks();
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
