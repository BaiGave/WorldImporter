#ifndef BLOCK_H
#define BLOCK_H

#include "config.h"
extern Config config;  // 声明外部变量

#include <vector>
#include <string>

// 获取区块 NBT 数据的函数
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z);

// 通过x, y, z 坐标获取方块 ID
int GetBlockId(int blockX, int blockY, int blockZ);

std::string GetBlockNameById(int blockId);


std::unordered_map<int, std::string> GetGlobalBlockPalette();


void InitializeGlobalBlockPalette();

#endif  // BLOCK_H
