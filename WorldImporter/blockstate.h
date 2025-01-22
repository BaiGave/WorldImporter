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

// 外部声明
extern std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
extern std::unordered_map<std::string, std::vector<FolderData>> modListCache;
extern std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
extern std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;

extern std::string currentSelectedGameVersion;

extern Config config;

// 函数声明
nlohmann::json GetBlockstateJson(const std::string& namespaceName, const std::string& blockId);
nlohmann::json ProcessBlockstateJson(const std::string& namespaceName, const std::string& blockId);

#endif // BLOCKSTATE_H
