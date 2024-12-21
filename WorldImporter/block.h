#ifndef BLOCK_H
#define BLOCK_H

#include <vector>
#include <string>


// ��ȡ���� NBT ���ݵĺ���
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z);

// ͨ��x, y, z �����ȡ���� ID
int GetBlockId(int blockX, int blockY, int blockZ);

std::string GetBlockNameById(int blockId);


std::unordered_map<int, std::string> GetGlobalBlockPalette();


void InitializeGlobalBlockPalette();

#endif  // BLOCK_H
