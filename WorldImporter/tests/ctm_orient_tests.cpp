// DirectionMaps[face][orientation] against a Java port of Continuity 3.0.0-beta.5 DirectionMaps.
// Letters are l d r u. Face order is Minecraft: DOWN UP NORTH SOUTH WEST EAST.
#include "CtmOrient.h"

#include <array>
#include <cstdio>
#include <string>

static int g_fails = 0;

static void Fail(const std::string& message) {
    std::fprintf(stderr, "FAIL %s\n", message.c_str());
    ++g_fails;
}

static bool Letter(char c, int& x, int& y, int& z) {
    switch (c) {
    case 'D': x = 0; y = -1; z = 0; return true;
    case 'U': x = 0; y = 1; z = 0; return true;
    case 'N': x = 0; y = 0; z = -1; return true;
    case 'S': x = 0; y = 0; z = 1; return true;
    case 'W': x = -1; y = 0; z = 0; return true;
    case 'E': x = 1; y = 0; z = 0; return true;
    default: return false;
    }
}

static bool Decode(const char* text, int out[4][3]) {
    const char* p = text;
    for (int i = 0; i < 4; ++i) {
        if (!Letter(*p, out[i][0], out[i][1], out[i][2])) return false;
        ++p;
        if (i < 3) {
            if (*p != ' ') return false;
            ++p;
        }
    }
    return *p == '\0';
}

static bool Same(const std::array<int, 3>& a, int x, int y, int z) {
    return a[0] == x && a[1] == y && a[2] == z;
}

static std::array<int, 3> Add(const std::array<int, 3>& a, const std::array<int, 3>& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

// Copied from javaref/dirref.txt. Do not edit by hand without rerunning DirRef.
static const char* kMap[6][8] = {
    // DOWN
    {"W N E S", "N E S W", "E S W N", "S W N E", "S E N W", "W S E N", "N W S E", "E N W S"},
    // UP
    {"W S E N", "S E N W", "E N W S", "N W S E", "N E S W", "W N E S", "S W N E", "E S W N"},
    // NORTH
    {"E D W U", "D W U E", "W U E D", "U E D W", "U W D E", "E U W D", "D E U W", "W D E U"},
    // SOUTH
    {"W D E U", "D E U W", "E U W D", "U W D E", "U E D W", "W U E D", "D W U E", "E D W U"},
    // WEST
    {"N D S U", "D S U N", "S U N D", "U N D S", "U S D N", "N U S D", "D N U S", "S D N U"},
    // EAST
    {"S D N U", "D N U S", "N U S D", "U S D N", "U N D S", "S U N D", "D S U N", "N D S U"},
};

static const char* kFace[6] = {"DOWN", "UP", "NORTH", "SOUTH", "WEST", "EAST"};

static void CheckMaps() {
    for (int face = 0; face < 6; ++face) {
        for (int orientation = 0; orientation < 8; ++orientation) {
            int expect[4][3];
            if (!Decode(kMap[face][orientation], expect)) {
                Fail(std::string("bad table ") + kFace[face] + " " + std::to_string(orientation));
                continue;
            }
            const CtmFaceDirs dirs = CtmDirectionMap(face, orientation);
            const std::array<int, 3>* got[4] = {&dirs.left, &dirs.down, &dirs.right, &dirs.up};
            bool edgesOk = true;
            for (int i = 0; i < 4; ++i) {
                if (!Same(*got[i], expect[i][0], expect[i][1], expect[i][2])) edgesOk = false;
            }
            if (!edgesOk) {
                Fail(std::string(kFace[face]) + " " + std::to_string(orientation)
                    + " got edges, expected " + kMap[face][orientation]);
            }
            if (dirs.upLeft != Add(dirs.up, dirs.left) ||
                dirs.upRight != Add(dirs.up, dirs.right) ||
                dirs.downLeft != Add(dirs.down, dirs.left) ||
                dirs.downRight != Add(dirs.down, dirs.right)) {
                Fail(std::string(kFace[face]) + " " + std::to_string(orientation) + " corner sum");
            }
        }
    }
}

static void CheckAxis() {
    const int table[3][6] = {
        {3, 3, 1, 3, 0, 2},
        {0, 0, 0, 0, 0, 0},
        {2, 0, 2, 0, 1, 3},
    };
    for (int axis = 0; axis < 3; ++axis) {
        for (int face = 0; face < 6; ++face) {
            if (kCtmAxisOrientations[axis][face] != table[axis][face]) {
                Fail("AXIS table [" + std::to_string(axis) + "][" + std::to_string(face) + "]");
            }
        }
    }
    const char* states[3] = {
        "oak_log[axis=x]",
        "minecraft:stripped_spruce_log[axis=y]",
        "bamboo_block[axis=z,waterlogged=false]",
    };
    for (int axis = 0; axis < 3; ++axis) {
        for (int face = 0; face < 6; ++face) {
            const int got = CtmStateAxisOrientation(states[axis], face);
            if (got != table[axis][face]) {
                Fail(std::string(states[axis]) + " face " + kFace[face]
                    + " -> " + std::to_string(got));
            }
        }
    }
    if (CtmStateAxisOrientation("stone", 0) != 0) Fail("no axis property");
    if (CtmStateAxisOrientation("minecraft:stone", 3) != 0) Fail("no axis full name");
    if (CtmStateAxisOrientation("custom[myaxis=x]", 0) != 0) Fail("myaxis must not match");
    if (CtmStateAxisOrientation("pillar[axis=X]", 2) != 1) Fail("axis=X north");
    if (CtmStateAxisOrientation("pillar[axis=Z]", 5) != 3) Fail("axis=Z east");
    if (CtmStateAxisOrientation("ns:log[foo=1,axis=x]", 4) != 0) Fail("axis mid-list west");
    if (CtmParseBlockAxis("hay_block[axis=y]") != 1) Fail("parse y");
    if (CtmParseBlockAxis("hay_block") != -1) Fail("parse missing");
}

int main() {
    CheckMaps();
    CheckAxis();
    if (g_fails != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_fails);
        return 1;
    }
    std::printf("ctm_orient ok (48 maps + axis)\n");
    return 0;
}
