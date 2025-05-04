// RegionModelExporter.h
#ifndef REGION_MODEL_EXPORTER_H
#define REGION_MODEL_EXPORTER_H

#include "block.h"
#include "EntityBlock.h"
#include "blockstate.h"
#include "model.h"
#include <unordered_set>
#include "include/json.hpp"
extern std::unordered_map<std::tuple<int, int, int>, float, TupleHash> g_chunkLODs;

extern std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

extern std::unordered_map<std::string,std::unordered_map<std::string, std::vector<WeightedModelData>>> VariantModelCache; // variant随机模型缓存

extern std::unordered_map<std::string, std::unordered_map<std::string,std::vector<std::vector<WeightedModelData>>>> MultipartModelCache; // multipart部件缓存

extern std::unordered_map<std::pair<int, int>, std::vector<std::shared_ptr<EntityBlock>>, pair_hash> EntityBlockCache;

class RegionModelExporter {
public:
    // 导出指定区域内的所有方块模型
    static void ExportModels(const std::string& outputName = "region_model");
    
};

#endif // REGION_MODEL_EXPORTER_H