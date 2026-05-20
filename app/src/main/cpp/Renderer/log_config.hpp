#pragma once

#include <android/log.h>

// Global log switch. Set to 0 to silence every project log tag.
#ifndef DRI_LOG_ALL
#define DRI_LOG_ALL 1
#endif

// Per-tag log switches. Set any tag to 0 to silence only that tag.
#ifndef DRI_LOG_DUAL_RENDER_IMGUI
#define DRI_LOG_DUAL_RENDER_IMGUI 1
#endif

#ifndef DRI_LOG_UNIVERSAL_RENDERER
#define DRI_LOG_UNIVERSAL_RENDERER 1
#endif

#ifndef DRI_LOG_INPUT_HANDLER
#define DRI_LOG_INPUT_HANDLER 1
#endif

#ifndef DRI_LOG_OPENGL_RENDERER
#define DRI_LOG_OPENGL_RENDERER 1
#endif

#ifndef DRI_LOG_VULKAN_RENDERER
#define DRI_LOG_VULKAN_RENDERER 1
#endif

#ifndef DRI_LOG_IMGUI_ANDROID_BACKEND
#define DRI_LOG_IMGUI_ANDROID_BACKEND 1
#endif

#ifndef DRI_LOG_IMAGE_TEXTURE
#define DRI_LOG_IMAGE_TEXTURE 1
#endif

#define DRI_LOG_PRINT(enabled, priority, tag, ...)        \
    do {                                                  \
        if (DRI_LOG_ALL && (enabled)) {                   \
            __android_log_print(priority, tag, __VA_ARGS__); \
        }                                                 \
    } while (0)
