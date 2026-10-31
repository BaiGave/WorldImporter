// Unit tests for the pure sign-text helpers (signtext_style / signtext_unihex /
// signtext_hash). These cover the arithmetic and parsing that used to be locked
// inside SignText.cpp's anonymous namespace.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <unordered_map>

#include "signtext_hash.h"
#include "signtext_style.h"
#include "signtext_unihex.h"

namespace {

int gFailures = 0;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

// ---------------------------------------------------------------- style

void TestDyeTextColor() {
    using namespace signtext;
    Check(DyeTextColor("white") == 16777215, "DyeTextColor white");
    Check(DyeTextColor("light_blue") == 10141901, "DyeTextColor light_blue");
    Check(DyeTextColor("black") == 0, "DyeTextColor black");
    Check(DyeTextColor("Light_Gray") == 13882323, "DyeTextColor case-insensitive");
    Check(DyeTextColor("not_a_dye") == 0, "DyeTextColor unknown -> 0");
}

void TestDarkenRgb() {
    using namespace signtext;
    // channel * 0.4. 255 -> 102, so white (0xFFFFFF) -> 0x666666.
    Check(DarkenRgb(0xFFFFFF) == 0x666666, "DarkenRgb white");
    Check(DarkenRgb(0xFF0000) == 0x660000, "DarkenRgb red");
    Check(DarkColor(0x000000, true) == static_cast<int>(0xF0EBCC), "DarkColor black glowing");
    Check(DarkColor(0x000000, false) == 0, "DarkColor black not glowing");
}

void TestParseHexColor() {
    using namespace signtext;
    int rgb = -1;
    Check(ParseHexColor("#FF0000", rgb) && rgb == 0xFF0000, "ParseHexColor #FF0000");
    Check(ParseHexColor("#abcdef", rgb) && rgb == 0xABCDEF, "ParseHexColor lowercase");
    Check(!ParseHexColor("FF0000", rgb), "ParseHexColor rejects missing #");
    Check(!ParseHexColor("#", rgb), "ParseHexColor rejects bare #");
    Check(!ParseHexColor("#GGGGGG", rgb), "ParseHexColor rejects non-hex");
    Check(!ParseHexColor("#1234567", rgb), "ParseHexColor rejects > 0xFFFFFF");
}

void TestParseColorString() {
    using namespace signtext;
    int rgb = -1;
    Check(ParseColorString("#FF0000", rgb) && rgb == 0xFF0000, "ParseColorString hex");
    Check(ParseColorString("red", rgb) && rgb == 16733525, "ParseColorString named");
    Check(!ParseColorString("nope", rgb), "ParseColorString unknown");
}

void TestRawStyleApplyTo() {
    using namespace signtext;
    RawStyle parent;
    parent.bold = 1;
    parent.italic = 1;

    RawStyle child;  // all -1: inherits everything
    const RawStyle merged = child.ApplyTo(parent);
    Check(merged.bold == 1 && merged.italic == 1, "ApplyTo inherits unset fields");

    child.bold = 0;  // explicit off
    const RawStyle overridden = child.ApplyTo(parent);
    Check(overridden.bold == 0, "ApplyTo explicit 0 overrides parent");
    Check(overridden.italic == 1, "ApplyTo keeps unset field");

    child.hasColor = true;
    child.color = 0x112233;
    const RawStyle colored = child.ApplyTo(parent);
    Check(colored.hasColor && colored.color == 0x112233, "ApplyTo carries colour");
    Check(colored.Resolve().color == 0x112233, "Resolve masks colour to 24 bit");
}

void TestApplyLegacyCode() {
    using namespace signtext;
    RunStyle current;
    const RunStyle base;

    ApplyLegacyCode(current, base, 'l');
    Check(current.bold, "§l sets bold");
    ApplyLegacyCode(current, base, 'o');
    Check(current.italic, "§o sets italic");
    ApplyLegacyCode(current, base, 'r');
    Check(!current.bold && !current.italic, "§r restores base");

    ApplyLegacyCode(current, base, 'c');
    Check(current.hasColor && current.color == 16733525, "§c sets red");
    // A colour code also clears the other attributes.
    ApplyLegacyCode(current, base, 'l');
    Check(current.bold, "bold set before colour");
    ApplyLegacyCode(current, base, 'c');
    Check(!current.bold, "colour code resets bold");

    ApplyLegacyCode(current, base, 'L');
    Check(current.bold, "§L (uppercase) works");

    ApplyLegacyCode(current, base, 'z');
    Check(current.bold, "unknown code is ignored");
}

// --------------------------------------------------------------- unihex

void TestDecodeHexDigit() {
    using namespace signtext;
    Check(DecodeHexDigit('0') == 0, "DecodeHexDigit 0");
    Check(DecodeHexDigit('9') == 9, "DecodeHexDigit 9");
    Check(DecodeHexDigit('A') == 10, "DecodeHexDigit A");
    Check(DecodeHexDigit('F') == 15, "DecodeHexDigit F");
    // Vanilla's hex reader is uppercase-only; lowercase is rejected.
    Check(DecodeHexDigit('a') == -1, "DecodeHexDigit rejects lowercase");
    Check(DecodeHexDigit('g') == -1, "DecodeHexDigit invalid");
}

void TestReadHexLine() {
    using namespace signtext;
    // 32 digits = 8 px, 2 hex digits per row, rows shifted up by 24.
    const std::string s = "88442211448822118844221144882211";
    HexLine line;
    Check(ReadHexLine(s, line), "ReadHexLine 32 digits");
    Check(line.bitWidth == 8, "bitWidth 8");
    Check(line.rows[0] == 0x88000000u, "row0 = 0x88 << 24");
    Check(line.rows[1] == 0x44000000u, "row1 = 0x44 << 24");
    Check(line.rows[15] == 0x11000000u, "row15 = 0x11 << 24");

    // 64 digits = 16 px, 4 hex digits per row, shifted up by 16.
    // "8000" in row 0, the remaining 15 rows blank.
    std::string s16 = "8000" + std::string(60, '0');
    HexLine line16;
    Check(ReadHexLine(s16, line16), "ReadHexLine 64 digits");
    Check(line16.bitWidth == 16, "bitWidth 16");
    Check(line16.rows[0] == 0x80000000u, "row0 = 0x8000 << 16");
    Check(line16.rows[1] == 0u, "row1 blank");

    // 96 digits = 24 px, 128 digits = 32 px.
    const std::string s24(96, '0');
    const std::string s32(128, '0');
    HexLine line24, line32;
    Check(ReadHexLine(s24, line24) && line24.bitWidth == 24, "ReadHexLine 96 digits -> 24 px");
    Check(ReadHexLine(s32, line32) && line32.bitWidth == 32, "ReadHexLine 128 digits -> 32 px");

    // Malformed lengths and digits.
    HexLine bad;
    Check(!ReadHexLine("abc", bad), "ReadHexLine rejects bad length");
    Check(!ReadHexLine("0000000000000000000000000000000z", bad), "ReadHexLine rejects non-hex");
}

void TestHexLineComputeBounds() {
    using namespace signtext;
    HexLine line;
    line.bitWidth = 8;
    // 0xCC in the top row = pixels 0-5 inked, everything else blank.
    line.rows[0] = 0xCC000000u;

    int left = -1, right = -1;
    line.ComputeBounds(left, right);
    Check(left == 0 && right == 5, "ComputeBounds narrows to the inked span");

    HexLine empty;
    empty.bitWidth = 8;
    left = right = -1;
    empty.ComputeBounds(left, right);
    Check(left == 0 && right == 8, "ComputeBounds empty falls back to bitWidth");
}

void TestParseHex() {
    using namespace signtext;
    std::unordered_map<uint32_t, HexLine> out;
    const std::string data =
        "0041:8000800080008000800080008000800080008000800080008000800080008000\n"
        "0042:0000000000000000000000000000000000000000000000000000000000000000\n";
    Check(ParseHex(data, out), "ParseHex parses two entries");
    Check(out.count(0x41) == 1, "ParseHex codepoint 0041");
    Check(out.count(0x42) == 1, "ParseHex codepoint 0042");
    Check(!ParseHex("bad", out), "ParseHex rejects malformed codepoint");
    Check(ParseHex("", out), "ParseHex accepts empty input");
}

// ----------------------------------------------------------------- hash

signtext::StyledLine MakeLine(const std::string& text, bool styled) {
    signtext::StyledLine line;
    signtext::TextRun run;
    run.text = text;
    if (styled) {
        run.style.bold = true;
        run.style.hasColor = true;
        run.style.color = 0xFF0000;
    }
    line.runs.push_back(run);
    return line;
}

void TestHashText() {
    using namespace signtext;
    std::array<StyledLine, 4> lines{};
    lines[0] = MakeLine("hello", false);

    const uint64_t a = HashText(lines, 0, false, 10, 90);
    const uint64_t b = HashText(lines, 0, false, 10, 90);
    Check(a == b, "HashText deterministic");

    std::array<StyledLine, 4> glow = lines;
    (void)glow;
    Check(HashText(lines, 0, true, 10, 90) != a, "HashText distinguishes glowing");

    Check(HashText(lines, 0, false, 9, 90) != a, "HashText distinguishes line height");
    Check(HashText(lines, 0, false, 10, 60) != a, "HashText distinguishes max width");
    Check(HashText(lines, 123, false, 10, 90) != a, "HashText distinguishes colour");

    std::array<StyledLine, 4> styled = lines;
    styled[0] = MakeLine("hello", true);
    Check(HashText(styled, 0, false, 10, 90) != a, "HashText distinguishes styled text");

    std::array<StyledLine, 4> shorter = lines;
    Check(HashText(shorter, 0, false, 10, 90) == a, "HashText ignores trailing empty lines");

    // A literal "§" inside the text also changes the hash.
    std::array<StyledLine, 4> section = lines;
    section[0] = MakeLine("he\xC2\xA7" "allo", false);
    Check(HashText(section, 0, false, 10, 90) != a, "HashText sees the section sign");
}

}  // namespace

int main() {
    TestDyeTextColor();
    TestDarkenRgb();
    TestParseHexColor();
    TestParseColorString();
    TestRawStyleApplyTo();
    TestApplyLegacyCode();
    TestDecodeHexDigit();
    TestReadHexLine();
    TestHexLineComputeBounds();
    TestParseHex();
    TestHashText();
    if (gFailures == 0) {
        std::cout << "sign_text_tests: all checks passed\n";
        return 0;
    }
    std::cerr << "sign_text_tests: " << gFailures << " check(s) failed\n";
    return 1;
}
