#include "coord_conversion.h"
#include <cmath>
#include <iostream>
#include <string>

// 计算 YZX 编码后的数字
int toYZX(int x, int y, int z) {
    int encoded = (y << 8) | (z << 4) | x;
    //std::cout << "Encoded YZX (" << x << ", " << y << ", " << z << ") -> " << encoded << std::endl;
    return encoded;
}

// 如果结果是负数，添加16，使其变为正数，确保返回的坐标在 [0, 15] 范围内
int mod16(int value) {
    int result = value % 16;  // 计算余数
    if (result < 0) {
        result += 16;  // 如果是负数，转换为正数
    }
    return result;
}


// 如果结果是负数，添加32，使其变为正数，确保返回的坐标在 [0, 31] 范围内
int mod32(int value) {
    int result = value % 32;
    return result < 0 ? result + 32 : result;
}

// 区块坐标转换为区域坐标
void chunkToRegion(int chunkX, int chunkZ, int& regionX, int& regionZ) {
    // 使用位移进行转换
    regionX = chunkX >> 5;  // 相当于 chunkX / 32
    regionZ = chunkZ >> 5;  // 相当于 chunkZ / 32
}

// 方块坐标转换为区块坐标
void blockToChunk(int blockX, int blockZ, int& chunkX, int& chunkZ) {
    // 使用位移进行转换
    chunkX = blockX >> 4;  // 相当于 chunkX / 16
    chunkZ = blockZ >> 4;  // 相当于 chunkZ / 16
}

// 方块Y坐标转换为子区块Y坐标
void blockYToSectionY(int blockY, int& chunkY) {
    chunkY = blockY >> 4; // 相当于 chunkY / 16
}

int AdjustSectionY(int SectionY) {
    const int OFFSET = 64;
    return SectionY + OFFSET;
}