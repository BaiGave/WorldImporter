#pragma once
#include <vector>

std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z);

std::vector<int> DecodeHeightMap(const std::vector<int64_t>& data);