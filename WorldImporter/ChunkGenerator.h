// ChunkGenerator.h
#ifndef CHUNK_GENERATOR_H
#define CHUNK_GENERATOR_H

#include "model.h"
#include "block.h"

class ChunkGenerator {
public:
    static ModelData GenerateChunkModel(int chunkX, int sectionY, int chunkZ);
    static ModelData GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize);
};

#endif // CHUNK_GENERATOR_H