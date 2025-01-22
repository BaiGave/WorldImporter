#ifndef TEXTURE_H
#define TEXTURE_H

#include <unordered_map>
#include <vector>
#include <string>
#include "config.h"
#include "version.h"
#include "JarReader.h"

// 外部声明
extern std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
extern std::unordered_map<std::string, std::vector<FolderData>> modListCache;
extern std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
extern std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;

extern std::string currentSelectedGameVersion;

extern Config config;

// 函数声明
std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId);
bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath);

#endif // TEXTURE_H
