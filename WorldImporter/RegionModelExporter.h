// RegionModelExporter.h
#ifndef REGION_MODEL_EXPORTER_H
#define REGION_MODEL_EXPORTER_H

#include "block.h"
#include "EntityBlock.h"
#include "blockstate.h"
#include "model.h"
#include <unordered_set>
#include "include/json.hpp"
extern std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;


extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<WeightedModelData>>> VariantModelCache; // variant随机模型缓存

extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<std::vector<WeightedModelData>>>> MultipartModelCache; // multipart部件缓存

extern std::unordered_map<std::pair<int, int>, std::vector<std::shared_ptr<EntityBlock>>, pair_hash> entityBlockCache;
enum BlockType {
    AIR,
    FLUID,
    SOLID
};
class RegionModelExporter {
public:
    // 导出指定区域内的所有方块模型
    static void ExportModels(const std::string& outputName = "region_model");
    
    static ModelData GenerateChunkModel(int chunkX, int sectionY, int chunkZ);

    static ModelData GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize);

private:
    // 获取区域内所有唯一的方块ID(带状态)
    static void LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd,
        int LOD0renderDistance, int LOD1renderDistance,
        int LOD2renderDistance, int LOD3renderDistance);


};

#endif // REGION_MODEL_EXPORTER_H