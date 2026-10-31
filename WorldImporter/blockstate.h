// blockstate.h
#ifndef BLOCKSTATE_H
#define BLOCKSTATE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <optional>
#include "include/json.hpp"
#include "blockposseed.h"
#include "model.h"
#include "block.h"
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"
#include <future>
#include <mutex>
#include <atomic>

struct WeightedModelData {
    ModelData model;
    int weight;
};

// multipart 的一个分组。weighted 记录原版里该分组的 apply 是"数组"还是"对象":
// 数组 → WeightedVariants，取模型时会消耗一次随机数；对象 → SingleVariant，不消耗。
// 复刻原版的抽取序列时必须区分这两者，否则每组会多抽/少抽，后续分组全部错位。
struct MultipartGroup {
    std::vector<WeightedModelData> models;
    bool weighted = false;
};

// 1.18.2..1.21.4 的 WeightedRandom.getWeightedItem。选不中返回空模型（没有面）。
inline ModelData TakeWeightedModel(const std::vector<WeightedModelData>& models, std::int32_t index) {
    const int slot = SelectWeightedSlot(index, static_cast<int>(models.size()),
        [&](int i) { return models[static_cast<size_t>(i)].weight; });
    if (slot < 0) return ModelData();
    return models[static_cast<size_t>(slot)].model;
}

// 全局缓存,键为 namespace,值为 blockId 到 ModelData 的映射
extern  std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<WeightedModelData>>> VariantModelCache; // variant随机模型缓存

extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<MultipartGroup>>> MultipartModelCache; // multipart部件缓存
extern std::atomic<bool> blockstateCachesFrozen;

bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when);

std::string SortedVariantKey(const std::string& key);

// --------------------------------------------------------------------------------
// 核心函数声明
// --------------------------------------------------------------------------------
void ProcessBlockstate(const std::string& namespaceName,const std::vector<std::string>& blockIds);

void ProcessBlockstateForBlocks(const std::vector<Block>& blocks);

// 获取方块状态 JSON 文件内容
nlohmann::json GetBlockstateJson(const std::string& namespaceName,const std::string& blockId);

// 方块坐标的渲染种子见 blockposseed.h（BlockPosSeed / LegacyRandom）。

// posSeed 为 nullopt 表示调用点拿不到方块坐标（实体方块 / create / LOD 等路径）。
// 此时 Game 模式退回"取第一个变体"，保证结果依然可复现。
// Game 且有坐标时，算法看 g_activeChunkDataVersion（< 4325 为旧挑选）。
ModelData GetRandomModelFromCache(const std::string& namespaceName, const std::string& blockId,
                                  std::optional<long long> posSeed = std::nullopt);

// 获取该 block state 的全部模型（普通模型=1 个；加权 variant=全部变体；
// multipart=按第 0 组选中的合并结果）。供运行时遮挡表做保守判定使用。
std::vector<ModelData> GetAllModelsFromCache(const std::string& namespaceName, const std::string& blockId);



#endif // BLOCKSTATE_H