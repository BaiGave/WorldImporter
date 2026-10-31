// blockposseed.h
// 与原版一致的"加权变体"渲染种子与随机数发生器。
//
// 两条算法，由区块 NBT 的 DataVersion 选择（1.21.5 = 4325）：
//   >= 4325 或缺失：WeightedList.get 用 nextInt(totalWeight)（1.21.5+ / 26.x）。
//   < 4325：WeightedBakedModel 用 abs((int)nextLong()) % totalWeight，
//           再按权重相减（1.18.2 .. 1.21.4）。multipart 先抽一次 nextLong，
//           每个加权部件各自 new Random(l)。
//
// RandomSource.create(long) / java.util.Random(long) 都是 48 位 LCG
// （0x5DEECE66D / 48 位掩码 / 加数 0xB）。
//
// 抽成头文件是为了让测试能直接检查这份实现本身，而不是一份抄件。
#ifndef BLOCKPOSSEED_H
#define BLOCKPOSSEED_H

#include <cstdint>
#include <limits>
#include <optional>

// 原版 Mth.getSeed(x,y,z)。
// 注意 x*3129871 在 Java 里是 32 位乘法（会回绕），之后整段按 64 位运算；
// C++ 有符号溢出是 UB，所以用无符号显式回绕再转回有符号。
inline long long BlockPosSeed(int x, int y, int z) {
    const std::uint32_t xm =
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(x) * 3129871u);
    const std::int64_t l = static_cast<std::int64_t>(static_cast<std::int32_t>(xm))
        ^ (static_cast<std::int64_t>(z) * 116129781LL)
        ^ static_cast<std::int64_t>(y);
    std::uint64_t u = static_cast<std::uint64_t>(l);
    u = u * u * 42317861ULL + u * 11ULL;   // 同样按 64 位回绕
    // 等价于 Java 的 (long) >> 16（算术右移）
    std::uint64_t r = u >> 16;
    if (u & 0x8000000000000000ULL) r |= 0xFFFF000000000000ULL;
    return static_cast<long long>(r);
}

// 原版 LegacyRandomSource == java.util.Random 的 LCG
struct LegacyRandom {
    std::uint64_t seed;

    explicit LegacyRandom(long long s) : seed(0) { setSeed(s); }

    void setSeed(long long s) {
        seed = (static_cast<std::uint64_t>(s) ^ 0x5DEECE66DULL) & 0xFFFFFFFFFFFFULL;
    }

    std::int32_t next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & 0xFFFFFFFFFFFFULL;
        return static_cast<std::int32_t>(seed >> (48 - bits));
    }

    // java.util.Random.nextInt(bound)，0-based
    int nextInt(int bound) {
        if (bound <= 0) return 0;
        if ((bound & -bound) == bound) {   // bound 是 2 的幂
            return static_cast<int>((static_cast<long long>(bound) * next(31)) >> 31);
        }
        int j, i;
        do { j = next(31); i = j % bound; } while (j - i + (bound - 1) < 0);
        return i;
    }

    // java.util.Random.nextLong()：((long)next(32) << 32) + next(32)。
    // next(32) 是有符号 int，相加前符号扩展，不能当成两段无符号位拼起来。
    long long nextLong() {
        const std::int32_t hi = next(32);
        const std::int32_t lo = next(32);
        const std::uint64_t bits =
            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(hi)) << 32)
            + static_cast<std::uint64_t>(static_cast<std::int64_t>(lo));
        return static_cast<long long>(bits);
    }
};

// 1.21.5 的 DataVersion。更小的值走旧挑选；缺失则保持 1.21.5+。
inline constexpr int kModernVariantDataVersion = 4325;

// 当前正在生成的区块的 DataVersion。模型线程在进入区块前写入。
extern thread_local std::optional<int> g_activeChunkDataVersion;

inline bool UseLegacyVariantPick() {
    return g_activeChunkDataVersion.has_value()
        && *g_activeChunkDataVersion < kModernVariantDataVersion;
}

// Math.abs(int)。Integer.MIN_VALUE 保持为负数。
inline std::int32_t JavaAbs32(std::int32_t v) {
    if (v >= 0) return v;
    if (v == std::numeric_limits<std::int32_t>::min()) return v;
    return static_cast<std::int32_t>(-v);
}

// WeightedBakedModel：abs((int)nextLong()) % totalWeight。余数可能为负。
inline int LegacyWeightedIndex(LegacyRandom& rng, int totalWeight) {
    return JavaAbs32(static_cast<std::int32_t>(rng.nextLong())) % totalWeight;
}

// WeightedRandom.getWeightedItem。减法按 Java int 回绕。
// 返回槽位；减完仍 >= 0 则 -1（没有面）。
// abs(MIN_VALUE) 未经取余时会回绕成大正数，从而返回 -1；
// 游戏先做 % totalWeight，余数落在 (-totalWeight, 0]，通常选中第一项。
template <typename WeightAt>
inline int SelectWeightedSlot(std::int32_t index, int count, WeightAt weightAt) {
    for (int i = 0; i < count; ++i) {
        index = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(index)
            - static_cast<std::uint32_t>(static_cast<int>(weightAt(i))));
        if (index < 0) return i;
    }
    return -1;
}

#endif // BLOCKPOSSEED_H
