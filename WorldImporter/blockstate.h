// blockstate.h
#ifndef BLOCKSTATE_H
#define BLOCKSTATE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include "include/json.hpp"
#include "model.h"
#include "block.h"
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"
#include <future>
#include <mutex>

struct WeightedModelData {
    ModelData model;
    int weight;
};

// 全局缓存,键为 namespace,值为 blockId 到 ModelData 的映射
extern  std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<WeightedModelData>>> VariantModelCache; // variant随机模型缓存

extern std::unordered_map<std::string,
    std::unordered_map<std::string,
    std::vector<std::vector<WeightedModelData>>>> MultipartModelCache; // multipart部件缓存

bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when);

std::string SortedVariantKey(const std::string& key);

// --------------------------------------------------------------------------------
// 核心函数声明
// --------------------------------------------------------------------------------
void ProcessBlockstate(const std::string& namespaceName,const std::vector<std::string>& blockIds);

void ProcessBlockstateForBlocks(const std::vector<Block>& blocks);

// 获取方块状态 JSON 文件内容
nlohmann::json GetBlockstateJson(const std::string& namespaceName,const std::string& blockId);

ModelData GetRandomModelFromCache(const std::string& namespaceName, const std::string& blockId);



#endif // BLOCKSTATE_H