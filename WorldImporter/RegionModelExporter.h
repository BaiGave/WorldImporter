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

class RegionModelExporter {
public:
    // 导出指定区域内的所有方块模型
    static void ExportRegionModels(const std::string& outputName = "region_model");

    static bool IsRegionValid(int x, int y, int z, float lodSize);

    static bool IsRegionEmpty(int x, int y, int z, float lodSize, bool isFluid = false);
    

    static float GetChunkLODAtBlock(int x, int y, int z);

    static ModelData GenerateChunkModel(int chunkX, int sectionY, int chunkZ);

    static ModelData GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize);

private:
    // 获取区域内所有唯一的方块ID（带状态）
    static void LoadChunks();
    // 应用顶点偏移
    static void ApplyPositionOffset(ModelData& model, int x, int y, int z);
};

#endif // REGION_MODEL_EXPORTER_H