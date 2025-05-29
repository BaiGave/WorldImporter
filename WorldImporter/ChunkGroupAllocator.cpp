// chunk_group_allocator.cpp
#include "ChunkGroupAllocator.h"
#include "LODManager.h" // 包含LODManager.h以访问g_chunkLODs
#include <iostream> // 用于潜在的调试输出
#include <limits> // 新增:用于 numeric_limits
#include <algorithm>
#include <shared_mutex>
#undef max
#undef min

namespace ChunkGroupAllocator {

    std::vector<ChunkGroup> g_chunkGroups; // 定义全局变量
    std::vector<ChunkBatch> g_chunkBatches; // 定义批处理全局变量
    void GenerateChunkGroups(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd)
    {
        g_chunkGroups.clear(); // 清空之前的分组
        int partitionSize = config.partitionSize;
        int groupsX = ((chunkXEnd - chunkXStart) / partitionSize) + 1;
        int groupsZ = ((chunkZEnd - chunkZStart) / partitionSize) + 1;
        g_chunkGroups.reserve(groupsX * groupsZ);
        for (int groupX = chunkXStart; groupX <= chunkXEnd; groupX += partitionSize) {
            int currentGroupXEnd = groupX + partitionSize - 1;
            if (currentGroupXEnd > chunkXEnd) currentGroupXEnd = chunkXEnd;

            for (int groupZ = chunkZStart; groupZ <= chunkZEnd; groupZ += partitionSize) {
                int currentGroupZEnd = groupZ + partitionSize - 1;
                if (currentGroupZEnd > chunkZEnd) currentGroupZEnd = chunkZEnd;

                ChunkGroup newGroup;
                newGroup.startX = groupX;
                newGroup.startZ = groupZ;
                int numChunksX = currentGroupXEnd - groupX + 1;
                int numChunksZ = currentGroupZEnd - groupZ + 1;
                int numSectionsY = sectionYEnd - sectionYStart + 1;
                newGroup.tasks.reserve(numChunksX * numChunksZ * numSectionsY);

                for (int chunkX = groupX; chunkX <= currentGroupXEnd; ++chunkX) {
                    for (int chunkZ = groupZ; chunkZ <= currentGroupZEnd; ++chunkZ) {
                        for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                            ChunkTask task;
                            task.chunkX = chunkX;
                            task.chunkZ = chunkZ;
                            task.sectionY = sectionY;

                            // 从全局g_chunkSectionInfoMap获取LOD等级
                            auto key = std::make_tuple(chunkX, sectionY, chunkZ);
                            {
                                std::shared_lock<std::shared_mutex> readLock(g_chunkSectionInfoMapMutex);
                                auto it = g_chunkSectionInfoMap.find(key);
                                if (it != g_chunkSectionInfoMap.end()) {
                                    task.lodLevel = it->second.lodLevel;
                                } else {
                                    task.lodLevel = 0.0f;
                                }
                            }

                            newGroup.tasks.push_back(task);
                        }
                    }
                }

                g_chunkGroups.push_back(newGroup);
            }
        }
    }

    // 新增:将区块组划分为批次
    void GenerateChunkBatches(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd,
        size_t maxTasksPerBatch)
    {
        // 首先生成区块组
        GenerateChunkGroups(chunkXStart, chunkXEnd,
                            chunkZStart, chunkZEnd,
                            sectionYStart, sectionYEnd);

        g_chunkBatches.clear();

        ChunkBatch currentBatch;
        currentBatch.chunkXStart = std::numeric_limits<int>::max();
        currentBatch.chunkZStart = std::numeric_limits<int>::max();
        currentBatch.chunkXEnd   = std::numeric_limits<int>::min();
        currentBatch.chunkZEnd   = std::numeric_limits<int>::min();

        size_t currentTaskCount = 0;

        auto flushCurrentBatch = [&]() {
            if (!currentBatch.groups.empty()) {
                g_chunkBatches.push_back(currentBatch);
                // 重新初始化
                currentBatch = ChunkBatch{};
                currentBatch.chunkXStart = std::numeric_limits<int>::max();
                currentBatch.chunkZStart = std::numeric_limits<int>::max();
                currentBatch.chunkXEnd   = std::numeric_limits<int>::min();
                currentBatch.chunkZEnd   = std::numeric_limits<int>::min();
                currentTaskCount = 0;
            }
        };

        for (const auto& group : g_chunkGroups) {
            size_t groupTaskCount = group.tasks.size();

            // 如果当前批次任务数超出限制,则先刷入当前批次
            if (currentTaskCount + groupTaskCount > maxTasksPerBatch && !currentBatch.groups.empty()) {
                flushCurrentBatch();
            }

            // 更新批次信息
            currentBatch.groups.push_back(group);
            currentTaskCount += groupTaskCount;

            currentBatch.chunkXStart = std::min(currentBatch.chunkXStart, group.startX);
            currentBatch.chunkZStart = std::min(currentBatch.chunkZStart, group.startZ);
            currentBatch.chunkXEnd   = std::max(currentBatch.chunkXEnd, group.startX + config.partitionSize - 1);
            currentBatch.chunkZEnd   = std::max(currentBatch.chunkZEnd, group.startZ + config.partitionSize - 1);
        }

        // 刷入最后一个批次
        flushCurrentBatch();
    }

} // namespace ChunkGroupAllocator