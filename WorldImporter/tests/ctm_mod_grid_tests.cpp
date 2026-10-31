// gridref.txt 的 48 条向量(GridRef.java)。同时核对 random 与 pattern 取格。
#include "CtmModGrid.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

struct Vec {
    int x, y, z, side, w, h, randCol, randRow, patCol, patRow;
};

// x y z side w h randCol randRow patCol patRow
constexpr Vec kVecs[] = {
    {-70, 38, 2, 0, 4, 4, 2, 0, 2, 3},
    {69, 204, -128, 1, 3, 1, 0, 0, 0, 0},
    {-70, -3, -99, 2, 2, 1, 1, 0, 1, 0},
    {-183, 269, 156, 3, 4, 3, 2, 0, 1, 1},
    {-86, 225, -41, 4, 1, 1, 0, 0, 0, 0},
    {-68, 227, -147, 5, 2, 2, 0, 0, 1, 1},
    {-127, -56, -175, 0, 2, 2, 1, 0, 1, 0},
    {-172, 305, 167, 1, 1, 1, 0, 0, 0, 0},
    {-113, 31, -92, 2, 1, 2, 0, 0, 0, 1},
    {-20, 181, 80, 3, 2, 2, 1, 0, 0, 1},
    {-199, 283, 54, 4, 2, 4, 0, 2, 1, 1},
    {95, 1, -114, 5, 2, 3, 0, 1, 0, 2},
    {87, 27, -48, 0, 1, 1, 0, 0, 0, 0},
    {-166, -41, -20, 1, 4, 4, 3, 2, 2, 1},
    {82, 127, 22, 2, 2, 1, 1, 0, 1, 0},
    {142, 194, -19, 3, 4, 3, 1, 2, 2, 1},
    {122, 131, 145, 4, 1, 1, 0, 0, 0, 0},
    {173, -44, 156, 5, 3, 1, 2, 0, 1, 0},
    {-156, 52, 98, 0, 2, 1, 0, 0, 0, 0},
    {130, 31, -137, 1, 3, 1, 2, 0, 1, 0},
    {89, -52, 36, 2, 2, 4, 1, 0, 0, 0},
    {23, 168, 42, 3, 3, 1, 0, 0, 2, 0},
    {-151, -38, -111, 4, 1, 4, 0, 0, 0, 2},
    {189, 286, 16, 5, 2, 4, 1, 0, 0, 2},
    {-192, 303, 69, 0, 4, 2, 1, 1, 0, 0},
    {-144, 75, -150, 1, 4, 3, 1, 0, 0, 1},
    {-8, 193, -186, 2, 4, 3, 2, 2, 3, 2},
    {-79, -49, -153, 3, 1, 2, 0, 1, 0, 1},
    {90, 193, 27, 4, 2, 3, 0, 1, 0, 2},
    {45, 103, 101, 5, 4, 1, 3, 0, 1, 0},
    {79, 75, 158, 0, 2, 1, 0, 0, 1, 0},
    {-73, 131, -144, 1, 2, 4, 0, 1, 1, 1},
    {-33, 47, 131, 2, 4, 2, 2, 1, 0, 1},
    {175, 168, 160, 3, 4, 3, 3, 1, 3, 0},
    {-4, -3, 4, 4, 1, 3, 0, 2, 0, 0},
    {155, 180, -66, 5, 4, 1, 3, 0, 0, 0},
    {92, 151, 199, 0, 3, 3, 0, 2, 2, 0},
    {-9, 303, -197, 1, 3, 3, 0, 1, 0, 2},
    {-55, 166, -93, 2, 3, 3, 2, 0, 0, 2},
    {-186, 6, -131, 3, 1, 2, 0, 0, 0, 0},
    {143, 2, 26, 4, 4, 1, 0, 0, 3, 0},
    {86, 290, 198, 5, 3, 3, 2, 2, 1, 1},
    {-159, 167, 119, 0, 2, 4, 0, 3, 1, 2},
    {-76, 233, 30, 1, 2, 4, 1, 3, 0, 3},
    {-99, 126, -125, 2, 2, 2, 0, 0, 0, 0},
    {146, 99, -82, 3, 4, 2, 3, 1, 2, 1},
    {-29, -31, 176, 4, 4, 2, 1, 0, 1, 1},
    {-123, 218, 109, 5, 1, 2, 0, 0, 0, 0},
};

static_assert(sizeof(kVecs) / sizeof(kVecs[0]) == 48, "gridref has 48 vectors");

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void checkCells() {
    for (int i = 0; i < 48; ++i) {
        const Vec& v = kVecs[i];
        int col = -1, row = -1;
        CtmModGridRandomCell(v.x, v.y, v.z, v.side, v.w, v.h, col, row);
        require(col == v.randCol && row == v.randRow,
            "random[" + std::to_string(i) + "] got " + std::to_string(col) + "," +
            std::to_string(row) + " expected " + std::to_string(v.randCol) + "," +
            std::to_string(v.randRow));
        col = -1;
        row = -1;
        CtmModGridPatternCell(v.x, v.y, v.z, v.side, v.w, v.h, col, row);
        require(col == v.patCol && row == v.patRow,
            "pattern[" + std::to_string(i) + "] got " + std::to_string(col) + "," +
            std::to_string(row) + " expected " + std::to_string(v.patCol) + "," +
            std::to_string(v.patRow));
    }
}

} // namespace

int main() {
    try {
        checkCells();
        std::cout << "ctm mod grid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
