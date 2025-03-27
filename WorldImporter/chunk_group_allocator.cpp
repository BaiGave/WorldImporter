// chunk_group_allocator.cpp
#include "chunk_group_allocator.h"
#include <cmath>

namespace ChunkGroupAllocator {

    std::vector<ChunkGroup> GenerateChunkGroups(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd,
        int LOD0distance, int LOD1distance,
        int LOD2distance, int LOD3distance)
    {
        std::vector<ChunkGroup> chunkGroups;
        int partitionSize = config.partitionSize;
        for (int groupX = chunkXStart; groupX <= chunkXEnd; groupX += partitionSize) {
            int currentGroupXEnd = groupX + partitionSize - 1;
            if (currentGroupXEnd > chunkXEnd) currentGroupXEnd = chunkXEnd;

            for (int groupZ = chunkZStart; groupZ <= chunkZEnd; groupZ += partitionSize) {
                int currentGroupZEnd = groupZ + partitionSize - 1;
                if (currentGroupZEnd > chunkZEnd) currentGroupZEnd = chunkZEnd;

                ChunkGroup newGroup;
                newGroup.startX = groupX;
                newGroup.startZ = groupZ;

                for (int chunkX = groupX; chunkX <= currentGroupXEnd; ++chunkX) {
                    for (int chunkZ = groupZ; chunkZ <= currentGroupZEnd; ++chunkZ) {
                        for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                            const int distance = static_cast<int>(std::sqrt(
                                (chunkX - config.LODCenterX) * (chunkX - config.LODCenterX) +
                                (chunkZ - config.LODCenterZ) * (chunkZ - config.LODCenterZ)
                            ));

                            ChunkTask task;
                            task.chunkX = chunkX;
                            task.chunkZ = chunkZ;
                            task.sectionY = sectionY;

                            if (config.activeLOD)
                            {
                                if (distance <= LOD0distance) {
                                    task.lodLevel = 0.0f;
                                }
                                else if (distance <= LOD1distance) {
                                    task.lodLevel = 1.0f;
                                }
                                else if (distance <= LOD2distance) {
                                    task.lodLevel = 2.0f;
                                }
                                else if (distance <= LOD3distance) {
                                    task.lodLevel = 4.0f;
                                }
                                else {
                                    task.lodLevel = 8.0f;
                                }
                            }
                            else
                            {
                                task.lodLevel = 0.0f;
                            }
                            

                            newGroup.tasks.push_back(task);
                        }
                    }
                }

                chunkGroups.push_back(newGroup);
            }
        }

        return chunkGroups;
    }

} // namespace ChunkGroupAllocator