#ifndef COORD_CONVERSION_H
#define COORD_CONVERSION_H
#include <tuple>
extern int minSectionY;

// 计算 YZX 编码后的数字
int toYZX(int x, int y, int z);

//如果结果是负数,添加16,使其变为正数
int mod16(int value);

//如果结果是负数,添加32,使其变为正数
int mod32(int value);

// 函数声明:区块坐标转换为区域坐标
void chunkToRegion(int chunkX, int chunkZ, int& regionX, int& regionZ);

// 函数声明:方块坐标转换为区块坐标
void blockToChunk(int blockX, int blockZ, int& chunkX, int& chunkZ);

// 函数声明:方块Y坐标转换为子区块Y坐标
void blockYToSectionY(int blockY, int& chunkY);

int AdjustSectionY(int SectionY);


#endif // COORD_CONVERSION_H
