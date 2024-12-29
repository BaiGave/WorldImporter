#ifndef VERSION_H
#define VERSION_H

#include <string>
#include <vector>
#include "nbtutils.h"

// 用于存储文件夹相关数据的结构体
struct FolderData {
    std::string namespaceName;
    std::string path;
};

// 为每个功能单独定义一个缓存
extern std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
extern std::unordered_map<std::string, std::vector<FolderData>> modListCache;
extern std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
extern std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;


std::string GetMinecraftVersion(const std::wstring& gameFolderPath, std::string& modLoaderType);
void GetModList(const std::wstring& gameFolderPath, std::vector<std::string>& modList, const std::string& modLoaderType);
void GetResourcePacks(const std::wstring& gameFolderPath, std::vector<std::string>& resourcePacks);
void GetSaveFiles(const std::wstring& gameFolderPath, std::vector<std::string>& saveFiles);
#endif // VERSION_H
