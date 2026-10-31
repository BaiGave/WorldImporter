#include "CtmRandom.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

// RandRef.java 对 Continuity 3.0.0-beta.5 打出的 40 组参考值。
// 每行: {x, y, z, face, loops, size, mix, unweightedIndex, weightedIndex}
// weighted 使用权重 {3, 1, 2}。
static constexpr int kRefs[][9] = {
    {251, 216, -1759, 0, 1, 48, -111676303, 31, 39},
    {-625, 242, 1501, 3, 2, 3, -540561976, 1, 2},
    {1390, -42, 1012, 4, 2, 17, 827500247, 2, 9},
    {532, 167, -199, 1, 1, 3, 350730385, 1, 0},
    {428, 317, 1425, 5, 2, 47, 1827274398, 32, 16},
    {1729, -59, 501, 1, 2, 36, -1620207870, 6, 3},
    {1952, -25, -990, 4, 0, 38, -744949485, 31, 34},
    {99, 70, -682, 4, 0, 34, -508269135, 7, 3},
    {1350, 104, -226, 4, 0, 31, 1923173661, 1, 0},
    {380, 263, 1158, 3, 1, 22, -1400150935, 7, 3},
    {-706, 167, 553, 4, 2, 22, -1537843579, 19, 9},
    {-1630, 45, 1990, 1, 2, 25, -114892691, 16, 20},
    {918, 144, -1812, 0, 0, 34, -297365270, 12, 23},
    {-448, 171, -997, 4, 0, 9, -271370477, 2, 5},
    {1275, 240, 409, 5, 2, 37, 541968553, 27, 13},
    {687, 231, 953, 3, 1, 35, -737834825, 0, 17},
    {-285, 226, -826, 1, 1, 30, 1181951589, 9, 4},
    {-1895, 133, -904, 1, 2, 13, 1939087445, 9, 4},
    {-830, 238, 44, 3, 2, 28, -118357821, 1, 14},
    {1716, 196, -1414, 3, 2, 3, -1230257910, 0, 0},
    {1104, 57, -188, 5, 1, 39, 682911757, 34, 36},
    {-225, 141, -1825, 3, 2, 47, -731820357, 42, 44},
    {-1541, 222, -1177, 5, 2, 36, 1728221291, 35, 17},
    {-1501, 19, -562, 2, 1, 40, 1683233145, 25, 12},
    {-618, 100, -473, 2, 2, 23, 305835772, 11, 17},
    {-144, 284, -793, 1, 0, 47, -638871335, 6, 26},
    {-1402, -47, -1546, 0, 0, 27, 722944704, 21, 24},
    {1626, -26, -1903, 0, 1, 31, 844178066, 16, 8},
    {557, 99, 858, 4, 0, 35, 1101428740, 20, 10},
    {-1033, 45, 37, 4, 1, 29, -2098493089, 19, 9},
    {1148, 103, 1587, 3, 2, 49, -89341347, 39, 19},
    {-577, -46, 1470, 4, 1, 22, -1500927451, 1, 11},
    {-254, 299, -1550, 4, 0, 26, -1387901838, 24, 25},
    {-1112, 286, -121, 3, 1, 3, 1734793264, 1, 2},
    {-1420, 21, -962, 3, 0, 50, -1246479921, 21, 10},
    {-1948, 301, -1017, 0, 0, 39, -470518691, 32, 35},
    {-768, 222, 141, 5, 0, 14, 677604842, 12, 13},
    {-1984, -18, 338, 3, 1, 45, 727857112, 22, 11},
    {763, 225, 292, 4, 0, 36, -818112391, 7, 3},
    {-349, -49, 983, 1, 2, 32, 1151974037, 21, 10},
};

static int g_failures = 0;

static void expect(bool ok, const std::string& message) {
    if (ok) return;
    std::cerr << message << '\n';
    ++g_failures;
}

static void checkReferenceVectors() {
    const std::vector<int> weights{3, 1, 2};
    constexpr int count = static_cast<int>(sizeof(kRefs) / sizeof(kRefs[0]));
    static_assert(count == 40, "expected 40 Continuity reference vectors");
    for (int i = 0; i < count; ++i) {
        const int x = kRefs[i][0];
        const int y = kRefs[i][1];
        const int z = kRefs[i][2];
        const int face = kRefs[i][3];
        const int loops = kRefs[i][4];
        const int size = kRefs[i][5];
        const int32_t expectedMix = kRefs[i][6];
        const int expectedUnweighted = kRefs[i][7];
        const int expectedWeighted = kRefs[i][8];

        const int32_t mix = ContinuityRandomMix(x, y, z, face, loops);
        const int unweighted = ContinuityRandomIndex(mix, size, {});
        const int weighted = ContinuityRandomIndex(mix, size, weights);
        if (mix == expectedMix && unweighted == expectedUnweighted && weighted == expectedWeighted)
            continue;
        std::cerr << "ref[" << i << "] (" << x << "," << y << "," << z
                  << " face=" << face << " loops=" << loops << " size=" << size << ")"
                  << " mix " << mix << " (expected " << expectedMix << ")"
                  << " unweighted " << unweighted << " (expected " << expectedUnweighted << ")"
                  << " weighted " << weighted << " (expected " << expectedWeighted << ")\n";
        ++g_failures;
    }
}

// Math.abs(INT_MIN) 仍是负数；sum<=0 退回无权重；权重为 0 时不钳制成 1。
static void checkGuards() {
    constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
    expect(ContinuityRandomIndex(kMin, 8, {}) == 0, "INT_MIN % 8 should be 0");
    expect(ContinuityRandomIndex(kMin, 3, {}) == 1, "INT_MIN % 3 should wrap into range");
    expect(ContinuityRandomIndex(kMin, 3, std::vector<int>{3, 1, 2}) == 2,
           "weighted INT_MIN should use the adjusted remainder");
    expect(ContinuityRandomIndex(5, 4, std::vector<int>{0, 0}) == 1,
           "non-positive weight sum should fall back to unweighted");
    expect(ContinuityRandomIndex(5, 4, std::vector<int>{-2}) == 1,
           "negative weight sum should fall back to unweighted");
    expect(ContinuityRandomIndex(4, 4, std::vector<int>{3, 0}) == 3,
           "zero weights must be copied as-is");
    expect(ContinuityRandomIndex(10, 0, {}) == 0, "size 0 should not divide");
    expect(ContinuityRandomIndex(10, -1, std::vector<int>{3, 1, 2}) == 0,
           "negative size should not divide");
}

int main() {
    checkReferenceVectors();
    checkGuards();
    if (g_failures != 0) {
        std::cerr << g_failures << " ctm random check(s) failed\n";
        return 1;
    }
    std::cout << "ctm random tests passed\n";
    return 0;
}
