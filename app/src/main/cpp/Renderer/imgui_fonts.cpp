#include "imgui_fonts.hpp"

#include "imgui.h"

#include "IconsFontAwesome7.h"
#include "casncadia_mono.h"
#include "fa_solid_900.h"

namespace Renderer {

    void SetupImGuiFonts(float fontSizePixels) {
        ImGuiIO& io = ImGui::GetIO();
        ImFontAtlas* fonts = io.Fonts;
        fonts->Clear();

        ImFontConfig textConfig;
        textConfig.FontDataOwnedByAtlas = false;
        textConfig.SizePixels = fontSizePixels;
        ImFont* textFont = fonts->AddFontFromMemoryCompressedBase85TTF(
                CascadiaMono_compressed_data_base85,
                fontSizePixels,
                &textConfig,
                fonts->GetGlyphRangesDefault());

        if (textFont) {
            io.FontDefault = textFont;
        } else {
            io.FontDefault = fonts->AddFontDefault();
        }

        static const ImWchar iconRanges[] = {
                ICON_MIN_FA,
                ICON_MAX_FA,
                0
        };

        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.FontDataOwnedByAtlas = false;
        iconConfig.GlyphMinAdvanceX = fontSizePixels;
        fonts->AddFontFromMemoryTTF(
                const_cast<unsigned char*>(fa_solid_900),
                static_cast<int>(sizeof(fa_solid_900)),
                fontSizePixels,
                &iconConfig,
                iconRanges);
    }

} // namespace Renderer
