#pragma once

// unihex provider parsing, extracted from SignText.cpp.
//
// A .hex line is "<codepoint>:<hexdigits>", one 16-row glyph per line where each
// hex digit is a nibble of a 16-bit row. UnihexProvider.LineData.line() shifts
// the rows so that bitLeft/bitRight describe the inked span; that derivation is
// ComputeBounds below.

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace signtext {

struct HexLine {
    int bitWidth = 0;
    std::array<uint32_t, 16> rows{};

    // UnihexProvider.LineData.line(): left/right from the inked bits.
    // Falls back to (0, bitWidth) when nothing is inked.
    void ComputeBounds(int& left, int& right) const;
};

int DecodeHexDigit(char c);

// Parse one "<cp>:<digits>" line. Returns false when the line is malformed.
bool ReadHexLine(const std::string& digits, HexLine& line);

// Parse a whole .hex file. Returns false on the first malformed line.
bool ParseHex(const std::string& data, std::unordered_map<uint32_t, HexLine>& out);

}  // namespace signtext
