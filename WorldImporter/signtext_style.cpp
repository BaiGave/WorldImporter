// Pure style/colour helpers for sign text. See signtext_style.h.

#include "signtext_style.h"

#include <cctype>
#include <utility>

namespace signtext {
namespace {

// SignRenderer.getDarkColor returns this for black glowing text.
constexpr int kBlackGlowOutline = 0xF0EBCC;

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

RawStyle RawStyle::ApplyTo(const RawStyle& parent) const {
    RawStyle out = parent;
    if (hasColor) {
        out.hasColor = true;
        out.color = color;
    }
    if (bold >= 0) out.bold = bold;
    if (italic >= 0) out.italic = italic;
    if (underlined >= 0) out.underlined = underlined;
    if (strikethrough >= 0) out.strikethrough = strikethrough;
    if (obfuscated >= 0) out.obfuscated = obfuscated;
    return out;
}

RunStyle RawStyle::Resolve() const {
    RunStyle style;
    style.hasColor = hasColor;
    style.color = color & 0xFFFFFF;
    style.bold = bold == 1;
    style.italic = italic == 1;
    style.underlined = underlined == 1;
    style.strikethrough = strikethrough == 1;
    style.obfuscated = obfuscated == 1;
    return style;
}

bool RunStyle::affectsPixels() const {
    return hasColor || bold || italic || underlined || strikethrough;
}

std::string StyledLine::Plain() const {
    std::string out;
    for (const auto& run : runs) out += run.text;
    return out;
}

bool StyledLine::HasStyle() const {
    for (const auto& run : runs) {
        if (run.style.affectsPixels()) return true;
    }
    return false;
}

int DyeTextColor(const std::string& name) {
    const std::string key = ToLower(name);
    static const std::pair<const char*, int> kColors[] = {
        {"white", 16777215},
        {"orange", 16738335},
        {"magenta", 16711935},
        {"light_blue", 10141901},
        {"yellow", 16776960},
        {"lime", 12582656},
        {"pink", 16738740},
        {"gray", 8421504},
        {"light_gray", 13882323},
        {"cyan", 65535},
        {"purple", 10494192},
        {"blue", 255},
        {"brown", 9127187},
        {"green", 65280},
        {"red", 16711680},
        {"black", 0},
    };
    for (const auto& entry : kColors) {
        if (key == entry.first) return entry.second;
    }
    return 0;
}

int DarkChannel(int channel) {
    return static_cast<int>(static_cast<double>(channel) * 0.4);
}

int DarkenRgb(int rgb) {
    const int r = DarkChannel((rgb >> 16) & 255);
    const int g = DarkChannel((rgb >> 8) & 255);
    const int b = DarkChannel(rgb & 255);
    return (r << 16) | (g << 8) | b;
}

int DarkColor(int textRgb, bool glowing) {
    if (textRgb == 0 && glowing) return kBlackGlowOutline;
    return DarkenRgb(textRgb);
}

bool NamedChatColor(const std::string& name, int& rgb) {
    static const std::pair<const char*, int> kColors[] = {
        {"black", 0},
        {"dark_blue", 170},
        {"dark_green", 43520},
        {"dark_aqua", 43690},
        {"dark_red", 11141120},
        {"dark_purple", 11141290},
        {"gold", 16755200},
        {"gray", 11184810},
        {"dark_gray", 5592405},
        {"blue", 5592575},
        {"green", 5635925},
        {"aqua", 5636095},
        {"red", 16733525},
        {"light_purple", 16733695},
        {"yellow", 16777045},
        {"white", 16777215},
    };
    for (const auto& entry : kColors) {
        if (name == entry.first) {
            rgb = entry.second;
            return true;
        }
    }
    return false;
}

bool ParseHexColor(const std::string& text, int& rgb) {
    if (text.size() < 2 || text[0] != '#') return false;
    int value = 0;
    for (size_t i = 1; i < text.size(); ++i) {
        const char c = text[i];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | digit;
        if (value > 0xFFFFFF) return false;
    }
    rgb = value;
    return true;
}

bool ParseColorString(const std::string& text, int& rgb) {
    if (ParseHexColor(text, rgb)) return true;
    return NamedChatColor(text, rgb);
}

void ApplyLegacyCode(RunStyle& current, const RunStyle& base, uint32_t code) {
    if (code >= 'A' && code <= 'Z') code = code - 'A' + 'a';
    if (code > 127) return;
    const char c = static_cast<char>(code);
    switch (c) {
        case 'k': current.obfuscated = true; return;
        case 'l': current.bold = true; return;
        case 'm': current.strikethrough = true; return;
        case 'n': current.underlined = true; return;
        case 'o': current.italic = true; return;
        case 'r': current = base; return;
        default: break;
    }
    int rgb = 0;
    bool color = false;
    switch (c) {
        case '0': rgb = 0; color = true; break;
        case '1': rgb = 170; color = true; break;
        case '2': rgb = 43520; color = true; break;
        case '3': rgb = 43690; color = true; break;
        case '4': rgb = 11141120; color = true; break;
        case '5': rgb = 11141290; color = true; break;
        case '6': rgb = 16755200; color = true; break;
        case '7': rgb = 11184810; color = true; break;
        case '8': rgb = 5592405; color = true; break;
        case '9': rgb = 5592575; color = true; break;
        case 'a': rgb = 5635925; color = true; break;
        case 'b': rgb = 5636095; color = true; break;
        case 'c': rgb = 16733525; color = true; break;
        case 'd': rgb = 16733695; color = true; break;
        case 'e': rgb = 16777045; color = true; break;
        case 'f': rgb = 16777215; color = true; break;
        default: break;
    }
    if (!color) return;
    current.hasColor = true;
    current.color = rgb;
    current.bold = false;
    current.italic = false;
    current.underlined = false;
    current.strikethrough = false;
    current.obfuscated = false;
}

}  // namespace signtext
