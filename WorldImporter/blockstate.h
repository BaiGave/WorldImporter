#ifndef BLOCKSTATE_H
#define BLOCKSTATE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>  // 用于解析 JSON
#include "version.h"
#include "model.h"
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"
#include <future>
#include <mutex>


// 全局缓存，键为 namespace，值为 blockId 到 ModelData 的映射
static std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;

std::unordered_map<std::string, ModelData> ProcessBlockstateJson(const std::string& namespaceName, const std::vector<std::string>& blockIds);


nlohmann::json GetBlockstateJson(const std::string& namespaceName, const std::string& blockId);

void ProcessAllBlockstateVariants();

void PrintModelCache(const std::unordered_map<std::string, std::unordered_map<std::string, ModelData>>& cache);
#endif // BLOCKSTATE_H
