#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace Renderer {
namespace Images {

    enum class SourceKind : uint8_t {
        NONE,
        RGBA_PIXELS,
        ENCODED_BYTES,
        FILE_BYTES
    };

    struct Texture {
        // GPU-side texture owned by the active ImGui renderer context.
        // This may be recreated automatically after a backend/context reset.
        ImTextureData* Data = nullptr;
        ImGuiContext* OwnerContext = nullptr;
        uint32_t Generation = 0;

        // Persistent image source. This survives renderer resets.
        SourceKind Source = SourceKind::NONE;
        std::vector<unsigned char> SourceData;
        std::string SourcePath;
        int Width = 0;
        int Height = 0;

        bool IsValid() const;
        bool IsUploaded() const;
        ImTextureRef TexRef() const;
    };

    // Creates an ImGui texture from RGBA8888 pixels. Call from the render thread
    // after the ImGui context exists, usually inside the draw callback.
    bool CreateFromRGBA(const unsigned char* rgbaPixels, int width, int height, Texture* outTexture);

    // Decodes PNG/JPEG/WebP/etc. with Android ImageDecoder when available.
    // File paths must be readable from the target process.
    bool LoadFromFile(const char* path, Texture* outTexture);
    bool LoadFromMemory(const void* encodedData, size_t encodedSize, Texture* outTexture);
    const char* GetLastError();

    void Render(const Texture& texture,
                const ImVec2& size = ImVec2(0.0f, 0.0f),
                const ImVec2& uv0 = ImVec2(0.0f, 0.0f),
                const ImVec2& uv1 = ImVec2(1.0f, 1.0f));

    bool RenderButton(const char* id,
                      const Texture& texture,
                      const ImVec2& size = ImVec2(0.0f, 0.0f),
                      const ImVec2& uv0 = ImVec2(0.0f, 0.0f),
                      const ImVec2& uv1 = ImVec2(1.0f, 1.0f),
                      const ImVec4& bg = ImVec4(0.0f, 0.0f, 0.0f, 0.0f),
                      const ImVec4& tint = ImVec4(1.0f, 1.0f, 1.0f, 1.0f));

    // Schedules GPU texture destruction. Call from the render thread.
    void Destroy(Texture* texture);

    // Called by renderer backends once per frame after ImGui draw data was submitted.
    void UpdateLifecycle();

    // Called by renderer backends before an ImGui context is destroyed/recreated.
    void OnImGuiContextDestroyed();

} // namespace Images
} // namespace Renderer
