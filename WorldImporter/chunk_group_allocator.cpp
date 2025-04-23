// chunk_group_allocator.cpp
#include "chunk_group_allocator.h"

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
        int groupsX = ((chunkXEnd - chunkXStart) / partitionSize) + 1;
        int groupsZ = ((chunkZEnd - chunkZStart) / partitionSize) + 1;
        chunkGroups.reserve(groupsX * groupsZ);
        int L0d2 = LOD0distance * LOD0distance;
        int L1d2 = LOD1distance * LOD1distance;
        int L2d2 = LOD2distance * LOD2distance;
        int L3d2 = LOD3distance * LOD3distance;
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
                            int dx = chunkX - config.LODCenterX;
                            int dz = chunkZ - config.LODCenterZ;
                            int dist2 = dx * dx + dz * dz;

                            ChunkTask task;
                            task.chunkX = chunkX;
                            task.chunkZ = chunkZ;
                            task.sectionY = sectionY;

                            if (config.activeLOD)
                            {
                                if (dist2 <= L0d2) {
                                    task.lodLevel = 0.0f;
                                }
                                else if (dist2 <= L1d2) {
                                    task.lodLevel = 1.0f;
                                }
                                else if (dist2 <= L2d2) {
                                    task.lodLevel = 2.0f;
                                }
                                else if (dist2 <= L3d2) {
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