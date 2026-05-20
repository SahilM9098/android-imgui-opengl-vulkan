#include "image_texture.hpp"

#include "log_config.hpp"

#include "imgui_internal.h"

#include <android/log.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#define LOG_TAG "ImageTexture"
#define LOGI(...) DRI_LOG_PRINT(DRI_LOG_IMAGE_TEXTURE, ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) DRI_LOG_PRINT(DRI_LOG_IMAGE_TEXTURE, ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct AImageDecoder;
struct AImageDecoderHeaderInfo;

namespace Renderer {
namespace Images {
namespace {

    constexpr int kDecoderSuccess = 0;
    constexpr int32_t kAndroidBitmapFormatRgba8888 = 1;
    constexpr int kDestroyDelayFrames = 6;

    struct DecodedImage {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> pixels;
    };

    struct DecoderApi {
        bool attempted = false;
        bool available = false;
        void* handle = nullptr;

        int (*createFromFd)(int fd, AImageDecoder** outDecoder) = nullptr;
        int (*createFromBuffer)(const void* buffer, size_t length, AImageDecoder** outDecoder) = nullptr;
        void (*destroy)(AImageDecoder* decoder) = nullptr;
        const AImageDecoderHeaderInfo* (*getHeaderInfo)(const AImageDecoder* decoder) = nullptr;
        int32_t (*getWidth)(const AImageDecoderHeaderInfo* info) = nullptr;
        int32_t (*getHeight)(const AImageDecoderHeaderInfo* info) = nullptr;
        int (*setFormat)(AImageDecoder* decoder, int32_t format) = nullptr;
        int (*setUnpremultiplied)(AImageDecoder* decoder, bool required) = nullptr;
        size_t (*getMinimumStride)(AImageDecoder* decoder) = nullptr;
        int (*decodeImage)(AImageDecoder* decoder, void* pixels, size_t stride, size_t size) = nullptr;
    };

    struct PendingDestroy {
        ImTextureData* data = nullptr;
        int frames = 0;
        bool destroyRequested = false;
    };

    struct TrackedTexture {
        ImTextureData* data = nullptr;
        ImGuiContext* context = nullptr;
        uint32_t generation = 0;
    };

    static DecoderApi g_DecoderApi;
    static std::vector<PendingDestroy> g_PendingDestroys;
    static std::vector<TrackedTexture> g_TrackedTextures;
    static std::mutex g_TextureMutex;
    static std::atomic<uint32_t> g_ContextGeneration{1};
    static std::string g_LastError;

    static void SetLastError(const char* format, ...) {
        char buffer[768];
        va_list args;
        va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);
        g_LastError = buffer;
        LOGE("%s", buffer);
    }

    template <typename T>
    static T ResolveSymbol(void* handle, const char* name) {
        return reinterpret_cast<T>(dlsym(handle, name));
    }

    static bool ResolveDecoderApi() {
        DecoderApi& api = g_DecoderApi;
        if (api.attempted)
            return api.available;
        api.attempted = true;

        api.handle = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
        if (!api.handle) {
            LOGI("Android ImageDecoder unavailable; using stb_image fallback");
            return false;
        }

        api.createFromFd = ResolveSymbol<decltype(api.createFromFd)>(api.handle, "AImageDecoder_createFromFd");
        api.createFromBuffer = ResolveSymbol<decltype(api.createFromBuffer)>(api.handle, "AImageDecoder_createFromBuffer");
        api.destroy = ResolveSymbol<decltype(api.destroy)>(api.handle, "AImageDecoder_delete");
        api.getHeaderInfo = ResolveSymbol<decltype(api.getHeaderInfo)>(api.handle, "AImageDecoder_getHeaderInfo");
        api.getWidth = ResolveSymbol<decltype(api.getWidth)>(api.handle, "AImageDecoderHeaderInfo_getWidth");
        api.getHeight = ResolveSymbol<decltype(api.getHeight)>(api.handle, "AImageDecoderHeaderInfo_getHeight");
        api.setFormat = ResolveSymbol<decltype(api.setFormat)>(api.handle, "AImageDecoder_setAndroidBitmapFormat");
        api.setUnpremultiplied = ResolveSymbol<decltype(api.setUnpremultiplied)>(api.handle, "AImageDecoder_setUnpremultipliedRequired");
        api.getMinimumStride = ResolveSymbol<decltype(api.getMinimumStride)>(api.handle, "AImageDecoder_getMinimumStride");
        api.decodeImage = ResolveSymbol<decltype(api.decodeImage)>(api.handle, "AImageDecoder_decodeImage");

        api.available = api.createFromFd && api.createFromBuffer && api.destroy &&
                        api.getHeaderInfo && api.getWidth && api.getHeight &&
                        api.setFormat && api.decodeImage;
        if (!api.available)
            LOGI("Android ImageDecoder symbols missing; using stb_image fallback");
        return api.available;
    }

    static bool HasContext(const char* operation) {
        if (ImGui::GetCurrentContext())
            return true;
        SetLastError("%s requires an active ImGui context; call it from the draw callback/render thread", operation);
        return false;
    }

    static bool ValidateSize(int width, int height, size_t* outSize) {
        if (width <= 0 || height <= 0) {
            SetLastError("Invalid image size: %dx%d", width, height);
            return false;
        }
        if (width > 65535 || height > 65535) {
            SetLastError("Image too large for ImGui texture update rects: %dx%d", width, height);
            return false;
        }

        const size_t rowBytes = static_cast<size_t>(width) * 4u;
        if (rowBytes > std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
            SetLastError("Image size overflow: %dx%d", width, height);
            return false;
        }

        *outSize = rowBytes * static_cast<size_t>(height);
        return true;
    }

    static bool ReadFileBytes(const char* path, std::vector<unsigned char>* outData) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            SetLastError("open image failed: %s (%s)", path, std::strerror(errno));
            return false;
        }

        struct stat st {};
        if (fstat(fd, &st) != 0) {
            SetLastError("fstat image failed: %s (%s)", path, std::strerror(errno));
            close(fd);
            return false;
        }

        if (st.st_size <= 0) {
            SetLastError("image file is empty: %s", path);
            close(fd);
            return false;
        }

        const auto fileSize = static_cast<size_t>(st.st_size);
        outData->resize(fileSize);

        size_t offset = 0;
        while (offset < fileSize) {
            ssize_t bytesRead = read(fd, outData->data() + offset, fileSize - offset);
            if (bytesRead < 0) {
                if (errno == EINTR)
                    continue;
                SetLastError("read image failed: %s (%s)", path, std::strerror(errno));
                close(fd);
                return false;
            }
            if (bytesRead == 0)
                break;
            offset += static_cast<size_t>(bytesRead);
        }

        close(fd);
        if (offset != fileSize) {
            SetLastError("image read incomplete: %s (%zu/%zu bytes)", path, offset, fileSize);
            return false;
        }

        LOGI("Read image file: %s (%zu bytes)", path, fileSize);
        return true;
    }

    static bool DecodeToRGBA(AImageDecoder* decoder, DecodedImage* outImage) {
        DecoderApi& api = g_DecoderApi;
        const AImageDecoderHeaderInfo* info = api.getHeaderInfo(decoder);
        if (!info) {
            SetLastError("AImageDecoder_getHeaderInfo failed");
            return false;
        }

        const int width = api.getWidth(info);
        const int height = api.getHeight(info);
        size_t compactSize = 0;
        if (!ValidateSize(width, height, &compactSize))
            return false;

        int result = api.setFormat(decoder, kAndroidBitmapFormatRgba8888);
        if (result != kDecoderSuccess) {
            SetLastError("AImageDecoder_setAndroidBitmapFormat failed: %d", result);
            return false;
        }

        if (api.setUnpremultiplied)
            api.setUnpremultiplied(decoder, true);

        const size_t compactStride = static_cast<size_t>(width) * 4u;
        size_t decodeStride = compactStride;
        if (api.getMinimumStride)
            decodeStride = api.getMinimumStride(decoder);
        if (decodeStride < compactStride)
            decodeStride = compactStride;

        if (decodeStride > std::numeric_limits<size_t>::max() / static_cast<size_t>(height)) {
            SetLastError("Decoded image stride overflow: %zu x %d", decodeStride, height);
            return false;
        }

        std::vector<unsigned char> decoded(decodeStride * static_cast<size_t>(height));
        result = api.decodeImage(decoder, decoded.data(), decodeStride, decoded.size());
        if (result != kDecoderSuccess) {
            SetLastError("AImageDecoder_decodeImage failed: %d", result);
            return false;
        }

        outImage->width = width;
        outImage->height = height;
        if (decodeStride == compactStride) {
            outImage->pixels = std::move(decoded);
            return true;
        }

        outImage->pixels.resize(compactSize);
        for (int y = 0; y < height; ++y) {
            std::memcpy(outImage->pixels.data() + static_cast<size_t>(y) * compactStride,
                        decoded.data() + static_cast<size_t>(y) * decodeStride,
                        compactStride);
        }
        return true;
    }

    static bool TryDecodeWithAndroid(const void* encodedData, size_t encodedSize, DecodedImage* outImage) {
        if (!ResolveDecoderApi())
            return false;

        AImageDecoder* decoder = nullptr;
        int result = g_DecoderApi.createFromBuffer(encodedData, encodedSize, &decoder);
        if (result != kDecoderSuccess || !decoder) {
            SetLastError("AImageDecoder_createFromBuffer failed: %d", result);
            return false;
        }

        const bool decoded = DecodeToRGBA(decoder, outImage);
        g_DecoderApi.destroy(decoder);
        return decoded;
    }

    static bool TryDecodeWithStb(const void* encodedData, size_t encodedSize, DecodedImage* outImage) {
        if (encodedSize > static_cast<size_t>(std::numeric_limits<int>::max())) {
            SetLastError("encoded image too large for stb_image: %zu bytes", encodedSize);
            return false;
        }

        int width = 0;
        int height = 0;
        int components = 0;
        unsigned char* pixels = stbi_load_from_memory(
                static_cast<const stbi_uc*>(encodedData),
                static_cast<int>(encodedSize),
                &width,
                &height,
                &components,
                4);
        if (!pixels) {
            SetLastError("stb_image decode failed: %s", stbi_failure_reason());
            return false;
        }

        size_t dataSize = 0;
        if (!ValidateSize(width, height, &dataSize)) {
            stbi_image_free(pixels);
            return false;
        }

        outImage->width = width;
        outImage->height = height;
        outImage->pixels.assign(pixels, pixels + dataSize);
        stbi_image_free(pixels);
        LOGI("Decoded image with stb_image: %dx%d (%d components)", width, height, components);
        return true;
    }

    static bool DecodeMemoryToRGBA(const void* encodedData, size_t encodedSize, DecodedImage* outImage) {
        if (TryDecodeWithAndroid(encodedData, encodedSize, outImage))
            return true;
        return TryDecodeWithStb(encodedData, encodedSize, outImage);
    }

    static ImVec2 ResolveRenderSize(const Texture& texture, const ImVec2& requested) {
        if (requested.x > 0.0f && requested.y > 0.0f)
            return requested;

        const float width = static_cast<float>(texture.Width);
        const float height = static_cast<float>(texture.Height);
        if (requested.x > 0.0f && width > 0.0f)
            return ImVec2(requested.x, requested.x * height / width);
        if (requested.y > 0.0f && height > 0.0f)
            return ImVec2(requested.y * width / height, requested.y);
        return ImVec2(width, height);
    }

    static bool IsPendingDestroy(ImTextureData* data) {
        for (const PendingDestroy& pending : g_PendingDestroys)
            if (pending.data == data)
                return true;
        return false;
    }

    static void ClearGpuHandle(Texture* texture) {
        texture->Data = nullptr;
        texture->OwnerContext = nullptr;
        texture->Generation = 0;
    }

    static void ClearTexture(Texture* texture) {
        ClearGpuHandle(texture);
        texture->Source = SourceKind::NONE;
        texture->SourceData.clear();
        texture->SourcePath.clear();
        texture->Width = 0;
        texture->Height = 0;
    }

    static void TrackTexture(ImTextureData* data, ImGuiContext* context, uint32_t generation) {
        std::lock_guard<std::mutex> lock(g_TextureMutex);
        for (TrackedTexture& tracked : g_TrackedTextures) {
            if (tracked.data == data) {
                tracked.context = context;
                tracked.generation = generation;
                return;
            }
        }
        g_TrackedTextures.push_back(TrackedTexture{data, context, generation});
    }

    static void UntrackTexture(ImTextureData* data) {
        std::lock_guard<std::mutex> lock(g_TextureMutex);
        g_TrackedTextures.erase(
                std::remove_if(g_TrackedTextures.begin(), g_TrackedTextures.end(),
                               [data](const TrackedTexture& tracked) {
                                   return tracked.data == data;
                               }),
                g_TrackedTextures.end());
    }

    static bool HasCurrentTextureData(const Texture& texture) {
        return texture.Data != nullptr &&
               texture.OwnerContext == ImGui::GetCurrentContext() &&
               texture.Generation == g_ContextGeneration.load(std::memory_order_acquire) &&
               texture.Data->Status != ImTextureStatus_Destroyed;
    }

    static void ReleaseGpuTexture(Texture* texture) {
        if (!texture)
            return;

        ImTextureData* data = texture->Data;
        if (data && HasCurrentTextureData(*texture)) {
            if (!IsPendingDestroy(data))
                g_PendingDestroys.push_back(PendingDestroy{data, 0, false});
        }

        ClearGpuHandle(texture);
    }

    static bool UploadRGBA(Texture* texture, const unsigned char* rgbaPixels, int width, int height) {
        if (!texture || !rgbaPixels)
            return false;
        if (!HasContext("UploadRGBA"))
            return false;

        size_t dataSize = 0;
        if (!ValidateSize(width, height, &dataSize))
            return false;

        ReleaseGpuTexture(texture);

        ImTextureData* data = IM_NEW(ImTextureData)();
        data->Create(ImTextureFormat_RGBA32, width, height);
        std::memcpy(data->Pixels, rgbaPixels, dataSize);
        data->UseColors = true;
        ImGui::RegisterUserTexture(data);

        texture->Data = data;
        texture->OwnerContext = ImGui::GetCurrentContext();
        texture->Generation = g_ContextGeneration.load(std::memory_order_acquire);
        TrackTexture(data, texture->OwnerContext, texture->Generation);
        LOGI("Image texture queued: %dx%d", width, height);
        return true;
    }

    static bool DecodeTextureSource(Texture* texture, DecodedImage* outImage) {
        if (!texture || !outImage)
            return false;

        switch (texture->Source) {
            case SourceKind::RGBA_PIXELS: {
                size_t dataSize = 0;
                if (!ValidateSize(texture->Width, texture->Height, &dataSize))
                    return false;
                if (texture->SourceData.size() != dataSize) {
                    SetLastError("RGBA source size mismatch: %zu/%zu bytes",
                                 texture->SourceData.size(), dataSize);
                    return false;
                }
                outImage->width = texture->Width;
                outImage->height = texture->Height;
                outImage->pixels = texture->SourceData;
                return true;
            }

            case SourceKind::ENCODED_BYTES:
            case SourceKind::FILE_BYTES:
                if (!texture->SourceData.empty())
                    return DecodeMemoryToRGBA(texture->SourceData.data(), texture->SourceData.size(), outImage);
                if (!texture->SourcePath.empty()) {
                    std::vector<unsigned char> encodedData;
                    if (!ReadFileBytes(texture->SourcePath.c_str(), &encodedData))
                        return false;
                    texture->SourceData = std::move(encodedData);
                    return DecodeMemoryToRGBA(texture->SourceData.data(), texture->SourceData.size(), outImage);
                }
                SetLastError("Image source has no encoded data");
                return false;

            default:
                SetLastError("Texture has no image source");
                return false;
        }
    }

    static bool EnsureUploaded(Texture* texture) {
        if (!texture || !texture->IsValid())
            return false;
        if (HasCurrentTextureData(*texture))
            return true;

        ClearGpuHandle(texture);

        DecodedImage image;
        if (!DecodeTextureSource(texture, &image))
            return false;
        return UploadRGBA(texture, image.pixels.data(), image.width, image.height);
    }

} // namespace

    bool Texture::IsValid() const {
        return Source != SourceKind::NONE && Width > 0 && Height > 0;
    }

    bool Texture::IsUploaded() const {
        return HasCurrentTextureData(*this) &&
               Data->Status == ImTextureStatus_OK &&
               Data->TexID != ImTextureID_Invalid;
    }

    ImTextureRef Texture::TexRef() const {
        return HasCurrentTextureData(*this) ? Data->GetTexRef() : ImTextureRef();
    }

    bool CreateFromRGBA(const unsigned char* rgbaPixels, int width, int height, Texture* outTexture) {
        if (!outTexture || !rgbaPixels)
            return false;
        if (!HasContext("CreateFromRGBA"))
            return false;

        size_t dataSize = 0;
        if (!ValidateSize(width, height, &dataSize))
            return false;

        Destroy(outTexture);
        outTexture->Width = width;
        outTexture->Height = height;
        outTexture->Source = SourceKind::RGBA_PIXELS;
        outTexture->SourceData.assign(rgbaPixels, rgbaPixels + dataSize);

        return UploadRGBA(outTexture, outTexture->SourceData.data(), width, height);
    }

    bool LoadFromFile(const char* path, Texture* outTexture) {
        if (!path || !outTexture)
            return false;
        if (!HasContext("LoadFromFile"))
            return false;

        std::vector<unsigned char> encodedData;
        if (!ReadFileBytes(path, &encodedData))
            return false;

        DecodedImage image;
        if (!DecodeMemoryToRGBA(encodedData.data(), encodedData.size(), &image))
            return false;

        Destroy(outTexture);
        outTexture->Width = image.width;
        outTexture->Height = image.height;
        outTexture->Source = SourceKind::FILE_BYTES;
        outTexture->SourcePath = path;
        outTexture->SourceData = std::move(encodedData);

        const bool created = UploadRGBA(outTexture, image.pixels.data(), image.width, image.height);
        if (created)
            LOGI("Loaded image from file: %s (%dx%d)", path, image.width, image.height);
        return created;
    }

    bool LoadFromMemory(const void* encodedData, size_t encodedSize, Texture* outTexture) {
        if (!encodedData || encodedSize == 0 || !outTexture)
            return false;
        if (!HasContext("LoadFromMemory"))
            return false;

        DecodedImage image;
        if (!DecodeMemoryToRGBA(encodedData, encodedSize, &image))
            return false;

        Destroy(outTexture);
        outTexture->Width = image.width;
        outTexture->Height = image.height;
        outTexture->Source = SourceKind::ENCODED_BYTES;
        outTexture->SourceData.assign(static_cast<const unsigned char*>(encodedData),
                                      static_cast<const unsigned char*>(encodedData) + encodedSize);

        const bool created = UploadRGBA(outTexture, image.pixels.data(), image.width, image.height);
        if (created)
            LOGI("Loaded image from memory: %dx%d", image.width, image.height);
        return created;
    }

    const char* GetLastError() {
        return g_LastError.c_str();
    }

    void Render(const Texture& texture, const ImVec2& size, const ImVec2& uv0, const ImVec2& uv1) {
        Texture& mutableTexture = const_cast<Texture&>(texture);
        if (!EnsureUploaded(&mutableTexture))
            return;
        ImGui::Image(mutableTexture.TexRef(), ResolveRenderSize(mutableTexture, size), uv0, uv1);
    }

    bool RenderButton(const char* id,
                      const Texture& texture,
                      const ImVec2& size,
                      const ImVec2& uv0,
                      const ImVec2& uv1,
                      const ImVec4& bg,
                      const ImVec4& tint) {
        Texture& mutableTexture = const_cast<Texture&>(texture);
        if (!EnsureUploaded(&mutableTexture))
            return false;
        return ImGui::ImageButton(id, mutableTexture.TexRef(),
                                  ResolveRenderSize(mutableTexture, size), uv0, uv1, bg, tint);
    }

    void Destroy(Texture* texture) {
        if (!texture)
            return;

        ReleaseGpuTexture(texture);
        ClearTexture(texture);
    }

    void UpdateLifecycle() {
        if (!ImGui::GetCurrentContext())
            return;

        {
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            for (TrackedTexture& tracked : g_TrackedTextures) {
                ImTextureData* data = tracked.data;
                if (data && data->Status == ImTextureStatus_OK && data->Pixels)
                    data->DestroyPixels();
            }
        }

        for (auto it = g_PendingDestroys.begin(); it != g_PendingDestroys.end();) {
            ImTextureData* data = it->data;
            if (!data) {
                it = g_PendingDestroys.erase(it);
                continue;
            }

            if (!it->destroyRequested) {
                it->frames++;
                if (it->frames < kDestroyDelayFrames) {
                    ++it;
                    continue;
                }

                data->WantDestroyNextFrame = true;
                data->UnusedFrames = 32;
                data->SetStatus(ImTextureStatus_WantDestroy);
                it->destroyRequested = true;
                ++it;
                continue;
            }

            if (data->Status == ImTextureStatus_Destroyed) {
                ImGui::UnregisterUserTexture(data);
                UntrackTexture(data);
                IM_DELETE(data);
                it = g_PendingDestroys.erase(it);
            } else {
                ++it;
            }
        }
    }

    void OnImGuiContextDestroyed() {
        std::vector<ImTextureData*> deleteList;
        ImGuiContext* context = ImGui::GetCurrentContext();

        {
            std::lock_guard<std::mutex> lock(g_TextureMutex);
            for (auto it = g_TrackedTextures.begin(); it != g_TrackedTextures.end();) {
                if (!context || it->context == context) {
                    deleteList.push_back(it->data);
                    it = g_TrackedTextures.erase(it);
                } else {
                    ++it;
                }
            }
        }

        if (!deleteList.empty()) {
            g_PendingDestroys.erase(
                    std::remove_if(g_PendingDestroys.begin(), g_PendingDestroys.end(),
                                   [&deleteList](const PendingDestroy& pending) {
                                       return std::find(deleteList.begin(), deleteList.end(), pending.data) != deleteList.end();
                                   }),
                    g_PendingDestroys.end());

            for (ImTextureData* data : deleteList) {
                if (!data)
                    continue;
                if (context && data->RefCount > 0)
                    ImGui::UnregisterUserTexture(data);
                IM_DELETE(data);
            }
            LOGI("Invalidated %zu image texture(s) for ImGui context reset", deleteList.size());
        }

        uint32_t nextGeneration = g_ContextGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (nextGeneration == 0)
            g_ContextGeneration.store(1, std::memory_order_release);
    }

} // namespace Images
} // namespace Renderer
