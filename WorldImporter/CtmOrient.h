#pragma once

// Continuity 3.0.0-beta.5 DirectionMaps + OrientationMode.AXIS_ORIENTATIONS.
// Face ordinals are Minecraft Direction: DOWN=0, UP=1, NORTH=2, SOUTH=3, WEST=4, EAST=5.
// Each edge is a block offset (x, y, z). Corners are the sums of the two adjacent edges.
// Compact quadrants stay in texture space (caller keeps quadrantMap identity).

#include <array>
#include <cctype>
#include <string>
#include <string_view>

inline constexpr int kCtmOrientNone = 0;
inline constexpr int kCtmOrientStateAxis = 1;
inline constexpr int kCtmOrientTexture = 2;

// [axis x,y,z][face down,up,north,south,west,east]
inline constexpr int kCtmAxisOrientations[3][6] = {
    {3, 3, 1, 3, 0, 2},
    {0, 0, 0, 0, 0, 0},
    {2, 0, 2, 0, 1, 3},
};

struct CtmFaceDirs {
    std::array<int, 3> up{};
    std::array<int, 3> down{};
    std::array<int, 3> left{};
    std::array<int, 3> right{};
    std::array<int, 3> upLeft{};
    std::array<int, 3> upRight{};
    std::array<int, 3> downLeft{};
    std::array<int, 3> downRight{};
};

namespace ctm_orient_detail {

enum Dir : int { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };

inline constexpr int kOffset[6][3] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0},
};

inline int AxisOf(int d) {
    if (d == Down || d == Up) return 1;     // Y
    if (d == North || d == South) return 2; // Z
    return 0;                               // X
}

inline bool Negative(int d) { return d == Down || d == North || d == West; }

inline int Opposite(int d) { return d ^ 1; }

// Mojang Direction.getClockWise(axis). Directions on that axis stay unchanged.
inline int Clockwise(int d, int axis) {
    if (axis == 1) { // Y: NORTH->EAST->SOUTH->WEST
        switch (d) {
        case North: return East;
        case East: return South;
        case South: return West;
        case West: return North;
        default: return d;
        }
    }
    if (axis == 0) { // X: DOWN->SOUTH->UP->NORTH
        switch (d) {
        case Down: return South;
        case South: return Up;
        case Up: return North;
        case North: return Down;
        default: return d;
        }
    }
    // Z: DOWN->WEST->UP->EAST
    switch (d) {
    case Down: return West;
    case West: return Up;
    case Up: return East;
    case East: return Down;
    default: return d;
    }
}

inline int CounterClockwise(int d, int axis) {
    return Clockwise(Clockwise(Clockwise(d, axis), axis), axis);
}

// commons-lang3 ArrayUtils.shift: element i moves to (i+offset) mod n.
inline void Shift(int arr[4], int offset) {
    int k = offset % 4;
    if (k < 0) k += 4;
    if (k == 0) return;
    int tmp[4];
    for (int i = 0; i < 4; ++i) tmp[i] = arr[i];
    for (int i = 0; i < 4; ++i) arr[(i + k) % 4] = tmp[i];
}

inline std::array<int, 3> Off(int d) {
    return {kOffset[d][0], kOffset[d][1], kOffset[d][2]};
}

inline std::array<int, 3> Add(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

} // namespace ctm_orient_detail

// DirectionMaps.getMap(face)[orientation], as l d r u plus corner sums.
inline CtmFaceDirs CtmDirectionMap(int mcFace, int orientation) {
    using namespace ctm_orient_detail;
    CtmFaceDirs out{};
    if (mcFace < Down || mcFace > East) return out;
    if (orientation < 0 || orientation > 7) orientation = 0;

    const int textureUp = (mcFace == Up) ? North : (mcFace == Down) ? South : Up;
    const int textureLeft = Negative(mcFace)
        ? Clockwise(textureUp, AxisOf(mcFace))
        : CounterClockwise(textureUp, AxisOf(mcFace));

    int map[8][4];
    map[0][0] = textureLeft;
    map[0][1] = Opposite(textureUp);
    map[0][2] = Opposite(textureLeft);
    map[0][3] = textureUp;
    for (int i = 0; i < 4; ++i) map[1][i] = map[0][i];
    Shift(map[1], -1);
    for (int i = 0; i < 4; ++i) map[2][i] = map[1][i];
    Shift(map[2], -1);
    for (int i = 0; i < 4; ++i) map[3][i] = map[2][i];
    Shift(map[3], -1);
    for (int i = 0; i < 4; ++i) map[4][i] = map[0][3 - i]; // reverse(map[0]) = u r d l
    for (int i = 0; i < 4; ++i) map[5][i] = map[4][i];
    Shift(map[5], 1);
    for (int i = 0; i < 4; ++i) map[6][i] = map[5][i];
    Shift(map[6], 1);
    for (int i = 0; i < 4; ++i) map[7][i] = map[6][i];
    Shift(map[7], 1);

    const int* row = map[orientation]; // l d r u
    out.left = Off(row[0]);
    out.down = Off(row[1]);
    out.right = Off(row[2]);
    out.up = Off(row[3]);
    out.upLeft = Add(out.up, out.left);
    out.upRight = Add(out.up, out.right);
    out.downLeft = Add(out.down, out.left);
    out.downRight = Add(out.down, out.right);
    return out;
}

// "axis=" in a block name/state such as "oak_log[axis=y]" or "ns:log[axis=x,waterlogged=false]".
// Returns 0=x, 1=y, 2=z, or -1 when the property is absent.
inline int CtmParseBlockAxis(std::string_view state) {
    constexpr std::string_view key = "axis=";
    for (size_t i = 0; i + key.size() < state.size(); ++i) {
        if (state.compare(i, key.size(), key) != 0) continue;
        if (i > 0) {
            const char prev = state[i - 1];
            if (prev != '[' && prev != ',' && prev != ' ') continue;
        }
        const char a = state[i + key.size()];
        if (a == 'x' || a == 'X') return 0;
        if (a == 'y' || a == 'Y') return 1;
        if (a == 'z' || a == 'Z') return 2;
        return -1;
    }
    return -1;
}

inline int CtmStateAxisOrientation(std::string_view blockState, int mcFace) {
    if (mcFace < 0 || mcFace > 5) return 0;
    const int axis = CtmParseBlockAxis(blockState);
    if (axis < 0) return 0;
    return kCtmAxisOrientations[axis][mcFace];
}

// orient property. Empty / "texture" / unknown -> TEXTURE (ConnectingCtm default).
// "none" -> 0. "state_axis" (also "state-axis") -> axis table, else 0.
inline int CtmOrientMode(std::string_view raw) {
    size_t begin = 0, end = raw.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1]))) --end;
    std::string mode;
    mode.reserve(end - begin);
    for (size_t i = begin; i < end; ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c == '-') c = '_';
        mode.push_back(static_cast<char>(std::tolower(c)));
    }
    if (mode == "none") return kCtmOrientNone;
    if (mode == "state_axis") return kCtmOrientStateAxis;
    return kCtmOrientTexture;
}
