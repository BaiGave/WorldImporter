// CtmModPillar.h
// CTM-mod type=pillar 选格, 与 TexturePillar.getQuad / TextureContextPillar
// 以及 scripts/javaref/PillarRef.java 对齐。
//
// same(dx,dy,dz) 表示相对当前方块的偏移处是否与中心 BlockState 相同。
// 邻居自己的连接会再向外看一格, 所以轴向偏移到 ±2; 更远或不相等视为空气。
#ifndef CTM_MOD_PILLAR_H
#define CTM_MOD_PILLAR_H

#include <type_traits>

// 顺序与原版 Direction 一致: DOWN UP NORTH SOUTH WEST EAST
enum class CtmPillarFace : int {
    Down = 0, Up, North, South, West, East
};

struct CtmPillarQuad {
    int sprite;    // 0 基础贴图整幅, 1 ctm 图集
    int col;       // Submap.X2 列 (sprite==1)
    int row;       // Submap.X2 行, 0 = 图像顶部 (sprite==1)
    int rotation;  // Quad.rotate 次数, 0..3
};

// Quad.rotate 在 Minecraft UV (u 向右, v 向下, 贴图内 0..1):
//   1 (u,v)->(v,1-u); 2 ->(1-u,1-v); 3 ->(1-v,u)
// 本工程 OBJ UV 的 v 向上, v_mc = 1-v, 转完再换回来:
//   1 (u,v)->(1-v,u); 2 ->(1-u,1-v); 3 ->(v,1-u)
// 4 与 0 一样不转 (Quad.rotate 的 default 分支)。
inline void RotateCtmPillarUv(float& u, float& v, int amount) {
    amount %= 4;
    if (amount < 0) amount += 4;
    const float ou = u;
    const float ov = v;
    switch (amount) {
    case 1: u = 1.0f - ov; v = ou; break;
    case 2: u = 1.0f - ou; v = 1.0f - ov; break;
    case 3: u = ov; v = 1.0f - ou; break;
    default: break;
    }
}

namespace ctm_pillar_detail {

struct Off { int x, y, z; };
constexpr Off kOff[6] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
};
constexpr int kDown = 0, kUp = 1, kNorth = 2, kSouth = 3, kWest = 4, kEast = 5;

constexpr int opposite(int d) { return d ^ 1; }
constexpr bool horizontal(int d) { return kOff[d].y == 0; }
constexpr char axisOf(int d) {
    if (kOff[d].x) return 'X';
    if (kOff[d].y) return 'Y';
    return 'Z';
}
constexpr int clockWise(int d) {
    switch (d) {
    case kNorth: return kEast;
    case kEast: return kSouth;
    case kSouth: return kWest;
    case kWest: return kNorth;
    default: return d;
    }
}
constexpr int rotateX(int d) {
    switch (d) {
    case kNorth: return kDown;
    case kSouth: return kUp;
    case kUp: return kNorth;
    case kDown: return kSouth;
    default: return d;
    }
}
constexpr int rotateZ(int d) {
    switch (d) {
    case kEast: return kDown;
    case kWest: return kUp;
    case kUp: return kEast;
    case kDown: return kWest;
    default: return d;
    }
}
constexpr int rotateAround(int d, char axis) {
    if (axis == 'X') return (d != kWest && d != kEast) ? rotateX(d) : d;
    if (axis == 'Y') return (d != kUp && d != kDown) ? clockWise(d) : d;
    return (d != kNorth && d != kSouth) ? rotateZ(d) : d;
}

constexpr bool has(int mask, int d) { return (mask & (1 << d)) != 0; }
constexpr bool cor(int mask, int a, int b) { return has(mask, a) || has(mask, b); }
constexpr bool cand(int mask, int a, int b) { return has(mask, a) && has(mask, b); }

template <class SameFn>
int connectionsAt(SameFn& same, int x, int y, int z) {
    if (!same(x, y, z)) return 0;
    int mask = 0;
    for (int f = 0; f < 6; ++f) {
        if (same(x + kOff[f].x, y + kOff[f].y, z + kOff[f].z)) mask |= 1 << f;
    }
    return mask;
}

inline bool blockConnection(const int nb[6], int dir, char axis) {
    const int rot = rotateAround(dir, axis);
    return cor(nb[dir], rot, opposite(rot));
}
inline bool blockConnectionY(const int nb[6], int dir) {
    return blockConnection(nb, dir, 'Y') || blockConnection(nb, dir, axisOf(clockWise(dir)));
}

inline void getUVs(int mask, int f1, int f2, int& col, int& row) {
    // X2[1][0]=col0 row1, X2[1][1]=col1 row1, X2[0][1]=col1 row0
    if (cand(mask, f1, f2)) { col = 0; row = 1; }
    else if (has(mask, f1)) { col = 1; row = 1; }
    else { col = 1; row = 0; }
}

} // namespace ctm_pillar_detail

// 数组另有重载; 这里排除数组, 否则模板会抢到 bool[5][5][5] 的精确匹配。
template <class SameFn>
    requires (!std::is_array_v<std::remove_cvref_t<SameFn>>)
CtmPillarQuad DecideCtmModPillar(CtmPillarFace face, SameFn&& same) {
    using namespace ctm_pillar_detail;
    const int f = static_cast<int>(face);
    const int cons0 = connectionsAt(same, 0, 0, 0);
    int nb[6];
    for (int d = 0; d < 6; ++d)
        nb[d] = connectionsAt(same, kOff[d].x, kOff[d].y, kOff[d].z);

    int real = cons0;
    if (cor(cons0, kUp, kDown)) {
        for (int d = 0; d < 6; ++d)
            if (horizontal(d)) real &= ~(1 << d);
    } else if (cor(cons0, kEast, kWest)) {
        real &= ~(1 << kNorth);
        real &= ~(1 << kSouth);
        for (int d = 0; d < 6; ++d)
            if ((real & (1 << d)) && blockConnection(nb, d, 'Z')) real &= ~(1 << d);
    } else {
        for (int d = 0; d < 6; ++d)
            if ((real & (1 << d)) && blockConnectionY(nb, d)) real &= ~(1 << d);
    }

    const int cons = real;
    int rotation = 0;
    int col = 0, row = 0;
    if (horizontal(f) && cor(cons, kUp, kDown)) {
        getUVs(cons, kUp, kDown, col, row);
    } else if (cor(cons, kEast, kWest)) {
        rotation = 1;
        getUVs(cons, kEast, kWest, col, row);
    } else if (cor(cons, kNorth, kSouth)) {
        getUVs(cons, kNorth, kSouth, col, row);
        if (f == kDown) rotation += 2;
    }

    bool connected = cons != 0;
    if (connected && !cor(cons, kUp, kDown)) {
        if (f == kEast) rotation += 1;
        if (f == kNorth) rotation += 2;
        if (f == kWest) rotation += 3;
    }
    if (has(cons, opposite(f))) connected = false;
    if (cons == 0 && horizontal(f)) connected = true;

    // 可达的最大值是 4 (西面 + 东西轴), Quad.rotate(4) 走 default = 不转。
    rotation %= 4;
    if (!connected) return {0, 0, 0, rotation};
    return {1, col, row, rotation};
}

inline CtmPillarQuad DecideCtmModPillar(CtmPillarFace face, const bool (&same)[5][5][5]) {
    return DecideCtmModPillar(face, [&](int dx, int dy, int dz) {
        if (static_cast<unsigned>(dx + 2) > 4u || static_cast<unsigned>(dy + 2) > 4u ||
            static_cast<unsigned>(dz + 2) > 4u)
            return false;
        return same[dx + 2][dy + 2][dz + 2];
    });
}

#endif
