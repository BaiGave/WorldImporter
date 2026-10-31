// showblockrandom.h
// Yuushya 展台内容的变体随机，对齐 1.20.1 ShowBlockModel：
// 同一个 LegacyRandom，按 DOWN, UP, NORTH, SOUTH, WEST, EAST, 无 cullface 各抽一次。
#ifndef SHOWBLOCKRANDOM_H
#define SHOWBLOCKRANDOM_H

#include "blockposseed.h"

#include <array>
#include <cstdint>
#include <vector>

// java.util.Random.nextLong：((long)next(32) << 32) + next(32)。
// next(32) 先符号扩展再相加，和按 64 位回绕（低 32 位为负时会借位）。
inline std::int64_t ShowRandNextLong(LegacyRandom& rng) {
    const std::int64_t hi = rng.next(32);
    const std::int64_t lo = rng.next(32);
    const std::uint64_t sum = (static_cast<std::uint64_t>(hi) << 32) + static_cast<std::uint64_t>(lo);
    return static_cast<std::int64_t>(sum);
}

// Math.abs(int)：INT_MIN 仍是 INT_MIN。
inline std::int32_t ShowRandJavaAbs(std::int32_t v) {
    const std::uint32_t u = static_cast<std::uint32_t>(v);
    if (v >= 0) return v;
    return static_cast<std::int32_t>(0u - u);
}

// 消耗一次 nextLong。idx = abs((int)nextLong) % total，再按权重累减；负 idx 取第 0 项。
inline int ShowRandWeightedSlot(const std::vector<int>& weights, LegacyRandom& rng) {
    int total = 0;
    for (int w : weights) total += w;
    const std::int32_t raw = static_cast<std::int32_t>(ShowRandNextLong(rng));
    if (total <= 0 || weights.empty()) return 0;
    std::int32_t idx = ShowRandJavaAbs(raw) % total;
    if (idx < 0) return 0;
    for (size_t i = 0; i < weights.size(); ++i) {
        idx = static_cast<std::int32_t>(static_cast<std::uint32_t>(idx) -
                                        static_cast<std::uint32_t>(weights[i]));
        if (idx < 0) return static_cast<int>(i);
    }
    return 0;
}

// Variant.weights：各变体权重。
// Multipart.groups：每个分组若是加权数组则放权重，否则为空（不单独抽）。
struct ShowRandEntry {
    enum Kind { Simple, Variant, Multipart } kind = Simple;
    std::vector<int> weights;
    std::vector<std::vector<int>> groups;
};

// 7 个面（D,U,N,S,W,E,null）各自的槽位。
// Variant 一个元素；Simple 为 {0}；Multipart 每个分组一个，未加权或不足 2 个模型为 0。
// 变体权重不足 2 个或总权重 <= 0 时不消耗随机数。
inline std::array<std::vector<int>, 7> ShowRandPickEntry(const ShowRandEntry& entry, LegacyRandom& rng) {
    std::array<std::vector<int>, 7> out{};
    if (entry.kind == ShowRandEntry::Simple) {
        for (auto& side : out) side.push_back(0);
        return out;
    }
    if (entry.kind == ShowRandEntry::Variant) {
        int total = 0;
        for (int w : entry.weights) total += w;
        const bool plain = entry.weights.size() < 2 || total <= 0;
        for (auto& side : out) {
            side.push_back(plain ? 0 : ShowRandWeightedSlot(entry.weights, rng));
        }
        return out;
    }
    for (auto& side : out) {
        const std::int64_t seed = ShowRandNextLong(rng);
        side.resize(entry.groups.size());
        for (size_t g = 0; g < entry.groups.size(); ++g) {
            const std::vector<int>& w = entry.groups[g];
            int total = 0;
            for (int x : w) total += x;
            if (w.size() >= 2 && total > 0) {
                LegacyRandom fresh(seed);
                side[g] = ShowRandWeightedSlot(w, fresh);
            } else {
                side[g] = 0;
            }
        }
    }
    return out;
}

#endif // SHOWBLOCKRANDOM_H
