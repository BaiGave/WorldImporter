#pragma once

// Pure style/colour helpers for sign text, extracted from SignText.cpp so they
// can be unit-tested without a world, a jar, or the font cache.
//
// Everything here mirrors the 1.21.1 client: Style.applyTo (tri-state
// inheritance), TextColor.parseColor, ChatFormatting colours, DyeColor text
// colours, SignRenderer.getDarkColor, and StringDecomposer.iterateFormatted
// with Style.applyLegacyFormat for "§" codes.

#include <cstdint>
#include <string>
#include <vector>

namespace signtext {

// Tri-state flags: -1 inherits, 0/1 are explicit. Matches Style.applyTo.
struct RawStyle {
    bool hasColor = false;
    int color = 0;
    int bold = -1;
    int italic = -1;
    int underlined = -1;
    int strikethrough = -1;
    int obfuscated = -1;

    RawStyle ApplyTo(const RawStyle& parent) const;
    struct RunStyle Resolve() const;
};

// Resolved style. Unset fields are not stored: inheritance happens in RawStyle
// before a run is built. hasColor false means "use the sign's default colour".
struct RunStyle {
    bool hasColor = false;
    int color = 0;
    bool bold = false;
    bool italic = false;
    bool underlined = false;
    bool strikethrough = false;
    bool obfuscated = false;

    bool affectsPixels() const;
};

struct TextRun {
    std::string text;
    RunStyle style;
};

struct StyledLine {
    std::vector<TextRun> runs;

    std::string Plain() const;
    bool HasStyle() const;
};

// DyeColor text colour, by dye name ("white", "light_blue", ...). 0 when unknown.
int DyeTextColor(const std::string& name);

// SignRenderer.getDarkColor / darkened dye for the glow outline.
int DarkChannel(int channel);
int DarkenRgb(int rgb);
int DarkColor(int textRgb, bool glowing);

// ChatFormatting colours (not dye colours). Names match TextColor.parseColor.
bool NamedChatColor(const std::string& name, int& rgb);

// TextColor.parseColor: '#' + hex, value in 0..0xFFFFFF.
bool ParseHexColor(const std::string& text, int& rgb);

// Named colour first, then '#' hex.
bool ParseColorString(const std::string& text, int& rgb);

// StringDecomposer.iterateFormatted + Style.applyLegacyFormat. §r restores `base`.
void ApplyLegacyCode(RunStyle& current, const RunStyle& base, uint32_t code);

}  // namespace signtext
