#include "model.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

Config config;

namespace {
struct Case {
    float region[4]; int rotation; FaceType face; int x, y;
    float expRegion[4]; int expRotation;
};
// Expected values follow vanilla 1.20.1 FaceBakery.recomputeUVs (incl. the
// rotation sign flip: floorMod(-round(atan2/90)*90, 360)).
const Case cases[] = {
    {{0, 0, 16, 16}, 90, UP, 0, 0, {0, 0, 16, 16}, 270},      // uvlock flips face rotation even unrotated
    {{0, 0, 16, 16}, 0, DOWN, 0, 90, {0, 0, 16, 16}, 90},     // bottom turns opposite to top
    {{0, 0, 16, 16}, 0, UP, 0, 90, {0, 0, 16, 16}, 270},
    {{0, 0, 16, 16}, 90, NORTH, 0, 270, {0, 0, 16, 16}, 270}, // side faces keep region under y rotation
    {{0, 0, 8, 16}, 0, DOWN, 0, 90, {0, 8, 16, 16}, 90},
    {{2, 4, 6, 10}, 0, NORTH, 90, 0, {10, 6, 14, 12}, 180},
    {{2, 4, 6, 10}, 0, UP, 90, 180, {10, 6, 14, 12}, 180},
    {{16, 0, 0, 16}, 0, EAST, 270, 90, {16, 0, 0, 16}, 90},   // mirrored u kept
};
} // namespace

int main() {
    int failures = 0;
    for (const auto& c : cases) {
        float region[4];
        int rotation = -1;
        RecomputeUvLock(c.region, c.rotation, c.face, c.x, c.y, region, rotation);
        bool ok = rotation == c.expRotation;
        for (int i = 0; i < 4; ++i) ok = ok && std::fabs(region[i] - c.expRegion[i]) < 1e-4f;
        if (!ok) {
            ++failures;
            std::cerr << "FAIL face " << c.face << " x" << c.x << " y" << c.y << ": got [" << region[0] << ","
                      << region[1] << "," << region[2] << "," << region[3] << "] rot " << rotation << '\n';
        }
    }
    if (failures) return 1;
    std::cout << "PASS: uvlock matches vanilla recomputeUVs\n";
    return 0;
}
