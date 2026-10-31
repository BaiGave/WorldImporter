// Text hash for the baked sign-text texture cache. See signtext_hash.h.

#include "signtext_hash.h"

#include <string>

namespace signtext {
namespace {

// A line needs the styled hash when it carries styles, or when its raw bytes
// contain "§" (U+00A0 SECTION SIGN, UTF-8 0xC2 0xA7) so the § parsing itself
// has to be represented in the cache key.
bool LineNeedsStyleHash(const StyledLine& line) {
    if (line.HasStyle()) return true;
    for (const auto& run : line.runs) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(run.text.data());
        const size_t n = run.text.size();
        for (size_t i = 0; i + 1 < n; ++i) {
            if (bytes[i] == 0xC2 && bytes[i + 1] == 0xA7) return true;
        }
    }
    return false;
}

}  // namespace

uint64_t HashText(const std::array<StyledLine, 4>& lines, int rgb, bool glowing,
                  int lineHeight, int maxWidth) {
    uint64_t hash = 14695981039346656037ull;
    auto mix = [&hash](unsigned char byte) {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    auto mixString = [&mix](const std::string& text) {
        for (unsigned char c : text) mix(c);
        mix(0);
    };
    // Unstyled lines mix only the plain text, same bytes as before styles existed.
    for (const auto& line : lines) mixString(line.Plain());
    mixString(std::to_string(rgb));
    mix(glowing ? 1 : 0);
    mixString(std::to_string(lineHeight));
    mixString(std::to_string(maxWidth));
    bool styled = false;
    for (const auto& line : lines) {
        if (LineNeedsStyleHash(line)) styled = true;
    }
    if (styled) {
        mix(1);
        for (const auto& line : lines) {
            for (const auto& run : line.runs) {
                unsigned char flags = 0;
                if (run.style.bold) flags |= 1;
                if (run.style.italic) flags |= 2;
                if (run.style.underlined) flags |= 4;
                if (run.style.strikethrough) flags |= 8;
                if (run.style.hasColor) flags |= 16;
                mix(flags);
                if (run.style.hasColor) {
                    mix(static_cast<unsigned char>((run.style.color >> 16) & 255));
                    mix(static_cast<unsigned char>((run.style.color >> 8) & 255));
                    mix(static_cast<unsigned char>(run.style.color & 255));
                }
                mixString(run.text);
            }
            mix(2);
        }
    }
    return hash;
}

}  // namespace signtext
