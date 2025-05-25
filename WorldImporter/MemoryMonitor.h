#ifndef MEMORY_MONITOR_H
#define MEMORY_MONITOR_H

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <shared_mutex>
#include <tuple>

// 需要确保 block.h 或其包含的头文件定义了以下类型
// struct SectionCacheEntry; (在 block.h 中定义)
// class EntityBlock; (在 EntityBlock.h 中定义, 被 block.h 包含)
// struct pair_hash; (在 hashutils.h 中定义, 被 block.h 包含)
// struct triple_hash; (在 hashutils.h 中定义, 被 block.h 包含)
#include "block.h" // 假设 block.h 提供了 SectionCacheEntry 等类型的定义

// 为缓存类型定义别名，以保持清晰，确保与 block.cpp 中的定义一致
using SectionCacheType = std::unordered_map<std::tuple<int, int, int>, SectionCacheEntry, triple_hash>;
using EntityBlockCacheType = std::unordered_map<std::pair<int, int>, std::vector<std::shared_ptr<EntityBlock>>, pair_hash>;
using HeightMapCacheType = std::unordered_map<std::pair<int, int>, std::unordered_map<std::string, std::vector<int>>, pair_hash>;

namespace MemoryMonitor {

void StartMonitoring(
    const SectionCacheType& sectionCache,
    std::shared_mutex& sectionCacheMutex,
    const EntityBlockCacheType& entityBlockCache,
    std::shared_mutex& entityBlockCacheMutex,
    const HeightMapCacheType& heightMapCache,
    std::shared_mutex& heightMapCacheMutex
);

void StopMonitoring();

} // namespace MemoryMonitor

#endif // MEMORY_MONITOR_H 