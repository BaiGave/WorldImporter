#ifndef GLOBAL_H
#define GLOBAL_H

#include <unordered_map>
#include <vector>
#include <string>
#include "config.h"
#include "version.h"

// 这些全局变量会在程序中被多个文件共享
extern std::string currentSelectedGameVersion;
extern std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
extern std::unordered_map<std::string, std::vector<FolderData>> modListCache;
extern std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
extern std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;

#endif  // GLOBAL_H
