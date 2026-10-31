// ChunkGenerator.h
#ifndef CHUNK_GENERATOR_H
#define CHUNK_GENERATOR_H

#include "model.h"
#include "block.h"

class ChunkGenerator {
public:
    // Call once after freezing the palette, before starting model workers.
    static void PrepareBlockNames();
    static ModelData GenerateChunkModel(int chunkX, int sectionY, int chunkZ);
    static ModelData GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize);
private:
    static void ProcessBlockForModel(ModelData& chunkModel, int x, int y, int z,
        std::unordered_map<std::string, int>* materialLookup = nullptr);
};

#endif // CHUNK_GENERATOR_H