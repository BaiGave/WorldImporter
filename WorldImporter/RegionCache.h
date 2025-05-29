#pragma once

#include <unordered_map>
#include <vector>
#include <utility>
#include "hashutils.h"
#include "config.h"

extern std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache;
const std::vector<char>& GetRegionFromCache(int regionX, int regionZ);

// 新增:判断指定 chunk 是否存在于 region 文件中
bool HasChunk(int chunkX, int chunkZ);