#pragma once

// Text hash for the baked sign-text texture cache, extracted from SignText.cpp.
//
// FNV-1a over the line contents, colour, glow flag, line height and max width.
// Unstyled lines mix only the plain text (same bytes as before styles existed);
// styled lines additionally mix per-run flags and colours.

#include <array>
#include <cstdint>

#include "signtext_style.h"

namespace signtext {

uint64_t HashText(const std::array<StyledLine, 4>& lines, int rgb, bool glowing,
                  int lineHeight, int maxWidth);

}  // namespace signtext
