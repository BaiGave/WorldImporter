// unihex provider parsing. See signtext_unihex.h.

#include "signtext_unihex.h"

#include <algorithm>
#include <bit>

namespace signtext {

void HexLine::ComputeBounds(int& left, int& right) const {
    uint32_t mask = 0;
    for (uint32_t row : rows) mask |= row;
    if (mask == 0) {
        left = 0;
        right = static_cast<int>(static_cast<int8_t>(bitWidth));
        return;
    }
    const int lead = std::countl_zero(mask);
    const int trail = std::countr_zero(mask);
    left = static_cast<int>(static_cast<int8_t>(lead));
    right = static_cast<int>(static_cast<int8_t>(32 - trail - 1));
}

int DecodeHexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadHexLine(const std::string& digits, HexLine& line) {
    const size_t n = digits.size();
    int perRow = 0;
    int shift = 0;
    if (n == 32) {
        perRow = 2;
        shift = 24;
        line.bitWidth = 8;
    } else if (n == 64) {
        perRow = 4;
        shift = 16;
        line.bitWidth = 16;
    } else if (n == 96) {
        perRow = 6;
        shift = 8;
        line.bitWidth = 24;
    } else if (n == 128) {
        perRow = 8;
        shift = 0;
        line.bitWidth = 32;
    } else {
        return false;
    }
    size_t cursor = 0;
    for (int row = 0; row < 16; ++row) {
        uint32_t value = 0;
        for (int d = 0; d < perRow; ++d) {
            const int digit = DecodeHexDigit(digits[cursor++]);
            if (digit < 0) return false;
            value = (value << 4) | static_cast<uint32_t>(digit);
        }
        line.rows[row] = value << shift;
    }
    return true;
}

bool ParseHex(const std::string& data, std::unordered_map<uint32_t, HexLine>& out) {
    size_t i = 0;
    const size_t n = data.size();
    auto nextByte = [&]() -> int {
        if (i >= n) return -1;
        return static_cast<unsigned char>(data[i++]);
    };
    while (true) {
        std::string code;
        bool colon = false;
        while (true) {
            const int c = nextByte();
            if (c < 0) break;
            if (c == ':') {
                colon = true;
                break;
            }
            code.push_back(static_cast<char>(c));
        }
        if (code.empty() && !colon) return true;
        if (!colon || (code.size() != 4 && code.size() != 5 && code.size() != 6)) return false;
        uint32_t cp = 0;
        for (char ch : code) {
            const int digit = DecodeHexDigit(ch);
            if (digit < 0) return false;
            cp = (cp << 4) | static_cast<uint32_t>(digit);
        }
        std::string bits;
        while (true) {
            const int c = nextByte();
            if (c < 0 || c == '\n') break;
            bits.push_back(static_cast<char>(c));
        }
        HexLine line;
        if (!ReadHexLine(bits, line)) return false;
        out[cp] = line;
    }
}

}  // namespace signtext
