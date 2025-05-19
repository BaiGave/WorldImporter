// ChunkLoader.h
#ifndef CHUNK_LOADER_H
#define CHUNK_LOADER_H

#include <tuple>
#include <atomic>
#include <unordered_map>
#include "block.h"
#include "LODManager.h"



extern Config config;

class ChunkLoader {
public:
    static void LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd);
    static void CalculateChunkLODs(int expandedChunkXStart, int expandedChunkXEnd, int expandedChunkZStart, int expandedChunkZEnd,
        int sectionYStart, int sectionYEnd);
        
};

#endif // CHUNK_LOADER_H