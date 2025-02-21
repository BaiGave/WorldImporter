#ifndef BLOCKSTATE_H
#define BLOCKSTATE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "version.h"
#include "model.h"
#include "block.h"
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"
#include <future>
#include <mutex>

// 全局缓存，键为 namespace，值为 blockId 到 ModelData 的映射
extern  std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when);

std::vector<std::string> SplitString(const std::string& input, char delimiter);

std::string SortedVariantKey(const std::string& key);

// --------------------------------------------------------------------------------
// 核心函数声明
// --------------------------------------------------------------------------------
std::unordered_map<std::string, ModelData> ProcessBlockstateJson(
    const std::string& namespaceName,
    const std::vector<std::string>& blockIds
);


void ProcessBlockstateForBlocks(const std::vector<Block>& blocks);
// 获取方块状态 JSON 文件内容
nlohmann::json GetBlockstateJson(
    const std::string& namespaceName,
    const std::string& blockId
);

// 处理所有方块状态变种并合并模型
void ProcessAllBlockstateVariants();

#endif // BLOCKSTATE_H