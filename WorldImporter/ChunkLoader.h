// ChunkLoader.h
#ifndef CHUNK_LOADER_H
#define CHUNK_LOADER_H

#include <tuple>
#include <atomic>
#include <unordered_map>
#include "block.h"
#include "LODManager.h"

extern std::unordered_map<std::tuple<int, int, int>, std::atomic<bool>, triple_hash> sectionLoadingStatus;

extern Config config;

class ChunkLoader {
public:
    static void LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd,
        int LOD0renderDistance, int LOD1renderDistance,
        int LOD2renderDistance, int LOD3renderDistance);
        
    static void LoadChunk(int chunkX, int chunkZ,
        int sectionYStart, int sectionYEnd,
        int LOD0renderDistance, int LOD1renderDistance,
        int LOD2renderDistance, int LOD3renderDistance);
};

#endif // CHUNK_LOADER_H