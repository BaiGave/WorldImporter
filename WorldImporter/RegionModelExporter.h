// RegionModelExporter.h
#ifndef REGION_MODEL_EXPORTER_H
#define REGION_MODEL_EXPORTER_H

#include "block.h"
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

enum BlockType {
    AIR,
    FLUID,
    SOLID
};
class RegionModelExporter {
public:
    // 导出指定区域内的所有方块模型
    static void ExportRegionModels(const std::string& outputName = "region_model");
    
    static ModelData GenerateChunkModel(int chunkX, int sectionY, int chunkZ);

    static ModelData GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize);

    static void  ApplyPositionOffset(ModelData& model, int x, int y, int z);

private:
    // 获取区域内所有唯一的方块ID（带状态）
    static void LoadChunks();


};

#endif // REGION_MODEL_EXPORTER_H