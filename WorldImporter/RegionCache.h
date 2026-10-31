#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>
#include "config.h"

// regionCache: 缓存已读取的区域文件(.mca)。
// 使用 shared_ptr 持有数据:缓存超限整体清空时,正在使用数据的调用方
// 持有的 shared_ptr 仍然有效,不会悬空。
extern std::map<std::pair<int, int>, std::shared_ptr<const std::vector<char>>> regionCache;
extern std::mutex regionCacheMutex;

// 返回共享指针,调用方可安全持有(即使期间缓存被清空)
std::shared_ptr<const std::vector<char>> GetRegionFromCache(int regionX, int regionZ);

// 新增:判断指定 chunk 是否存在于 region 文件中
bool HasChunk(int chunkX, int chunkZ);
