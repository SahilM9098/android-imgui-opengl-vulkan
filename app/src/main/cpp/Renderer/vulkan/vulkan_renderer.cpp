#include "vulkan_renderer.hpp"
#include "image_texture.hpp"
#include "imgui_fonts.hpp"
#include "log_config.hpp"
#include "renderer.hpp"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/log.h>
#include <android/native_window.h>
#include <dlfcn.h>
#include <vector>
#include <cstring>
#include <fstream>
#include <string>
#include <sstream>
#include <thread>
#include <chrono>
#include <mutex>
#include <atomic>
#include <link.h>
#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <algorithm>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "dobby.h"

#define LOG_TAG "VulkanRenderer"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_VULKAN_RENDERER, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_VULKAN_RENDERER, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace Renderer {
namespace Vulkan {

    static bool g_Initialized = false;
    static bool g_Claimed = false;
    static bool g_CreatingResources = false;
    static bool g_SettingUpImGui = false;
    static DrawCallback g_DrawCallback = nullptr;
    static std::mutex g_HookMutex;
    static std::atomic<bool> g_HooksInstalled{false};
    static std::atomic<bool> g_WaitThreadStarted{false};

    static VkInstance g_Instance = VK_NULL_HANDLE;
    static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
    static VkDevice g_Device = VK_NULL_HANDLE;
    static VkQueue g_Queue = VK_NULL_HANDLE;
    static uint32_t g_QueueFamily = 0;
    static bool g_HasQueueFamily = false;

    static VkSwapchainKHR g_Swapchain = VK_NULL_HANDLE;
    static VkFormat g_SwapchainFormat = VK_FORMAT_UNDEFINED;
    static VkExtent2D g_SwapchainExtent = {0, 0};
    static VkSurfaceTransformFlagBitsKHR g_SwapchainTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    static int g_SurfaceWidth = 0;
    static int g_SurfaceHeight = 0;
    static int g_DisplayWidth = 0;
    static int g_DisplayHeight = 0;
    static bool g_TransformLogged = false;
    static std::vector<VkImage> g_SwapchainImages;
    static std::vector<VkImageView> g_SwapchainImageViews;
    static std::vector<VkFramebuffer> g_Framebuffers;

    static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
    static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
    static VkCommandPool g_CommandPool = VK_NULL_HANDLE;
    static std::vector<VkCommandBuffer> g_CommandBuffers;
    static std::vector<VkFence> g_Fences;
    static std::vector<VkSemaphore> g_RenderCompleteSemaphores;
    static VkPipeline g_Pipeline = VK_NULL_HANDLE;
    static uint32_t g_CurrentImageIndex = 0;

    static PFN_vkCreateInstance orig_vkCreateInstance = nullptr;
    static PFN_vkCreateAndroidSurfaceKHR orig_vkCreateAndroidSurfaceKHR = nullptr;
    static PFN_vkCreateDevice orig_vkCreateDevice = nullptr;
    static PFN_vkGetDeviceQueue orig_vkGetDeviceQueue = nullptr;
    static PFN_vkGetDeviceQueue2 orig_vkGetDeviceQueue2 = nullptr;
    static PFN_vkCreateCommandPool orig_vkCreateCommandPool = nullptr;
    static PFN_vkCreateSwapchainKHR orig_vkCreateSwapchainKHR = nullptr;
    static PFN_vkAcquireNextImageKHR orig_vkAcquireNextImageKHR = nullptr;
    static PFN_vkAcquireNextImage2KHR orig_vkAcquireNextImage2KHR = nullptr;
    static PFN_vkQueuePresentKHR orig_vkQueuePresentKHR = nullptr;
    static PFN_vkGetDeviceProcAddr orig_vkGetDeviceProcAddr = nullptr;
    static PFN_vkGetInstanceProcAddr orig_vkGetInstanceProcAddr = nullptr;
    static void* (*orig_dlsym)(void*, const char*) = nullptr;
    static std::atomic<bool> g_DlsymHooked{false};

    // Forward declarations
    static VkResult hook_vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
    static VkResult hook_vkCreateAndroidSurfaceKHR(VkInstance, const VkAndroidSurfaceCreateInfoKHR*, const VkAllocationCallbacks*, VkSurfaceKHR*);
    static VkResult hook_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
    static void hook_vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue*);
    static void hook_vkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2*, VkQueue*);
    static VkResult hook_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*, VkCommandPool*);
    static VkResult hook_vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
    static VkResult hook_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t*);
    static VkResult hook_vkAcquireNextImage2KHR(VkDevice, const VkAcquireNextImageInfoKHR*, uint32_t*);
    static VkResult hook_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR*);
    static void* hook_dlsym(void*, const char*);
    static int HookLoadedLibraryGotSymbols(void* replacementGipa, void** originalGipa,
                                           void* replacementGdpa, void** originalGdpa);

    static bool TrySetupImGui();

    enum class DisplayTransform {
        Identity,
        Rotate90,
        Rotate180,
        Rotate270
    };

    static const char* TransformName(DisplayTransform transform) {
        switch (transform) {
            case DisplayTransform::Rotate90: return "rotate90";
            case DisplayTransform::Rotate180: return "rotate180";
            case DisplayTransform::Rotate270: return "rotate270";
            default: return "identity";
        }
    }

    static DisplayTransform GetDisplayTransform() {
        switch (g_SwapchainTransform) {
            case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
                return DisplayTransform::Rotate90;
            case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
                return DisplayTransform::Rotate180;
            case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
                return DisplayTransform::Rotate270;
            default:
                break;
        }

        if (g_SurfaceWidth > 0 && g_SurfaceHeight > 0 &&
            g_SwapchainExtent.width > 0 && g_SwapchainExtent.height > 0) {
            const bool surfaceLandscape = g_SurfaceWidth > g_SurfaceHeight;
            const bool swapchainLandscape = g_SwapchainExtent.width > g_SwapchainExtent.height;
            if (surfaceLandscape != swapchainLandscape)
                return surfaceLandscape ? DisplayTransform::Rotate90 : DisplayTransform::Rotate270;
        }

        return DisplayTransform::Identity;
    }

    static void UpdateDisplayGeometry() {
        int width = (int)g_SwapchainExtent.width;
        int height = (int)g_SwapchainExtent.height;
        const DisplayTransform transform = GetDisplayTransform();

        if (g_SurfaceWidth > 0 && g_SurfaceHeight > 0) {
            const bool rotated = transform == DisplayTransform::Rotate90 ||
                                 transform == DisplayTransform::Rotate270;
            const bool orientationMismatch =
                    (g_SurfaceWidth > g_SurfaceHeight) !=
                    (g_SwapchainExtent.width > g_SwapchainExtent.height);
            if (rotated || orientationMismatch) {
                width = g_SurfaceWidth;
                height = g_SurfaceHeight;
            }
        } else if (transform == DisplayTransform::Rotate90 || transform == DisplayTransform::Rotate270) {
            width = (int)g_SwapchainExtent.height;
            height = (int)g_SwapchainExtent.width;
        }

        if (width <= 0 || height <= 0) {
            width = (int)g_SwapchainExtent.width;
            height = (int)g_SwapchainExtent.height;
        }

        if (g_DisplayWidth == width && g_DisplayHeight == height && g_TransformLogged)
            return;

        g_DisplayWidth = width;
        g_DisplayHeight = height;
        LOGI("Vulkan display geometry: logical=%dx%d framebuffer=%ux%u transform=%s preTransform=0x%x",
             g_DisplayWidth, g_DisplayHeight,
             g_SwapchainExtent.width, g_SwapchainExtent.height,
             TransformName(transform), (unsigned)g_SwapchainTransform);
        g_TransformLogged = true;
    }

    static ImVec2 TransformPointToFramebuffer(const ImVec2& p) {
        const float lw = (float)std::max(1, g_DisplayWidth);
        const float lh = (float)std::max(1, g_DisplayHeight);
        const float fw = (float)std::max(1u, g_SwapchainExtent.width);
        const float fh = (float)std::max(1u, g_SwapchainExtent.height);

        switch (GetDisplayTransform()) {
            case DisplayTransform::Rotate90:
                return ImVec2((lh - p.y) * fw / lh, p.x * fh / lw);
            case DisplayTransform::Rotate180:
                return ImVec2((lw - p.x) * fw / lw, (lh - p.y) * fh / lh);
            case DisplayTransform::Rotate270:
                return ImVec2(p.y * fw / lh, (lw - p.x) * fh / lw);
            default:
                return ImVec2(p.x * fw / lw, p.y * fh / lh);
        }
    }

    static void TransformClipRectToFramebuffer(ImVec4& rect) {
        ImVec2 points[4] = {
                TransformPointToFramebuffer(ImVec2(rect.x, rect.y)),
                TransformPointToFramebuffer(ImVec2(rect.z, rect.y)),
                TransformPointToFramebuffer(ImVec2(rect.z, rect.w)),
                TransformPointToFramebuffer(ImVec2(rect.x, rect.w))
        };

        float minX = points[0].x;
        float minY = points[0].y;
        float maxX = points[0].x;
        float maxY = points[0].y;
        for (int i = 1; i < 4; ++i) {
            minX = std::min(minX, points[i].x);
            minY = std::min(minY, points[i].y);
            maxX = std::max(maxX, points[i].x);
            maxY = std::max(maxY, points[i].y);
        }

        rect = ImVec4(minX, minY, maxX, maxY);
    }

    static void PrepareDrawDataForFramebuffer(ImDrawData* drawData) {
        if (!drawData || g_DisplayWidth <= 0 || g_DisplayHeight <= 0 ||
            g_SwapchainExtent.width == 0 || g_SwapchainExtent.height == 0)
            return;

        const DisplayTransform transform = GetDisplayTransform();
        const bool rotated = transform == DisplayTransform::Rotate90 ||
                             transform == DisplayTransform::Rotate180 ||
                             transform == DisplayTransform::Rotate270;
        const bool scaled = g_DisplayWidth != (int)g_SwapchainExtent.width ||
                            g_DisplayHeight != (int)g_SwapchainExtent.height;

        if (!rotated && !scaled)
            return;

        for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
            ImDrawList* drawList = drawData->CmdLists[listIndex];
            for (int vertexIndex = 0; vertexIndex < drawList->VtxBuffer.Size; ++vertexIndex)
                drawList->VtxBuffer[vertexIndex].pos =
                        TransformPointToFramebuffer(drawList->VtxBuffer[vertexIndex].pos);

            for (int cmdIndex = 0; cmdIndex < drawList->CmdBuffer.Size; ++cmdIndex)
                TransformClipRectToFramebuffer(drawList->CmdBuffer[cmdIndex].ClipRect);
        }

        drawData->DisplayPos = ImVec2(0.0f, 0.0f);
        drawData->DisplaySize = ImVec2((float)g_SwapchainExtent.width,
                                       (float)g_SwapchainExtent.height);
        drawData->FramebufferScale = ImVec2(1.0f, 1.0f);
    }

    struct ScopedFlag {
        bool& value;
        explicit ScopedFlag(bool& flag) : value(flag) { value = true; }
        ~ScopedFlag() { value = false; }
    };

#define DRI_SET_ORIGINAL_IF_EMPTY(slot, value, type) \
    do {                                             \
        if (!(slot) && (value))                      \
            (slot) = (type)(value);                  \
    } while (0)

    static PFN_vkVoidFunction hook_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
        if (!pName) {
            return orig_vkGetDeviceProcAddr ? orig_vkGetDeviceProcAddr(device, pName) : nullptr;
        }
        PFN_vkVoidFunction real = orig_vkGetDeviceProcAddr ? orig_vkGetDeviceProcAddr(device, pName) : nullptr;
        if (strcmp(pName, "vkGetDeviceQueue") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue, real, PFN_vkGetDeviceQueue);
            return (PFN_vkVoidFunction)hook_vkGetDeviceQueue;
        }
        if (strcmp(pName, "vkGetDeviceQueue2") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue2, real, PFN_vkGetDeviceQueue2);
            return (PFN_vkVoidFunction)hook_vkGetDeviceQueue2;
        }
        if (strcmp(pName, "vkCreateCommandPool") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateCommandPool, real, PFN_vkCreateCommandPool);
            return (PFN_vkVoidFunction)hook_vkCreateCommandPool;
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            LOGI("Game requested vkQueuePresentKHR via vkGetDeviceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkQueuePresentKHR, real, PFN_vkQueuePresentKHR);
            return (PFN_vkVoidFunction)hook_vkQueuePresentKHR;
        }
        if (strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
            LOGI("Game requested vkAcquireNextImage2KHR via vkGetDeviceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImage2KHR, real, PFN_vkAcquireNextImage2KHR);
            return (PFN_vkVoidFunction)hook_vkAcquireNextImage2KHR;
        }
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
            LOGI("Game requested vkAcquireNextImageKHR via vkGetDeviceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImageKHR, real, PFN_vkAcquireNextImageKHR);
            return (PFN_vkVoidFunction)hook_vkAcquireNextImageKHR;
        }
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            LOGI("Game requested vkCreateSwapchainKHR via vkGetDeviceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateSwapchainKHR, real, PFN_vkCreateSwapchainKHR);
            return (PFN_vkVoidFunction)hook_vkCreateSwapchainKHR;
        }
        return real;
    }

    static PFN_vkVoidFunction hook_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
        if (!pName) {
            return orig_vkGetInstanceProcAddr ? orig_vkGetInstanceProcAddr(instance, pName) : nullptr;
        }
        PFN_vkVoidFunction real = orig_vkGetInstanceProcAddr ? orig_vkGetInstanceProcAddr(instance, pName) : nullptr;
        if (strcmp(pName, "vkCreateInstance") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateInstance, real, PFN_vkCreateInstance);
            return (PFN_vkVoidFunction)hook_vkCreateInstance;
        }
        if (strcmp(pName, "vkCreateAndroidSurfaceKHR") == 0) {
            LOGI("Game requested vkCreateAndroidSurfaceKHR via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateAndroidSurfaceKHR, real, PFN_vkCreateAndroidSurfaceKHR);
            return (PFN_vkVoidFunction)hook_vkCreateAndroidSurfaceKHR;
        }
        if (strcmp(pName, "vkCreateDevice") == 0) {
            LOGI("Game requested vkCreateDevice via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateDevice, real, PFN_vkCreateDevice);
            return (PFN_vkVoidFunction)hook_vkCreateDevice;
        }
        if (strcmp(pName, "vkGetDeviceQueue") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue, real, PFN_vkGetDeviceQueue);
            return (PFN_vkVoidFunction)hook_vkGetDeviceQueue;
        }
        if (strcmp(pName, "vkGetDeviceQueue2") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue2, real, PFN_vkGetDeviceQueue2);
            return (PFN_vkVoidFunction)hook_vkGetDeviceQueue2;
        }
        if (strcmp(pName, "vkCreateCommandPool") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateCommandPool, real, PFN_vkCreateCommandPool);
            return (PFN_vkVoidFunction)hook_vkCreateCommandPool;
        }
        if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
            LOGI("Game requested vkCreateSwapchainKHR via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateSwapchainKHR, real, PFN_vkCreateSwapchainKHR);
            return (PFN_vkVoidFunction)hook_vkCreateSwapchainKHR;
        }
        if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
            LOGI("Game requested vkAcquireNextImageKHR via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImageKHR, real, PFN_vkAcquireNextImageKHR);
            return (PFN_vkVoidFunction)hook_vkAcquireNextImageKHR;
        }
        if (strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
            LOGI("Game requested vkAcquireNextImage2KHR via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImage2KHR, real, PFN_vkAcquireNextImage2KHR);
            return (PFN_vkVoidFunction)hook_vkAcquireNextImage2KHR;
        }
        if (strcmp(pName, "vkQueuePresentKHR") == 0) {
            LOGI("Game requested vkQueuePresentKHR via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkQueuePresentKHR, real, PFN_vkQueuePresentKHR);
            return (PFN_vkVoidFunction)hook_vkQueuePresentKHR;
        }
        if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
            LOGI("Game requested vkGetDeviceProcAddr via vkGetInstanceProcAddr -> returning our hook");
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceProcAddr, real, PFN_vkGetDeviceProcAddr);
            return (PFN_vkVoidFunction)hook_vkGetDeviceProcAddr;
        }
        return real;
    }

    // Find libvulkan.so in /proc/self/maps and dlopen it with full path
    static void* GetVulkanHandle(bool logMissing = true) {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        std::string vulkanPath;

        while (std::getline(maps, line)) {
            if (line.find("libvulkan.so") != std::string::npos) {
                // Extract the path (last field after the permissions and offsets)
                size_t pos = line.find('/');
                if (pos != std::string::npos) {
                    vulkanPath = line.substr(pos);
                    // Remove trailing newline or spaces
                    while (!vulkanPath.empty() && (vulkanPath.back() == '\n' || vulkanPath.back() == '\r' || vulkanPath.back() == ' '))
                        vulkanPath.pop_back();
                    break;
                }
            }
        }

        if (vulkanPath.empty()) {
            if (logMissing) LOGE("libvulkan.so not found in /proc/self/maps");
            return nullptr;
        }

        LOGI("Found libvulkan.so at: %s", vulkanPath.c_str());

        // Try dlopen with full path - bypasses namespace restrictions
        void* handle = dlopen(vulkanPath.c_str(), RTLD_LAZY);
        if (!handle) {
            LOGE("dlopen(%s) failed: %s", vulkanPath.c_str(), dlerror());
            // Fallback: try with basename
            handle = dlopen("libvulkan.so", RTLD_LAZY);
            if (!handle) {
                LOGE("dlopen(libvulkan.so) also failed: %s", dlerror());
            }
        }

        return handle;
    }

    static PFN_vkVoidFunction GetInstanceProc(VkInstance instance, const char* name) {
        if (orig_vkGetInstanceProcAddr)
            return orig_vkGetInstanceProcAddr(instance, name);
        return nullptr;
    }

    static PFN_vkVoidFunction GetDeviceProc(VkDevice device, const char* name) {
        if (orig_vkGetDeviceProcAddr && device)
            return orig_vkGetDeviceProcAddr(device, name);
        if (orig_vkGetInstanceProcAddr)
            return orig_vkGetInstanceProcAddr(g_Instance, name);
        return nullptr;
    }

    static void CaptureQueueFamily(uint32_t queueFamily) {
        g_QueueFamily = queueFamily;
        g_HasQueueFamily = true;
    }

    static void CaptureQueue(VkDevice device, uint32_t queueFamily, VkQueue queue) {
        if (device != VK_NULL_HANDLE)
            g_Device = device;
        CaptureQueueFamily(queueFamily);
        if (queue != VK_NULL_HANDLE)
            g_Queue = queue;
        TrySetupImGui();
    }

    static void DestroyResources() {
        if (g_Device == VK_NULL_HANDLE)
            return;

        for (auto sem : g_RenderCompleteSemaphores)
            if (sem != VK_NULL_HANDLE) vkDestroySemaphore(g_Device, sem, nullptr);
        g_RenderCompleteSemaphores.clear();

        for (auto f : g_Fences)
            if (f != VK_NULL_HANDLE) vkDestroyFence(g_Device, f, nullptr);
        g_Fences.clear();

        if (g_CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_Device, g_CommandPool, nullptr);
            g_CommandPool = VK_NULL_HANDLE;
        }

        for (auto fb : g_Framebuffers)
            if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(g_Device, fb, nullptr);
        g_Framebuffers.clear();

        for (auto iv : g_SwapchainImageViews)
            if (iv != VK_NULL_HANDLE) vkDestroyImageView(g_Device, iv, nullptr);
        g_SwapchainImageViews.clear();

        if (g_RenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(g_Device, g_RenderPass, nullptr);
            g_RenderPass = VK_NULL_HANDLE;
        }

        if (g_DescriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
            g_DescriptorPool = VK_NULL_HANDLE;
        }

        g_CommandBuffers.clear();
    }

    static void ResetImGuiForSwapchain() {
        if (g_Device == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(g_Device);
        if (g_Initialized) {
            ImGui_ImplVulkan_Shutdown();
            Renderer::Images::OnImGuiContextDestroyed();
            ImGui::DestroyContext();
            g_Initialized = false;
        }
        DestroyResources();
    }

    static bool CreateResources() {
        if (g_CreatingResources)
            return false;
        if (g_Device == VK_NULL_HANDLE || g_Swapchain == VK_NULL_HANDLE ||
            g_SwapchainFormat == VK_FORMAT_UNDEFINED || g_SwapchainExtent.width == 0 ||
            g_SwapchainExtent.height == 0 || g_SwapchainImages.empty() || !g_HasQueueFamily) {
            LOGE("CreateResources skipped: incomplete Vulkan state");
            return false;
        }
        ScopedFlag creating(g_CreatingResources);

        VkAttachmentDescription att{};
        att.format = g_SwapchainFormat;
        att.samples = VK_SAMPLE_COUNT_1_BIT;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rpci{};
        rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpci.attachmentCount = 1; rpci.pAttachments = &att;
        rpci.subpassCount = 1; rpci.pSubpasses = &sub;
        rpci.dependencyCount = 1; rpci.pDependencies = &dep;
        VkResult err = vkCreateRenderPass(g_Device, &rpci, nullptr, &g_RenderPass);
        if (err != VK_SUCCESS) {
            LOGE("vkCreateRenderPass failed: %d", err);
            return false;
        }

        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 100}};
        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        dpci.maxSets = 100; dpci.poolSizeCount = 1; dpci.pPoolSizes = sizes;
        err = vkCreateDescriptorPool(g_Device, &dpci, nullptr, &g_DescriptorPool);
        if (err != VK_SUCCESS) {
            LOGE("vkCreateDescriptorPool failed: %d", err);
            return false;
        }

        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = g_QueueFamily;
        err = vkCreateCommandPool(g_Device, &cpci, nullptr, &g_CommandPool);
        if (err != VK_SUCCESS) {
            LOGE("vkCreateCommandPool failed: %d", err);
            return false;
        }

        uint32_t n = (uint32_t)g_SwapchainImages.size();
        g_SwapchainImageViews.resize(n);
        g_Framebuffers.resize(n);
        g_CommandBuffers.resize(n);
        g_Fences.resize(n);
        g_RenderCompleteSemaphores.resize(n);

        for (uint32_t i = 0; i < n; i++) {
            VkImageViewCreateInfo ivci{};
            ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image = g_SwapchainImages[i];
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = g_SwapchainFormat;
            ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            err = vkCreateImageView(g_Device, &ivci, nullptr, &g_SwapchainImageViews[i]);
            if (err != VK_SUCCESS) {
                LOGE("vkCreateImageView[%u] failed: %d", i, err);
                return false;
            }

            VkFramebufferCreateInfo fbci{};
            fbci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbci.renderPass = g_RenderPass;
            fbci.attachmentCount = 1; fbci.pAttachments = &g_SwapchainImageViews[i];
            fbci.width = g_SwapchainExtent.width;
            fbci.height = g_SwapchainExtent.height;
            fbci.layers = 1;
            err = vkCreateFramebuffer(g_Device, &fbci, nullptr, &g_Framebuffers[i]);
            if (err != VK_SUCCESS) {
                LOGE("vkCreateFramebuffer[%u] failed: %d", i, err);
                return false;
            }

            VkFenceCreateInfo fci{};
            fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            err = vkCreateFence(g_Device, &fci, nullptr, &g_Fences[i]);
            if (err != VK_SUCCESS) {
                LOGE("vkCreateFence[%u] failed: %d", i, err);
                return false;
            }

            VkSemaphoreCreateInfo sci{};
            sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            err = vkCreateSemaphore(g_Device, &sci, nullptr, &g_RenderCompleteSemaphores[i]);
            if (err != VK_SUCCESS) {
                LOGE("vkCreateSemaphore[%u] failed: %d", i, err);
                return false;
            }
        }

        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_CommandPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = n;
        err = vkAllocateCommandBuffers(g_Device, &cbai, g_CommandBuffers.data());
        if (err != VK_SUCCESS) {
            LOGE("vkAllocateCommandBuffers failed: %d", err);
            return false;
        }

        return true;
    }

    static bool SetupImGui() {
        if (g_SettingUpImGui)
            return false;
        if (g_PhysicalDevice == VK_NULL_HANDLE) {
            LOGE("Cannot initialize ImGui Vulkan: missed VkPhysicalDevice. Inject before vkCreateDevice.");
            return false;
        }
        if (g_RenderPass == VK_NULL_HANDLE || g_DescriptorPool == VK_NULL_HANDLE ||
            g_Device == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE || !g_HasQueueFamily) {
            LOGE("Cannot initialize ImGui Vulkan: incomplete device/queue/swapchain state");
            return false;
        }
        if (g_SwapchainImages.size() < 2) {
            LOGE("Cannot initialize ImGui Vulkan: swapchain image count is %zu", g_SwapchainImages.size());
            return false;
        }

        IMGUI_CHECKVERSION();
        ScopedFlag settingUp(g_SettingUpImGui);
        UpdateDisplayGeometry();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)g_DisplayWidth, (float)g_DisplayHeight);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.IniFilename = nullptr;
        Renderer::SetupImGuiFonts();
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(3.0f);

        ImGui_ImplVulkan_InitInfo ii{};
        memset(&ii, 0, sizeof(ii));
        ii.ApiVersion = VK_API_VERSION_1_0;
        // The bundled ImGui backend asserts Instance is non-null but does not use it after init.
        // Late loaders may miss vkCreateInstance, so keep rendering possible if PhysicalDevice was captured.
        ii.Instance = g_Instance != VK_NULL_HANDLE ? g_Instance : (VkInstance)g_PhysicalDevice;
        ii.PhysicalDevice = g_PhysicalDevice;
        ii.Device = g_Device;
        ii.QueueFamily = g_QueueFamily;
        ii.Queue = g_Queue;
        ii.DescriptorPool = g_DescriptorPool;
        ii.MinImageCount = 2;
        ii.ImageCount = (uint32_t)g_SwapchainImages.size();
        ii.PipelineInfoMain.RenderPass = g_RenderPass;
        ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ii.UseDynamicRendering = false;

        if (!ImGui_ImplVulkan_Init(&ii)) {
            LOGE("ImGui_ImplVulkan_Init FAILED");
            ImGui::DestroyContext();
            return false;
        }

        // Font atlas is auto-created by ImGui_ImplVulkan_NewFrame() in v1.92+

        g_Initialized = true;
        LOGI("=== ImGui Vulkan READY === logical=%dx%d framebuffer=%ux%u",
             g_DisplayWidth, g_DisplayHeight, g_SwapchainExtent.width, g_SwapchainExtent.height);
        return true;
    }

    static bool TrySetupImGui() {
        if (g_Initialized)
            return true;
        if (g_CreatingResources || g_SettingUpImGui)
            return false;
        if (g_Device == VK_NULL_HANDLE || g_Queue == VK_NULL_HANDLE || !g_HasQueueFamily ||
            g_Swapchain == VK_NULL_HANDLE || g_SwapchainImages.empty())
            return false;
        if (g_CommandBuffers.empty() && !CreateResources()) {
            DestroyResources();
            return false;
        }
        return SetupImGui();
    }

    static VkResult hook_vkCreateInstance(const VkInstanceCreateInfo* ci,
                                           const VkAllocationCallbacks* a,
                                           VkInstance* instance) {
        LOGI("vkCreateInstance called, dynamically re-scanning GOT tables for newly loaded libraries...");
        HookLoadedLibraryGotSymbols((void*)hook_vkGetInstanceProcAddr,
                                    (void**)&orig_vkGetInstanceProcAddr,
                                    (void*)hook_vkGetDeviceProcAddr,
                                    (void**)&orig_vkGetDeviceProcAddr);

        PFN_vkCreateInstance realCreate = orig_vkCreateInstance;
        if (!realCreate)
            realCreate = (PFN_vkCreateInstance)GetInstanceProc(nullptr, "vkCreateInstance");
        if (!realCreate)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realCreate(ci, a, instance);
        if (r == VK_SUCCESS && instance && *instance != VK_NULL_HANDLE) {
            g_Instance = *instance;
            LOGI("vkCreateInstance captured: instance=%p", (void*)g_Instance);
        }
        return r;
    }

    static VkResult hook_vkCreateAndroidSurfaceKHR(VkInstance instance,
                                                    const VkAndroidSurfaceCreateInfoKHR* ci,
                                                    const VkAllocationCallbacks* a,
                                                    VkSurfaceKHR* surface) {
        PFN_vkCreateAndroidSurfaceKHR realFunc = orig_vkCreateAndroidSurfaceKHR;
        if (!realFunc)
            realFunc = (PFN_vkCreateAndroidSurfaceKHR)GetInstanceProc(instance, "vkCreateAndroidSurfaceKHR");
        if (!realFunc)
            return VK_ERROR_INITIALIZATION_FAILED;

        if (ci && ci->window) {
            int width = ANativeWindow_getWidth(ci->window);
            int height = ANativeWindow_getHeight(ci->window);
            g_SurfaceWidth = width;
            g_SurfaceHeight = height;
            g_TransformLogged = false;
            Renderer::SetInputSurfaceSize(width, height);
            LOGI("vkCreateAndroidSurfaceKHR window=%p size=%dx%d", ci->window, width, height);
            if (g_SwapchainExtent.width > 0 && g_SwapchainExtent.height > 0)
                UpdateDisplayGeometry();
        }

        return realFunc(instance, ci, a, surface);
    }

    static VkResult hook_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci,
                                          const VkAllocationCallbacks* a, VkDevice* dev) {
        PFN_vkCreateDevice realCreate = orig_vkCreateDevice;
        if (!realCreate)
            realCreate = (PFN_vkCreateDevice)GetInstanceProc(g_Instance, "vkCreateDevice");
        if (!realCreate)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realCreate(pd, ci, a, dev);
        if (r == VK_SUCCESS && dev && *dev != VK_NULL_HANDLE) {
            g_PhysicalDevice = pd;
            g_Device = *dev;
            if (ci) {
                for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
                    if (ci->pQueueCreateInfos[i].queueCount > 0) {
                        CaptureQueueFamily(ci->pQueueCreateInfos[i].queueFamilyIndex);
                        break;
                    }
                }
            }

            if (g_HasQueueFamily) {
                PFN_vkGetDeviceQueue realGetQueue = orig_vkGetDeviceQueue;
                if (!realGetQueue)
                    realGetQueue = (PFN_vkGetDeviceQueue)GetDeviceProc(g_Device, "vkGetDeviceQueue");
                if (realGetQueue)
                    realGetQueue(g_Device, g_QueueFamily, 0, &g_Queue);
            }

            LOGI("vkCreateDevice captured: physical=%p device=%p queue=%p queueFamily=%u",
                 (void*)g_PhysicalDevice, (void*)g_Device, (void*)g_Queue, g_QueueFamily);
            TrySetupImGui();
        }
        return r;
    }

    static void hook_vkGetDeviceQueue(VkDevice dev, uint32_t queueFamilyIndex,
                                      uint32_t queueIndex, VkQueue* queue) {
        PFN_vkGetDeviceQueue realFunc = orig_vkGetDeviceQueue;
        if (!realFunc)
            realFunc = (PFN_vkGetDeviceQueue)GetDeviceProc(dev, "vkGetDeviceQueue");
        if (!realFunc)
            return;

        realFunc(dev, queueFamilyIndex, queueIndex, queue);
        if (queue && *queue != VK_NULL_HANDLE) {
            LOGI("vkGetDeviceQueue captured: device=%p queue=%p queueFamily=%u index=%u",
                 (void*)dev, (void*)*queue, queueFamilyIndex, queueIndex);
            CaptureQueue(dev, queueFamilyIndex, *queue);
        }
    }

    static void hook_vkGetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* queue) {
        PFN_vkGetDeviceQueue2 realFunc = orig_vkGetDeviceQueue2;
        if (!realFunc)
            realFunc = (PFN_vkGetDeviceQueue2)GetDeviceProc(dev, "vkGetDeviceQueue2");
        if (!realFunc)
            return;

        realFunc(dev, info, queue);
        if (info && queue && *queue != VK_NULL_HANDLE) {
            LOGI("vkGetDeviceQueue2 captured: device=%p queue=%p queueFamily=%u index=%u",
                 (void*)dev, (void*)*queue, info->queueFamilyIndex, info->queueIndex);
            CaptureQueue(dev, info->queueFamilyIndex, *queue);
        }
    }

    static VkResult hook_vkCreateCommandPool(VkDevice dev, const VkCommandPoolCreateInfo* ci,
                                             const VkAllocationCallbacks* a, VkCommandPool* pool) {
        PFN_vkCreateCommandPool realFunc = orig_vkCreateCommandPool;
        if (!realFunc)
            realFunc = (PFN_vkCreateCommandPool)GetDeviceProc(dev, "vkCreateCommandPool");
        if (!realFunc)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realFunc(dev, ci, a, pool);
        if (r == VK_SUCCESS && ci) {
            if (!g_HasQueueFamily)
                CaptureQueueFamily(ci->queueFamilyIndex);
            if (g_Device == VK_NULL_HANDLE)
                g_Device = dev;
            if (!g_CreatingResources && !g_SettingUpImGui)
                TrySetupImGui();
        }
        return r;
    }

    static VkResult hook_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci,
                                                const VkAllocationCallbacks* a, VkSwapchainKHR* sc) {
        PFN_vkCreateSwapchainKHR realFunc = orig_vkCreateSwapchainKHR;
        if (!realFunc)
            realFunc = (PFN_vkCreateSwapchainKHR)GetDeviceProc(dev, "vkCreateSwapchainKHR");
        if (!realFunc) return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realFunc(dev, ci, a, sc);
        if (r == VK_SUCCESS && ci && sc && *sc != VK_NULL_HANDLE) {
            if (g_Initialized || !g_CommandBuffers.empty())
                ResetImGuiForSwapchain();

            if (g_Device == VK_NULL_HANDLE)
                g_Device = dev;
            if (!g_HasQueueFamily && ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT &&
                ci->queueFamilyIndexCount > 0 && ci->pQueueFamilyIndices) {
                CaptureQueueFamily(ci->pQueueFamilyIndices[0]);
            }

            g_Swapchain = *sc;
            g_SwapchainFormat = ci->imageFormat;
            g_SwapchainExtent = ci->imageExtent;
            g_SwapchainTransform = ci->preTransform;
            g_TransformLogged = false;
            UpdateDisplayGeometry();
            uint32_t cnt = 0;
            vkGetSwapchainImagesKHR(dev, g_Swapchain, &cnt, nullptr);
            g_SwapchainImages.resize(cnt);
            vkGetSwapchainImagesKHR(dev, g_Swapchain, &cnt, g_SwapchainImages.data());
            LOGI("vkCreateSwapchainKHR: fmt=%d %ux%u imgs=%u preTransform=0x%x",
                 g_SwapchainFormat, g_SwapchainExtent.width, g_SwapchainExtent.height,
                 cnt, (unsigned)g_SwapchainTransform);
            TrySetupImGui();
        }
        return r;
    }

    static VkResult hook_vkAcquireNextImageKHR(VkDevice dev, VkSwapchainKHR sc, uint64_t t,
                                                 VkSemaphore sem, VkFence f, uint32_t* idx) {
        PFN_vkAcquireNextImageKHR realFunc = orig_vkAcquireNextImageKHR;
        if (!realFunc)
            realFunc = (PFN_vkAcquireNextImageKHR)GetDeviceProc(dev, "vkAcquireNextImageKHR");
        if (!realFunc) return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realFunc(dev, sc, t, sem, f, idx);
        if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && idx)
            g_CurrentImageIndex = *idx;
        return r;
    }

    static VkResult hook_vkAcquireNextImage2KHR(VkDevice dev, const VkAcquireNextImageInfoKHR* info,
                                                uint32_t* idx) {
        PFN_vkAcquireNextImage2KHR realFunc = orig_vkAcquireNextImage2KHR;
        if (!realFunc)
            realFunc = (PFN_vkAcquireNextImage2KHR)GetDeviceProc(dev, "vkAcquireNextImage2KHR");
        if (!realFunc)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkResult r = realFunc(dev, info, idx);
        if ((r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) && idx)
            g_CurrentImageIndex = *idx;
        return r;
    }

    static VkResult hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pi) {
        // Track heartbeat if active or none claimed yet
        if (Renderer::GetActiveAPI() == API::NONE || Renderer::GetActiveAPI() == API::VULKAN) {
            Renderer::OnFrameRendered(API::VULKAN);
        }

        PFN_vkQueuePresentKHR realFunc = orig_vkQueuePresentKHR;
        if (!realFunc && g_Device)
            realFunc = (PFN_vkQueuePresentKHR)GetDeviceProc(g_Device, "vkQueuePresentKHR");
        if (!realFunc) return VK_ERROR_INITIALIZATION_FAILED;

        if (queue != VK_NULL_HANDLE)
            g_Queue = queue;
        if (pi && pi->swapchainCount > 0 && pi->pImageIndices) {
            g_CurrentImageIndex = pi->pImageIndices[0];
            if (g_Swapchain == VK_NULL_HANDLE && pi->pSwapchains) {
                static bool warnedMissedSwapchain = false;
                if (!warnedMissedSwapchain) {
                    LOGE("vkQueuePresentKHR seen, but swapchain creation was missed. Inject earlier or wait for swapchain recreation.");
                    warnedMissedSwapchain = true;
                }
            }
        }

        if (!g_Claimed) {
            if (Renderer::ClaimAPI(API::VULKAN)) {
                g_Claimed = true;
                LOGI(">>> Vulkan claimed as active renderer <<<");
            } else {
                return realFunc(queue, pi);
            }
        }

        if (Renderer::GetActiveAPI() != API::VULKAN) {
            return realFunc(queue, pi);
        }

        TrySetupImGui();

        if (g_Initialized && g_CurrentImageIndex < g_CommandBuffers.size() &&
            g_CurrentImageIndex < g_Framebuffers.size() &&
            g_CurrentImageIndex < g_RenderCompleteSemaphores.size()) {
            uint32_t imageIndex = g_CurrentImageIndex;
            UpdateDisplayGeometry();
            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)g_DisplayWidth, (float)g_DisplayHeight);
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);

            vkWaitForFences(g_Device, 1, &g_Fences[imageIndex], VK_TRUE, UINT64_MAX);
            vkResetFences(g_Device, 1, &g_Fences[imageIndex]);

            VkCommandBuffer cmd = g_CommandBuffers[imageIndex];
            vkResetCommandBuffer(cmd, 0);

            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &bi);

            VkRenderPassBeginInfo rp{};
            rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rp.renderPass = g_RenderPass;
            rp.framebuffer = g_Framebuffers[imageIndex];
            rp.renderArea.extent = g_SwapchainExtent;
            rp.renderArea.offset = {0, 0};
            rp.clearValueCount = 0;
            rp.pClearValues = nullptr;
            vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport viewport{};
            viewport.x = 0;
            viewport.y = 0;
            viewport.width = (float)g_SwapchainExtent.width;
            viewport.height = (float)g_SwapchainExtent.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &viewport);

            VkRect2D scissor{};
            scissor.offset = {0, 0};
            scissor.extent = g_SwapchainExtent;
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            ImGui_ImplVulkan_NewFrame();
            Renderer::DrainInputEvents();
            ImGui::NewFrame();
            if (g_DrawCallback) g_DrawCallback();
            Renderer::UpdateInputCaptureState();
            ImGui::EndFrame();
            ImGui::Render();
            ImDrawData* drawData = ImGui::GetDrawData();
            PrepareDrawDataForFramebuffer(drawData);
            ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
            Renderer::Images::UpdateLifecycle();

            vkCmdEndRenderPass(cmd);
            vkEndCommandBuffer(cmd);

            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            std::vector<VkPipelineStageFlags> waitStages;
            if (pi && pi->waitSemaphoreCount > 0 && pi->pWaitSemaphores) {
                waitStages.resize(pi->waitSemaphoreCount, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                si.waitSemaphoreCount = pi->waitSemaphoreCount;
                si.pWaitSemaphores = pi->pWaitSemaphores;
                si.pWaitDstStageMask = waitStages.data();
            }
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            VkSemaphore renderDone = g_RenderCompleteSemaphores[imageIndex];
            si.signalSemaphoreCount = 1;
            si.pSignalSemaphores = &renderDone;

            VkResult submitResult = vkQueueSubmit(queue, 1, &si, g_Fences[imageIndex]);
            if (submitResult == VK_SUCCESS && pi) {
                VkPresentInfoKHR patchedPresent = *pi;
                patchedPresent.waitSemaphoreCount = 1;
                patchedPresent.pWaitSemaphores = &renderDone;
                return realFunc(queue, &patchedPresent);
            }
            if (submitResult != VK_SUCCESS)
                LOGE("vkQueueSubmit for ImGui failed: %d", submitResult);
        }

        return realFunc(queue, pi);
    }

    static bool HookSymbol(void* handle, const char* name, void* replacement, void** original) {
        void* addr = orig_dlsym ? orig_dlsym(handle, name) : dlsym(handle, name);
        if (!addr) {
            LOGE("%s not exported by libvulkan.so", name);
            return false;
        }
        if (DobbyHook(addr, replacement, original) == 0) {
            LOGI("Hooked %s at %p, orig=%p", name, addr, original ? *original : nullptr);
            return true;
        }
        LOGE("Failed to hook %s at %p", name, addr);
        return false;
    }

    static void* hook_dlsym(void* handle, const char* symbol) {
        if (!orig_dlsym)
            return nullptr;

        void* real = orig_dlsym(handle, symbol);
        if (!symbol || !real)
            return real;

        // If Vulkan symbols are requested, scan/re-scan GOT tables to patch newly loaded libraries like libUnreal.so or libunity.so
        if (symbol[0] == 'v' && symbol[1] == 'k') {
            LOGI("dlsym requested %s, dynamically re-scanning GOT tables for new libraries...", symbol);
            HookLoadedLibraryGotSymbols((void*)hook_vkGetInstanceProcAddr,
                                        (void**)&orig_vkGetInstanceProcAddr,
                                        (void*)hook_vkGetDeviceProcAddr,
                                        (void**)&orig_vkGetDeviceProcAddr);
        }

        if (strcmp(symbol, "vkGetInstanceProcAddr") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetInstanceProcAddr, real, PFN_vkGetInstanceProcAddr);
            LOGI("dlsym intercepted vkGetInstanceProcAddr -> returning hook");
            return (void*)hook_vkGetInstanceProcAddr;
        }
        if (strcmp(symbol, "vkGetDeviceProcAddr") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceProcAddr, real, PFN_vkGetDeviceProcAddr);
            LOGI("dlsym intercepted vkGetDeviceProcAddr -> returning hook");
            return (void*)hook_vkGetDeviceProcAddr;
        }
        if (strcmp(symbol, "vkCreateInstance") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateInstance, real, PFN_vkCreateInstance);
            LOGI("dlsym intercepted vkCreateInstance -> returning hook");
            return (void*)hook_vkCreateInstance;
        }
        if (strcmp(symbol, "vkCreateAndroidSurfaceKHR") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateAndroidSurfaceKHR, real, PFN_vkCreateAndroidSurfaceKHR);
            LOGI("dlsym intercepted vkCreateAndroidSurfaceKHR -> returning hook");
            return (void*)hook_vkCreateAndroidSurfaceKHR;
        }
        if (strcmp(symbol, "vkCreateDevice") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateDevice, real, PFN_vkCreateDevice);
            LOGI("dlsym intercepted vkCreateDevice -> returning hook");
            return (void*)hook_vkCreateDevice;
        }
        if (strcmp(symbol, "vkGetDeviceQueue") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue, real, PFN_vkGetDeviceQueue);
            return (void*)hook_vkGetDeviceQueue;
        }
        if (strcmp(symbol, "vkGetDeviceQueue2") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkGetDeviceQueue2, real, PFN_vkGetDeviceQueue2);
            return (void*)hook_vkGetDeviceQueue2;
        }
        if (strcmp(symbol, "vkCreateCommandPool") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateCommandPool, real, PFN_vkCreateCommandPool);
            return (void*)hook_vkCreateCommandPool;
        }
        if (strcmp(symbol, "vkCreateSwapchainKHR") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkCreateSwapchainKHR, real, PFN_vkCreateSwapchainKHR);
            LOGI("dlsym intercepted vkCreateSwapchainKHR -> returning hook");
            return (void*)hook_vkCreateSwapchainKHR;
        }
        if (strcmp(symbol, "vkAcquireNextImageKHR") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImageKHR, real, PFN_vkAcquireNextImageKHR);
            LOGI("dlsym intercepted vkAcquireNextImageKHR -> returning hook");
            return (void*)hook_vkAcquireNextImageKHR;
        }
        if (strcmp(symbol, "vkAcquireNextImage2KHR") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkAcquireNextImage2KHR, real, PFN_vkAcquireNextImage2KHR);
            LOGI("dlsym intercepted vkAcquireNextImage2KHR -> returning hook");
            return (void*)hook_vkAcquireNextImage2KHR;
        }
        if (strcmp(symbol, "vkQueuePresentKHR") == 0) {
            DRI_SET_ORIGINAL_IF_EMPTY(orig_vkQueuePresentKHR, real, PFN_vkQueuePresentKHR);
            LOGI("dlsym intercepted vkQueuePresentKHR -> returning hook");
            return (void*)hook_vkQueuePresentKHR;
        }

        return real;
    }

    static bool InstallDlsymHook() {
        if (g_DlsymHooked.load())
            return true;

        void* dlsymAddr = DobbySymbolResolver("libdl.so", "dlsym");
        if (!dlsymAddr) {
            void* dlHandle = dlopen("libdl.so", RTLD_NOW);
            if (dlHandle)
                dlsymAddr = dlsym(dlHandle, "dlsym");
        }
        if (!dlsymAddr) {
            LOGE("Failed to resolve libdl.so!dlsym");
            return false;
        }

        if (DobbyHook(dlsymAddr, (void*)hook_dlsym, (void**)&orig_dlsym) != 0) {
            LOGE("Failed to hook libdl.so!dlsym at %p", dlsymAddr);
            return false;
        }

        g_DlsymHooked.store(true);
        LOGI("Hooked libdl.so!dlsym for Vulkan symbol interception");
        return true;
    }

    struct GotHookRequest {
        const char* image;
        const char* symbol;
        void* replacement;
        void** original;
        bool hooked;
    };

#ifdef __LP64__
#define DRI_R_SYM(info) ELF64_R_SYM(info)
#else
#define DRI_R_SYM(info) ELF32_R_SYM(info)
#endif

    static uintptr_t DynamicPtrToAddress(const dl_phdr_info* info, ElfW(Addr) ptr) {
        if (ptr == 0)
            return 0;
        uintptr_t value = (uintptr_t)ptr;
        uintptr_t base = (uintptr_t)info->dlpi_addr;
        return value < base ? base + value : value;
    }

    static int GetMemoryProtection(void* address) {
        std::ifstream maps("/proc/self/maps");
        std::string line;
        uintptr_t target = (uintptr_t)address;

        while (std::getline(maps, line)) {
            uintptr_t start = 0;
            uintptr_t end = 0;
            char perms[5] = {};
            if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, perms) != 3)
                continue;
            if (target < start || target >= end)
                continue;

            int prot = 0;
            if (perms[0] == 'r') prot |= PROT_READ;
            if (perms[1] == 'w') prot |= PROT_WRITE;
            if (perms[2] == 'x') prot |= PROT_EXEC;
            return prot;
        }

        return PROT_READ;
    }

    static bool PatchGotSlot(void** slot, void* replacement, void** original) {
        if (!slot || *slot == replacement)
            return false;

        if (original && !*original)
            *original = *slot;

        long pageSize = sysconf(_SC_PAGESIZE);
        if (pageSize <= 0)
            pageSize = 4096;

        int oldProt = GetMemoryProtection(slot);
        int patchProt = oldProt | PROT_READ | PROT_WRITE;
        uintptr_t page = (uintptr_t)slot & ~((uintptr_t)pageSize - 1);
        if (mprotect((void*)page, (size_t)pageSize, patchProt) != 0) {
            LOGE("mprotect GOT RW failed: errno=%d", errno);
            return false;
        }

        *slot = replacement;
        __builtin___clear_cache((char*)slot, (char*)slot + sizeof(void*));

        if (mprotect((void*)page, (size_t)pageSize, oldProt) != 0)
            LOGE("mprotect GOT RO failed: errno=%d", errno);
        return true;
    }

    static bool IsAppLibraryForGotHook(const char* imageName) {
        if (!imageName || imageName[0] == '\0')
            return false;
        if (strstr(imageName, "libmenu.so"))
            return false;
        if (strstr(imageName, "/data/app/") ||
            strstr(imageName, "/data/user/") ||
            strstr(imageName, "/data/data/") ||
            strstr(imageName, "/mnt/expand/")) {
            return true;
        }
        return false;
    }

    static bool TryPatchRelaTable(const dl_phdr_info* info, GotHookRequest* request,
                                  const char* imageName,
                                  ElfW(Rela)* rela, size_t relaCount,
                                  ElfW(Sym)* symtab, const char* strtab) {
        if (!rela || !symtab || !strtab)
            return false;

        bool patchedAny = false;
        for (size_t i = 0; i < relaCount; i++) {
            size_t symIndex = DRI_R_SYM(rela[i].r_info);
            if (symIndex == 0)
                continue;

            const char* name = strtab + symtab[symIndex].st_name;
            if (strcmp(name, request->symbol) != 0)
                continue;

            void** slot = (void**)((uintptr_t)info->dlpi_addr + (uintptr_t)rela[i].r_offset);
            if (PatchGotSlot(slot, request->replacement, request->original)) {
                LOGI("GOT (RELA) hooked %s!%s at %p", imageName, request->symbol, slot);
                request->hooked = true;
                patchedAny = true;
            }
        }

        return patchedAny;
    }

    static bool TryPatchRelTable(const dl_phdr_info* info, GotHookRequest* request,
                                 const char* imageName,
                                 ElfW(Rel)* rel, size_t relCount,
                                 ElfW(Sym)* symtab, const char* strtab) {
        if (!rel || !symtab || !strtab)
            return false;

        bool patchedAny = false;
        for (size_t i = 0; i < relCount; i++) {
            size_t symIndex = DRI_R_SYM(rel[i].r_info);
            if (symIndex == 0)
                continue;

            const char* name = strtab + symtab[symIndex].st_name;
            if (strcmp(name, request->symbol) != 0)
                continue;

            void** slot = (void**)((uintptr_t)info->dlpi_addr + (uintptr_t)rel[i].r_offset);
            if (PatchGotSlot(slot, request->replacement, request->original)) {
                LOGI("GOT (REL) hooked %s!%s at %p", imageName, request->symbol, slot);
                request->hooked = true;
                patchedAny = true;
            }
        }

        return patchedAny;
    }

    static int GotHookPhdrCallback(dl_phdr_info* info, size_t, void* data) {
        auto* request = (GotHookRequest*)data;
        const char* imageName = info->dlpi_name ? info->dlpi_name : "";
        if (request->image && request->image[0] != '\0') {
            if (strstr(imageName, request->image) == nullptr)
                return 0;
        } else {
            if (!IsAppLibraryForGotHook(imageName)) {
                return 0;
            }
        }

        ElfW(Dyn)* dynamic = nullptr;
        for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
            if (phdr.p_type == PT_DYNAMIC) {
                dynamic = (ElfW(Dyn)*)((uintptr_t)info->dlpi_addr + phdr.p_vaddr);
                break;
            }
        }

        if (!dynamic)
            return 0;

        ElfW(Sym)* symtab = nullptr;
        const char* strtab = nullptr;
        void* jmprel = nullptr;
        size_t jmprelSize = 0;
        uintptr_t pltrelFormat = DT_RELA;
        ElfW(Rela)* rela = nullptr;
        size_t relaSize = 0;
        ElfW(Rel)* rel = nullptr;
        size_t relSize = 0;

        for (ElfW(Dyn)* dyn = dynamic; dyn->d_tag != DT_NULL; dyn++) {
            switch (dyn->d_tag) {
                case DT_SYMTAB:
                    symtab = (ElfW(Sym)*)DynamicPtrToAddress(info, dyn->d_un.d_ptr);
                    break;
                case DT_STRTAB:
                    strtab = (const char*)DynamicPtrToAddress(info, dyn->d_un.d_ptr);
                    break;
                case DT_JMPREL:
                    jmprel = (void*)DynamicPtrToAddress(info, dyn->d_un.d_ptr);
                    break;
                case DT_PLTRELSZ:
                    jmprelSize = dyn->d_un.d_val;
                    break;
                case DT_PLTREL:
                    pltrelFormat = dyn->d_un.d_val;
                    break;
                case DT_RELA:
                    rela = (ElfW(Rela)*)DynamicPtrToAddress(info, dyn->d_un.d_ptr);
                    break;
                case DT_RELASZ:
                    relaSize = dyn->d_un.d_val;
                    break;
                case DT_REL:
                    rel = (ElfW(Rel)*)DynamicPtrToAddress(info, dyn->d_un.d_ptr);
                    break;
                case DT_RELSZ:
                    relSize = dyn->d_un.d_val;
                    break;
                default:
                    break;
            }
        }

        if (jmprel && jmprelSize > 0) {
            if (pltrelFormat == DT_REL) {
                size_t jmprelCount = jmprelSize / sizeof(ElfW(Rel));
                TryPatchRelTable(info, request, imageName, (ElfW(Rel)*)jmprel, jmprelCount, symtab, strtab);
            } else {
                size_t jmprelCount = jmprelSize / sizeof(ElfW(Rela));
                TryPatchRelaTable(info, request, imageName, (ElfW(Rela)*)jmprel, jmprelCount, symtab, strtab);
            }
        }

        if (rela && relaSize > 0) {
            size_t relaCount = relaSize / sizeof(ElfW(Rela));
            TryPatchRelaTable(info, request, imageName, rela, relaCount, symtab, strtab);
        }

        if (rel && relSize > 0) {
            size_t relCount = relSize / sizeof(ElfW(Rel));
            TryPatchRelTable(info, request, imageName, rel, relCount, symtab, strtab);
        }

        return 0;
    }

    static bool HookGotSymbol(const char* image, const char* symbol, void* replacement, void** original) {
        GotHookRequest request{image, symbol, replacement, original, false};
        dl_iterate_phdr(GotHookPhdrCallback, &request);
        return request.hooked;
    }

    static int HookLoadedLibraryGotSymbols(void* replacementGipa, void** originalGipa,
                                           void* replacementGdpa, void** originalGdpa) {
        int hooked = 0;
        if (HookGotSymbol("", "vkGetInstanceProcAddr", replacementGipa, originalGipa))
            hooked++;
        if (HookGotSymbol("", "vkGetDeviceProcAddr", replacementGdpa, originalGdpa))
            hooked++;
        if (HookGotSymbol("", "vkCreateInstance",
                          (void*)hook_vkCreateInstance,
                          (void**)&orig_vkCreateInstance))
            hooked++;
        if (HookGotSymbol("", "vkCreateAndroidSurfaceKHR",
                          (void*)hook_vkCreateAndroidSurfaceKHR,
                          (void**)&orig_vkCreateAndroidSurfaceKHR))
            hooked++;
        if (HookGotSymbol("", "vkCreateDevice",
                          (void*)hook_vkCreateDevice,
                          (void**)&orig_vkCreateDevice))
            hooked++;
        if (HookGotSymbol("", "vkGetDeviceQueue",
                          (void*)hook_vkGetDeviceQueue,
                          (void**)&orig_vkGetDeviceQueue))
            hooked++;
        if (HookGotSymbol("", "vkGetDeviceQueue2",
                          (void*)hook_vkGetDeviceQueue2,
                          (void**)&orig_vkGetDeviceQueue2))
            hooked++;
        if (HookGotSymbol("", "vkCreateCommandPool",
                          (void*)hook_vkCreateCommandPool,
                          (void**)&orig_vkCreateCommandPool))
            hooked++;
        if (HookGotSymbol("", "vkCreateSwapchainKHR",
                          (void*)hook_vkCreateSwapchainKHR,
                          (void**)&orig_vkCreateSwapchainKHR))
            hooked++;
        if (HookGotSymbol("", "vkAcquireNextImageKHR",
                          (void*)hook_vkAcquireNextImageKHR,
                          (void**)&orig_vkAcquireNextImageKHR))
            hooked++;
        if (HookGotSymbol("", "vkAcquireNextImage2KHR",
                          (void*)hook_vkAcquireNextImage2KHR,
                          (void**)&orig_vkAcquireNextImage2KHR))
            hooked++;
        if (HookGotSymbol("", "vkQueuePresentKHR",
                          (void*)hook_vkQueuePresentKHR,
                          (void**)&orig_vkQueuePresentKHR))
            hooked++;
        return hooked;
    }

    static bool InstallVulkanHooks(void* vkHandle) {
        std::lock_guard<std::mutex> lock(g_HookMutex);
        if (g_HooksInstalled.load())
            return true;
        if (!vkHandle)
            return false;

        if (vkHandle) {
            void* pGIPA = orig_dlsym ? orig_dlsym(vkHandle, "vkGetInstanceProcAddr")
                                     : dlsym(vkHandle, "vkGetInstanceProcAddr");
            void* pGDPA = orig_dlsym ? orig_dlsym(vkHandle, "vkGetDeviceProcAddr")
                                     : dlsym(vkHandle, "vkGetDeviceProcAddr");
            if (pGIPA)
                orig_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)pGIPA;
            if (pGDPA)
                orig_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)pGDPA;
            if (!pGIPA || !pGDPA)
                LOGE("Failed to pre-resolve Vulkan proc-address functions: GIPA=%p GDPA=%p", pGIPA, pGDPA);
        }

        int ok = 0;
        ok += InstallDlsymHook() ? 1 : 0;
        ok += HookLoadedLibraryGotSymbols((void*)hook_vkGetInstanceProcAddr,
                                          (void**)&orig_vkGetInstanceProcAddr,
                                          (void*)hook_vkGetDeviceProcAddr,
                                          (void**)&orig_vkGetDeviceProcAddr);

        if (ok > 0) {
            g_HooksInstalled.store(true);
            LOGI("Installed Vulkan symbol interception (%d hooks)", ok);
            return true;
        }

        return false;
    }

    static void StartVulkanWaitThread() {
        bool expected = false;
        if (!g_WaitThreadStarted.compare_exchange_strong(expected, true))
            return;

        std::thread([]() {
            for (int i = 0; i < 300 && !g_HooksInstalled.load(); i++) {
                void* vkHandle = GetVulkanHandle(false);
                if (vkHandle && InstallVulkanHooks(vkHandle)) {
                    LOGI("Vulkan hooks installed after waiting for libraries");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!g_HooksInstalled.load())
                LOGE("Timed out waiting for libvulkan.so or Vulkan imports");
        }).detach();
    }

    bool Init() {
        void* vkHandle = GetVulkanHandle(false);
        if (vkHandle) {
            if (InstallVulkanHooks(vkHandle))
                return true;
            LOGI("Vulkan loaded, but Vulkan imports are not ready yet; waiting in background");
            StartVulkanWaitThread();
            return true;
        }

        LOGI("libvulkan.so not loaded yet; waiting in background");
        StartVulkanWaitThread();
        return true;
    }

    void Shutdown() {
        if (g_Initialized) {
            vkDeviceWaitIdle(g_Device);
            ImGui_ImplVulkan_Shutdown();
            Renderer::Images::OnImGuiContextDestroyed();
            ImGui::DestroyContext();
            g_Initialized = false;
        }

        if (g_Pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(g_Device, g_Pipeline, nullptr);
            g_Pipeline = VK_NULL_HANDLE;
        }

        DestroyResources();

        g_SwapchainImages.clear();
        g_Instance = VK_NULL_HANDLE;
        g_PhysicalDevice = VK_NULL_HANDLE;
        g_Device = VK_NULL_HANDLE;
        g_Queue = VK_NULL_HANDLE;
        g_HasQueueFamily = false;
        g_Swapchain = VK_NULL_HANDLE;
        g_SwapchainExtent = {0, 0};
        g_SwapchainTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        g_SurfaceWidth = 0;
        g_SurfaceHeight = 0;
        g_DisplayWidth = 0;
        g_DisplayHeight = 0;
        g_TransformLogged = false;
    }

    void SetDrawCallback(DrawCallback cb) { g_DrawCallback = cb; }
    int GetScreenWidth() {
        if (g_DisplayWidth <= 0)
            UpdateDisplayGeometry();
        return g_DisplayWidth > 0 ? g_DisplayWidth : (int)g_SwapchainExtent.width;
    }

    int GetScreenHeight() {
        if (g_DisplayHeight <= 0)
            UpdateDisplayGeometry();
        return g_DisplayHeight > 0 ? g_DisplayHeight : (int)g_SwapchainExtent.height;
    }

    void HandleTouch(int action, float x, float y) {
        if (!g_Initialized) return;
        ImGuiIO& io = ImGui::GetIO();
        switch (action) {
            case 0: io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, true); break;
            case 1: io.AddMousePosEvent(x, y); io.AddMouseButtonEvent(0, false); break;
            case 2: io.AddMousePosEvent(x, y); break;
        }
    }

} // namespace Vulkan
} // namespace Renderer
