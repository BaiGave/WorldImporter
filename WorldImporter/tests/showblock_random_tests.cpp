// 展台随机序列对照 scripts/javaref/showref.txt（ShowRef.java + 游戏类）。
#include "showblockrandom.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

static int gFailures = 0;

static void Check(bool ok, const std::string& label) {
    if (ok) return;
    std::cerr << "FAIL " << label << '\n';
    ++gFailures;
}

static std::string SlotsText(const std::vector<int>& slots) {
    std::string text = "{";
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i) text += ',';
        text += std::to_string(slots[i]);
    }
    text += '}';
    return text;
}

// x y z | 7*e1 | 7*multipart{1,1,1,1} | 7*multipart{3,1} | 7*e3
static const int kShowRef[][31] = {
    {129,274,-1627,0,10,4,3,3,9,3,1,1,2,1,0,0,3,0,0,0,0,0,0,1,2,3,0,1,0,2,2},
    {750,116,1645,4,9,0,5,9,9,7,3,1,1,0,1,3,0,1,0,0,0,0,1,0,2,2,2,2,3,2,2},
    {22,117,12,1,10,6,3,3,9,7,2,2,1,3,2,2,2,0,0,0,1,0,0,0,1,0,2,2,0,2,2},
    {-240,184,-982,1,10,7,8,9,8,3,3,1,0,1,3,2,1,1,0,0,0,1,0,0,2,0,2,0,2,2,3},
    {-1167,-12,826,4,0,10,4,8,2,7,2,3,0,2,2,1,1,0,1,0,0,0,0,0,0,3,2,0,2,2,0},
    {771,138,-659,1,4,2,7,6,8,4,2,0,2,2,0,1,1,0,0,0,0,0,0,0,2,2,1,2,0,2,2},
    {308,93,-1322,10,8,6,2,4,2,7,0,1,0,0,2,3,3,0,0,0,0,0,1,1,0,2,0,2,3,0,2},
    {-1020,211,-35,7,10,8,0,4,10,1,3,0,3,0,1,2,2,1,0,1,0,0,0,0,2,2,2,0,3,2,2},
    {1542,239,-319,8,8,10,4,6,9,8,3,0,0,3,2,1,3,1,0,0,1,0,0,1,2,1,1,2,2,2,2},
    {-328,226,1631,10,0,4,4,10,2,0,1,0,1,1,3,0,1,0,0,0,0,1,0,0,0,2,0,2,0,2,3},
    {1650,316,-708,10,9,3,3,9,1,7,2,3,0,2,0,3,2,0,1,0,0,0,1,0,2,2,2,1,2,2,2},
    {1877,-6,1513,5,8,1,4,5,6,7,2,2,0,0,1,1,2,0,0,0,0,0,0,0,0,2,2,0,2,2,3},
    {-559,193,-307,1,3,2,7,5,2,10,3,3,0,2,2,2,3,1,1,0,0,0,0,1,2,2,3,2,2,0,1},
    {-416,267,1801,5,9,4,3,5,8,1,0,0,3,3,3,1,2,0,0,1,1,1,0,0,2,1,3,2,2,3,2},
    {-305,262,-1101,6,7,8,2,2,2,8,1,3,2,1,0,2,1,0,1,0,0,0,0,0,1,3,1,3,1,0,1},
    {-1629,42,-1628,6,6,5,4,7,6,1,2,1,3,3,1,0,3,0,0,1,1,0,0,1,2,2,2,2,1,0,3},
    {1076,112,781,6,3,4,2,3,8,1,1,3,1,3,3,1,3,0,1,0,1,1,0,1,2,1,1,2,2,2,2},
    {1935,283,-1948,1,4,2,1,8,1,6,3,3,3,1,3,0,1,1,1,1,0,1,0,0,2,2,0,2,1,2,0},
    {-1727,207,1387,6,6,0,9,5,5,0,2,3,3,3,2,1,0,0,1,1,1,0,0,0,2,2,0,2,2,0,0},
    {1062,183,1566,2,6,5,3,6,10,5,0,0,2,1,0,1,3,0,0,0,0,0,0,1,0,2,2,2,0,3,3},
    {222,160,-243,9,1,1,2,7,7,7,3,2,1,2,0,3,2,1,0,0,0,0,1,0,1,3,2,1,1,2,0},
    {-690,165,243,1,6,9,6,4,6,8,2,3,2,3,0,2,1,0,1,0,1,0,0,0,3,0,3,1,3,2,3},
    {-1760,311,-1376,5,9,9,4,7,7,0,0,3,2,3,2,1,0,0,1,0,1,0,0,0,2,0,2,2,1,0,3},
    {1003,28,-659,0,1,0,9,4,4,8,0,3,1,2,2,1,1,0,1,0,0,0,0,0,2,2,2,1,2,1,0}
};

static void CheckNextLong() {
    LegacyRandom zero(0);
    Check(ShowRandNextLong(zero) == -4962768465676381896LL, "Random(0).nextLong");
    LegacyRandom fortyTwo(42);
    Check(ShowRandNextLong(fortyTwo) == -5025562857975149833LL, "Random(42).nextLong");
}

static void CheckJavaAbs() {
    const auto kMin = std::numeric_limits<std::int32_t>::min();
    Check(ShowRandJavaAbs(kMin) == kMin, "JavaAbs(INT_MIN)==INT_MIN");
    Check(ShowRandJavaAbs(-1) == 1, "JavaAbs(-1)");
    Check(ShowRandJavaAbs(0) == 0, "JavaAbs(0)");
    Check(ShowRandJavaAbs(5) == 5, "JavaAbs(5)");

    // 第二步 next(32) 的高位为 INT_MIN。abs 后对 11 取模为负，必须落到槽 0
    // （若把 INT_MIN 当成无符号 2^31，会得到槽 2）。
    LegacyRandom probe(0);
    probe.seed = 261042947131990ULL;
    const std::int32_t low = static_cast<std::int32_t>(ShowRandNextLong(probe));
    Check(low == kMin, "crafted nextLong low bits == INT_MIN");
    Check(ShowRandJavaAbs(low) % 11 < 0, "INT_MIN % 11 为负");

    LegacyRandom slotRng(0);
    slotRng.seed = 261042947131990ULL;
    const int slot = ShowRandWeightedSlot(std::vector<int>(11, 1), slotRng);
    Check(slot == 0, "negative idx picks slot 0");
}

static void CheckPlainDoesNotConsume() {
    LegacyRandom left(99);
    LegacyRandom right(99);
    ShowRandEntry simple;
    simple.kind = ShowRandEntry::Simple;
    const auto simplePicks = ShowRandPickEntry(simple, left);
    for (int side = 0; side < 7; ++side) {
        Check(simplePicks[side].size() == 1 && simplePicks[side][0] == 0, "simple slot 0");
    }

    ShowRandEntry one;
    one.kind = ShowRandEntry::Variant;
    one.weights = {4};
    const auto onePicks = ShowRandPickEntry(one, left);
    Check(onePicks[0].size() == 1 && onePicks[3][0] == 0, "single variant slot 0");

    ShowRandEntry zeros;
    zeros.kind = ShowRandEntry::Variant;
    zeros.weights = {0, 0};
    const auto zeroPicks = ShowRandPickEntry(zeros, left);
    Check(zeroPicks[6].size() == 1 && zeroPicks[6][0] == 0, "zero total slot 0");

    Check(ShowRandNextLong(left) == ShowRandNextLong(right), "plain entries consume nothing");

    ShowRandEntry bare;
    bare.kind = ShowRandEntry::Multipart;
    bare.groups = {std::vector<int>{}, {5}};
    const auto barePicks = ShowRandPickEntry(bare, left);
    for (int side = 0; side < 7; ++side) {
        Check(barePicks[side].size() == 2 && barePicks[side][0] == 0 && barePicks[side][1] == 0,
              "unweighted multipart slot 0");
        ShowRandNextLong(right);
    }
    Check(ShowRandNextLong(left) == ShowRandNextLong(right), "multipart still consumes one nextLong per side");
}

static void CheckShowRef() {
    ShowRandEntry simple;
    simple.kind = ShowRandEntry::Simple;

    ShowRandEntry var11;
    var11.kind = ShowRandEntry::Variant;
    var11.weights.assign(11, 1);

    ShowRandEntry multi;
    multi.kind = ShowRandEntry::Multipart;
    multi.groups = {std::vector<int>{}, {1, 1, 1, 1}, {3, 1}};

    ShowRandEntry var4;
    var4.kind = ShowRandEntry::Variant;
    var4.weights = {2, 1, 5, 1};

    for (const auto& row : kShowRef) {
        const int x = row[0], y = row[1], z = row[2];
        const std::string where = std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(z);
        LegacyRandom rng(BlockPosSeed(x, y, z));
        LegacyRandom expect(BlockPosSeed(x, y, z));

        const auto plain = ShowRandPickEntry(simple, rng);
        const auto e1 = ShowRandPickEntry(var11, rng);
        const auto parts = ShowRandPickEntry(multi, rng);
        const auto e3 = ShowRandPickEntry(var4, rng);

        for (int side = 0; side < 7; ++side) {
            Check(plain[side].size() == 1 && plain[side][0] == 0, where + " simple");
            Check(e1[side].size() == 1 && e1[side][0] == row[3 + side],
                  where + " e1 side " + std::to_string(side) + " got " + SlotsText(e1[side]));
            Check(parts[side].size() == 3 && parts[side][0] == 0, where + " multipart g0");
            Check(parts[side].size() == 3 && parts[side][1] == row[10 + side],
                  where + " g1 side " + std::to_string(side) + " got " + SlotsText(parts[side]));
            Check(parts[side].size() == 3 && parts[side][2] == row[17 + side],
                  where + " g2 side " + std::to_string(side) + " got " + SlotsText(parts[side]));
            Check(e3[side].size() == 1 && e3[side][0] == row[24 + side],
                  where + " e3 side " + std::to_string(side) + " got " + SlotsText(e3[side]));
        }
        // simple 不消耗；两个加权变体各 7 次；multipart 每面一次，共 21 次。
        for (int i = 0; i < 21; ++i) ShowRandNextLong(expect);
        Check(ShowRandNextLong(rng) == ShowRandNextLong(expect), where + " rng consumption");
    }
}

int main() {
    CheckNextLong();
    CheckJavaAbs();
    CheckPlainDoesNotConsume();
    CheckShowRef();
    if (gFailures) {
        std::cerr << gFailures << " failure(s)\n";
        return 1;
    }
    std::cout << "showblock_random ok\n";
    return 0;
}
