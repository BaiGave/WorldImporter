// chunk_group_allocator.cpp
#include "ChunkGroupAllocator.h"
#include "LODManager.h" // 包含LODManager.h以访问g_chunkLODs
#include <iostream> // 用于潜在的调试输出
#include <limits> // 新增:用于 numeric_limits
#include <algorithm>
#include <shared_mutex>
#include <utility>
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
                // Batch 可能包含数万个任务，必须移动而不是完整复制。
                g_chunkBatches.push_back(std::move(currentBatch));
                // 重新初始化
                currentBatch = ChunkBatch{};
                currentBatch.chunkXStart = std::numeric_limits<int>::max();
                currentBatch.chunkZStart = std::numeric_limits<int>::max();
                currentBatch.chunkXEnd   = std::numeric_limits<int>::min();
                currentBatch.chunkZEnd   = std::numeric_limits<int>::min();
                currentTaskCount = 0;
            }
        };

        for (auto& group : g_chunkGroups) {
            const size_t groupTaskCount = group.tasks.size();
            const int groupStartX = group.startX;
            const int groupStartZ = group.startZ;

            // 如果当前批次任务数超出限制,则先刷入当前批次
            if (currentTaskCount + groupTaskCount > maxTasksPerBatch && !currentBatch.groups.empty()) {
                flushCurrentBatch();
            }

            currentTaskCount += groupTaskCount;
            currentBatch.chunkXStart = std::min(currentBatch.chunkXStart, groupStartX);
            currentBatch.chunkZStart = std::min(currentBatch.chunkZStart, groupStartZ);
            // 最后一组可能不足 partitionSize，不能把批次边界扩到选择区域外。
            currentBatch.chunkXEnd   = std::max(currentBatch.chunkXEnd,
                std::min(chunkXEnd, groupStartX + config.partitionSize - 1));
            currentBatch.chunkZEnd   = std::max(currentBatch.chunkZEnd,
                std::min(chunkZEnd, groupStartZ + config.partitionSize - 1));

            // g_chunkGroups 在批次生成后只保留数量统计，任务向量直接搬入 Batch。
            currentBatch.groups.push_back(std::move(group));
        }

        // 刷入最后一个批次
        flushCurrentBatch();
    }

} // namespace ChunkGroupAllocator