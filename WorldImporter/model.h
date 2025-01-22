#ifndef MODEL_H
#define MODEL_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cmath>
#include <nlohmann/json.hpp>  // 用于解析 JSON
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "version.h"
#pragma once

#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif
// 外部声明
extern std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
extern std::unordered_map<std::string, std::vector<FolderData>> modListCache;
extern std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
extern std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;

extern std::string currentSelectedGameVersion;

extern Config config;

// 函数声明
nlohmann::json GetModelJson(const std::string& namespaceName, const std::string& blockId);
nlohmann::json ProcessModelJson(const std::string& namespaceName, const std::string& blockId);
nlohmann::json LoadParentModel(const std::string& namespaceName, const std::string& blockId, nlohmann::json& currentModelJson);
nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson, const nlohmann::json& currentModelJson);

#endif // MODEL_H
