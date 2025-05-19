// ChunkGroupAllocator.h
#pragma once

#include <vector>
#include "config.h" 

struct ChunkTask {
    int chunkX;
    int sectionY;
    int chunkZ;
    float lodLevel; 
};

struct ChunkGroup {
    int startX;
    int startZ;
    std::vector<ChunkTask> tasks;
};

namespace ChunkGroupAllocator {

    std::vector<ChunkGroup> GenerateChunkGroups(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd
    );

}