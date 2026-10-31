// CtmRandom.h
// Continuity 3.0.0-beta.5 random CTM 的纯计算（RandomSpriteProvider）。
// 种子是原版 Mth.getSeed，混合是 MathUtil.mix，下标用 Math.abs 取模。
// 对称面和 linked 列由调用方处理。乘法按 Java 回绕，用无符号避免有符号溢出 UB。
#ifndef CTM_RANDOM_H
#define CTM_RANDOM_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ctm_random_detail {

// SplittableRandom mix64。>>> 是逻辑右移，乘法按 64 位回绕。
inline std::uint64_t Mix64(std::uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

// SplittableRandom mix32，结果取高 32 位再按有符号 int 解释。
inline std::int32_t Mix32(std::uint64_t z) {
    z = (z ^ (z >> 33)) * 0x62a9d9ed799705f5ULL;
    return static_cast<std::int32_t>(((z ^ (z >> 28)) * 0xcb24d0a5c88c35b3ULL) >> 32);
}

// Math.abs(int) 后对正模数取余。INT_MIN 的绝对值仍是 INT_MIN，余数为负时加回模数。
inline int JavaAbsMod(std::int32_t value, int modulus) {
    if (modulus <= 0) return 0;
    const std::uint32_t bits = value < 0
        ? (0u - static_cast<std::uint32_t>(value))
        : static_cast<std::uint32_t>(value);
    const int rem = static_cast<std::int32_t>(bits) % modulus;
    return rem < 0 ? rem + modulus : rem;
}

inline int WrapAdd(int a, int b) {
    return static_cast<int>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

} // namespace ctm_random_detail

// 原版 Mth.getSeed(x, y, z)。
// x*3129871 在 Java 里是 32 位乘法（回绕后再符号扩展），后面整段按 64 位运算。
inline std::int64_t McGetSeed(int x, int y, int z) {
    const std::uint32_t xm =
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(x) * 3129871u);
    const std::uint64_t l0 =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(xm)))
        ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * 116129781ULL)
        ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    const std::uint64_t l = l0 * l0 * 42317861ULL + l0 * 11ULL;
    // 等价于 Java 的 (long) >> 16（算术右移）
    std::uint64_t shifted = l >> 16;
    if (l & 0x8000000000000000ULL) shifted |= 0xFFFF000000000000ULL;
    return static_cast<std::int64_t>(shifted);
}

// MathUtil.mix: mix32((getSeed(x,y,z) ^ mix64(G*(1+face))) + G*(1+loops))
inline std::int32_t ContinuityRandomMix(int x, int y, int z, int face, int loops) {
    constexpr std::uint64_t kGamma = 0x9e3779b97f4a7c15ULL;
    const std::uint64_t mixed =
        static_cast<std::uint64_t>(McGetSeed(x, y, z))
        ^ ctm_random_detail::Mix64(
            kGamma * static_cast<std::uint64_t>(static_cast<std::int64_t>(1) + face));
    const std::uint64_t withLoops =
        mixed + kGamma * static_cast<std::uint64_t>(static_cast<std::int64_t>(1) + loops);
    return ctm_random_detail::Mix32(withLoops);
}

// 无权重（weights 为空）: Math.abs(r) % size。
// 有权重: 原样拷贝前 min(len, size) 个（不钳制），不足的格子用 sum/copied 的整数平均值补上。
// sum<=0 时退回无权重，避免除零或对非正数取模。
inline int ContinuityRandomIndex(std::int32_t r, int size, const std::vector<int>& weights) {
    if (size <= 0) return 0;
    if (weights.empty()) return ctm_random_detail::JavaAbsMod(r, size);

    const int copied = static_cast<int>(
        weights.size() < static_cast<std::size_t>(size) ? weights.size()
                                                        : static_cast<std::size_t>(size));
    int sum = 0;
    for (int i = 0; i < copied; ++i)
        sum = ctm_random_detail::WrapAdd(sum, weights[static_cast<std::size_t>(i)]);

    int average = 0;
    if (copied < size) {
        average = sum / copied; // copied >= 1；向零截断，与 Java int 除法一致
        for (int i = copied; i < size; ++i)
            sum = ctm_random_detail::WrapAdd(sum, average);
    }
    if (sum <= 0) return ctm_random_detail::JavaAbsMod(r, size);

    int temp = ctm_random_detail::JavaAbsMod(r, sum);
    int index = 0;
    for (; index < size - 1; ++index) {
        const int weight = index < copied ? weights[static_cast<std::size_t>(index)] : average;
        if (temp < weight) break;
        temp = static_cast<int>(static_cast<std::uint32_t>(temp) - static_cast<std::uint32_t>(weight));
    }
    return index;
}

#endif // CTM_RANDOM_H
