// CtmModGrid.h
// ConnectedTexturesMod (ctm_version 1) 网格贴图取格。
// random / pattern 与 team.chisel.ctm TextureContextGrid 一致。pillar 不在这里。
// x_offset / y_offset 由 Yuushya 忽略, 坐标就是方块坐标。
#ifndef CTM_MOD_GRID_H
#define CTM_MOD_GRID_H

#include <cstdint>

// 原版 Mth.getSeed。x*3129871 是 32 位乘法(回绕后符号扩展), 其后按 64 位回绕。
inline std::int64_t McGetSeedCtm(int x, int y, int z) {
    const std::uint32_t xm =
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(x) * 3129871u);
    const std::uint64_t l0 =
        static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(xm)))
        ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(z)) * 116129781ULL)
        ^ static_cast<std::uint64_t>(static_cast<std::int64_t>(y));
    const std::uint64_t l = l0 * l0 * 42317861ULL + l0 * 11ULL;
    // 等价于 Java long 的算术右移 16
    std::uint64_t shifted = l >> 16;
    if (l & 0x8000000000000000ULL) shifted |= 0xFFFF000000000000ULL;
    return static_cast<std::int64_t>(shifted);
}

// java.util.Random 的 48 位 LCG。乘法用无符号, 避免有符号溢出 UB。
struct JavaRandom {
    std::uint64_t seed = 0;

    void setSeed(std::int64_t s) {
        seed = (static_cast<std::uint64_t>(s) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    }

    // next(bits): seed = (seed * 0x5DEECE66D + 0xB) & mask; return seed >>> (48-bits)
    int next(int bits) {
        seed = (seed * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
        return static_cast<int>(seed >> (48 - bits));
    }

    bool nextBoolean() { return next(1) != 0; }

    // bound 为 2 的幂走快速路径, 否则拒绝采样。比较按 32 位回绕, 与 Java int 溢出一致。
    int nextInt(int bound) {
        if (bound <= 0) return 0;
        if ((bound & (bound - 1)) == 0) {
            const int bits31 = next(31);
            const std::uint64_t product =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(bound)) *
                static_cast<std::uint64_t>(static_cast<std::int64_t>(bits31));
            return static_cast<int>(product >> 31);
        }
        for (;;) {
            const int bits = next(31);
            const int val = bits % bound;
            const std::uint32_t wrapped = static_cast<std::uint32_t>(bits)
                - static_cast<std::uint32_t>(val)
                + static_cast<std::uint32_t>(bound - 1);
            if (static_cast<std::int32_t>(wrapped) >= 0) return val;
        }
    }
};

// side: Direction 序数 DOWN0 UP1 NORTH2 SOUTH3 WEST4 EAST5。col/row 从 0 起, 行 0 是图顶。
// CTM 先 nextInt 再 +1, 贴图子区域再减一格, 所以 0 基下标就是 nextInt 本身。
inline void CtmModGridRandomCell(int x, int y, int z, int side, int w, int h,
    int& col, int& row) {
    JavaRandom rand;
    const std::uint64_t mixed = static_cast<std::uint64_t>(McGetSeedCtm(x, y, z))
        + static_cast<std::uint64_t>(static_cast<std::int64_t>(side));
    rand.setSeed(static_cast<std::int64_t>(mixed));
    rand.nextBoolean();
    col = rand.nextInt(w);
    row = rand.nextInt(h);
}

// TextureContextGrid.Patterned。Java % 向零截断, 负数再加回宽/高。
inline void CtmModGridPatternCell(int x, int y, int z, int side, int w, int h,
    int& col, int& row) {
    col = 0;
    row = 0;
    if (w <= 0 || h <= 0) return;

    auto add = [](int a, int b) {
        return static_cast<int>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
    };
    auto neg = [](int v) {
        return static_cast<int>(0u - static_cast<std::uint32_t>(v));
    };

    int tx, ty;
    if (side <= 1) {
        // DOWN stepY=-1, UP stepY=+1
        const int scaled = (side == 1) ? z : neg(z);
        tx = x % w;
        ty = add(scaled, 1) % h;
    } else if (side <= 3) {
        // NORTH / SOUTH
        tx = x % w;
        ty = neg(y) % h;
    } else {
        // WEST / EAST
        tx = add(z, 1) % w;
        ty = neg(y) % h;
    }
    // NORTH 与 EAST 翻转列
    if (side == 2 || side == 5) tx = add(add(w, neg(tx)), -1) % w;
    if (tx < 0) tx = add(tx, w);
    if (ty < 0) ty = add(ty, h);
    col = tx;
    row = ty;
}

#endif // CTM_MOD_GRID_H
