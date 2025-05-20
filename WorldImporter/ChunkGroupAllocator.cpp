// chunk_group_allocator.cpp
#include "ChunkGroupAllocator.h"
#include "LODManager.h" // 包含LODManager.h以访问g_chunkLODs
#include <iostream> // 用于潜在的调试输出


namespace ChunkGroupAllocator {

    std::vector<ChunkGroup> g_chunkGroups; // 定义全局变量

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
                            auto it = g_chunkSectionInfoMap.find(key);
                            if (it != g_chunkSectionInfoMap.end()) {
                                task.lodLevel = it->second.lodLevel;
                            } else {
                                // 如果在g_chunkSectionInfoMap中找不到 (理论上不应该发生，因为RegionModelExporter会预先计算LOD)
                                // 作为回退，可以设置一个默认值或根据距离计算
                                task.lodLevel = 0.0f; // 默认LOD
                                // std::cerr << "Warning: ChunkSectionInfo not found for chunk (" << chunkX << ", " << sectionY << ", " << chunkZ << ") in g_chunkSectionInfoMap. Defaulting LOD to 0.0f." << std::endl;
                            }

                            newGroup.tasks.push_back(task);
                        }
                    }
                }

                g_chunkGroups.push_back(newGroup);
            }
        }
    }

} // namespace ChunkGroupAllocator