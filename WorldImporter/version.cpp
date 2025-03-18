#include "version.h"
#include "JarReader.h"
#include "fileutils.h"
#include "dat.h"
#include <fstream>
#include <iostream>
#include "include/json.hpp"

using json = nlohmann::json;
// 获取 Minecraft 版本 id
std::string GetMinecraftVersion(const std::wstring& gameFolderPath, std::string& modLoaderType) {
    // 获取整合包的文件夹名作为版本名称
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName);

    // 清空缓存（如果存在）
    if (VersionCache.find(folderNameStr) != VersionCache.end()) {
        VersionCache[folderNameStr].clear();
    }

    // 尝试读取 version.json
    std::wstring versionJsonPath = gameFolderPath + L"\\" + folderName + L".json";
    std::ifstream versionFile(versionJsonPath, std::ios::binary);
    versionFile.imbue(std::locale("en_US.UTF-8"));

    bool hasVersionJson = versionFile.is_open();
    json versionData;
    bool isForgePack = false, isFabricPack = false, isNeoForgePack = false;

    if (hasVersionJson) {
        try {
            versionFile >> versionData;
            versionFile.close();

            // 解析 mod 加载器类型
            if (versionData.contains("arguments") && versionData["arguments"].contains("game")) {
                for (const auto& arg : versionData["arguments"]["game"]) {
                    if (arg.is_string()) {
                        std::string argStr = arg.get<std::string>();
                        if (argStr == "forgeclient") isForgePack = true;
                        else if (argStr == "neoforgeclient") isNeoForgePack = true;
                    }
                }
            }

            if (versionData.contains("mainClass")) {
                std::string mainClass = versionData["mainClass"];
                if (mainClass == "net.fabricmc.loader.impl.launch.knot.KnotClient" ||
                    mainClass == "org.quiltmc.loader.impl.launch.knot.KnotClient") {
                    isFabricPack = true;
                }
                else if (mainClass == "net.minecraftforge.bootstrap.ForgeBootstrap") {
                    isForgePack = true;
                }
            }

            // 设置 modLoaderType
            if (isForgePack)       modLoaderType = "Forge";
            else if (isNeoForgePack) modLoaderType = "NeoForge";
            else if (isFabricPack)  modLoaderType = "Fabric";
            else                   modLoaderType = "Vanilla";

        }
        catch (...) {
            std::cerr << "Failed to parse version.json, using fallback" << std::endl;
            hasVersionJson = false;
        }
    }
    else {
        std::cerr << "version.json not found, using fallback" << std::endl;
        modLoaderType = "Vanilla";  // 默认值
    }


    std::wstring jarFilePath = gameFolderPath + L"\\" + folderName + L".jar";

    // 更新缓存
    FolderData versionInfo = { "minecraft", wstring_to_string(jarFilePath) };
    auto& folderDataList = VersionCache[folderNameStr];

    auto it = std::find_if(folderDataList.begin(), folderDataList.end(),
        [](const FolderData& data) { return data.namespaceName == "minecraft"; });

    if (it != folderDataList.end()) it->path = wstring_to_string(jarFilePath);
    else folderDataList.push_back(versionInfo);

    // 从 JAR 文件获取版本
    JarReader jarReader(jarFilePath);
    if (jarReader.getModType() == JarReader::ModType::Vanilla) {
        return jarReader.getVanillaVersionId();
    }

    std::cerr << "Could not determine Minecraft version from JAR" << std::endl;
    return "";
}


// 获取 mod 列表并更新 modListCache
void GetModList(const std::wstring& gameFolderPath, std::vector<std::string>& modList, const std::string& modLoaderType) {
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName); // 转换为 std::string

    // 获取 mods 文件夹路径
    std::wstring modsFolderPath = gameFolderPath + L"\\mods\\";

    // 获取 mods 文件夹中的所有文件，匹配 .jar 文件
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;
    std::wstring searchPath = modsFolderPath + L"*.jar";  // 搜索路径，查找所有 .jar 文件

    hFind = FindFirstFile(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    // 临时存储当前扫描到的 mod
    std::unordered_map<std::string, FolderData> newMods;
    // 遍历文件夹中的所有 .jar 文件
    do {
        if ((findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring fileName = findFileData.cFileName;

            // 确保文件名以 .jar 结尾
            if (fileName.find(L".jar") != std::wstring::npos) {
                std::string modId;
                try
                {
                    // 使用 JarReader 处理 .jar 文件
                    JarReader jarReader(modsFolderPath + fileName);

                    if (modLoaderType == "Fabric") {
                        modId = jarReader.getFabricModId();
                    }
                    else if (modLoaderType == "Forge") {
                        modId = jarReader.getForgeModId();
                    }
                    else if (modLoaderType == "NeoForge") {
                        modId = jarReader.getNeoForgeModId();
                    }
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Error occurred: " << e.what() << std::endl;
                }


                if (!modId.empty()) {
                    FolderData modInfo = { modId, wstring_to_string(modsFolderPath + fileName) };
                    newMods[modId] = modInfo;
                }
            }
        }
    } while (FindNextFile(hFind, &findFileData) != 0);



    FindClose(hFind);

    // 临时存储一个新的 modList
    std::unordered_map<std::string, FolderData> updatedModListMap;

    // 先复制原 modList 中已有的元素到 updatedModListMap 中
    for (const auto& mod : modList) {
        if (newMods.find(mod) != newMods.end()) {
            updatedModListMap[mod] = newMods[mod];
            newMods.erase(mod);  // 删除已经存在的 mod，避免重复添加
        }
    }

    // 然后将新的 mod 加入 updatedModListMap
    for (const auto& newMod : newMods) {
        updatedModListMap[newMod.first] = newMod.second;
    }

    // 更新 modListCache 和 modList
    std::vector<FolderData> finalModList;

    // 在 modList 中插入 "vanilla" 和 "resourcePack" ，如果它们尚未存在
    bool vanillaFound = false, resourcePackFound = false;

    // 将 modList 中已有的 mod 加入 finalModList
    for (const auto& modId : modList) {
        if (modId == "vanilla" && !vanillaFound) {
            finalModList.push_back({ "vanilla", wstring_to_string(modsFolderPath + L"vanilla.jar") });
            vanillaFound = true;
        }
        if (modId == "resourcePack" && !resourcePackFound) {
            finalModList.push_back({ "resourcePack", wstring_to_string(modsFolderPath + L"resourcePack.jar") });
            resourcePackFound = true;
        }

        // 如果原 modList 中的 mod 在新的扫描列表中找不到，删除它
        if (updatedModListMap.find(modId) != updatedModListMap.end()) {
            finalModList.push_back(updatedModListMap[modId]);
            updatedModListMap.erase(modId);  // 已添加的 mod 从 map 中移除
        }
    }

    // 如果 modList 中没有 "vanilla" 和 "resourcePack"，添加它们到最后
    if (!vanillaFound) {
        finalModList.push_back({ "vanilla", wstring_to_string(modsFolderPath + L"vanilla.jar") });
    }
    if (!resourcePackFound) {
        finalModList.push_back({ "resourcePack", wstring_to_string(modsFolderPath + L"resourcePack.jar") });
    }

    // 把新的 mod 加入 finalModList（这些是从 newMods 中没有被提前加入的）
    for (const auto& newMod : updatedModListMap) {
        finalModList.push_back(newMod.second);
    }

    // 更新 modListCache 和 modList
    modListCache[folderNameStr] = finalModList;
    // 更新 modList（与 modListCache 保持一致的顺序）
    modList.clear();
    for (const auto& modData : finalModList) {
        modList.push_back(modData.namespaceName);
    }
}

// 获取资源包列表，并支持手动调整顺序
void GetResourcePacks(const std::wstring& gameFolderPath, std::vector<std::string>& resourcePacks) {
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName); // 转换为 std::string

    // 获取资源包文件夹路径
    std::wstring resourcePacksFolderPath = gameFolderPath + L"\\resourcepacks\\";  // 资源包文件夹路径

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;

    // 获取资源包文件夹中的所有文件
    std::wstring searchPath = resourcePacksFolderPath + L"*";  // 查找文件夹下所有文件

    hFind = FindFirstFile(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    // 临时存储当前扫描到的资源包
    std::unordered_map<std::string, FolderData> newResourcePacks;

    // 遍历文件夹中的所有文件
    do {
        if ((findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring fileName = findFileData.cFileName;
            std::string resourcePackName = wstring_to_string(fileName);
            newResourcePacks[resourcePackName] = { resourcePackName, wstring_to_string(resourcePacksFolderPath + fileName) };
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);

    // 临时存储更新后的资源包列表
    std::unordered_map<std::string, FolderData> updatedResourcePackMap;

    // 先复制原 resourcePacks 中已有的元素到 updatedResourcePackMap 中
    for (const auto& resourcePackName : resourcePacks) {
        if (newResourcePacks.find(resourcePackName) != newResourcePacks.end()) {
            updatedResourcePackMap[resourcePackName] = newResourcePacks[resourcePackName];
            newResourcePacks.erase(resourcePackName);  // 删除已经存在的资源包，避免重复添加
        }
    }

    // 然后将新的资源包加入 updatedResourcePackMap
    for (const auto& newResourcePack : newResourcePacks) {
        updatedResourcePackMap[newResourcePack.first] = newResourcePack.second;
    }

    // 更新 resourcePacksCache 和 resourcePacks
    std::vector<FolderData> finalResourcePackList;

    // 将 resourcePacks 中已有的资源包加入 finalResourcePackList
    for (const auto& resourcePackName : resourcePacks) {
        if (updatedResourcePackMap.find(resourcePackName) != updatedResourcePackMap.end()) {
            finalResourcePackList.push_back(updatedResourcePackMap[resourcePackName]);
            updatedResourcePackMap.erase(resourcePackName);  // 已添加的资源包从 map 中移除
        }
    }

    // 把新的资源包加入 finalResourcePackList（这些是从 newResourcePacks 中没有被提前加入的）
    for (const auto& newResourcePack : updatedResourcePackMap) {
        finalResourcePackList.push_back(newResourcePack.second);
    }

    // 更新 resourcePacksCache
    resourcePacksCache[folderNameStr] = finalResourcePackList;

    // 更新 resourcePacks（与 resourcePacksCache 保持一致的顺序）
    resourcePacks.clear();
    for (const auto& resourcePackData : finalResourcePackList) {
        resourcePacks.push_back(resourcePackData.namespaceName);
    }
}


// 获取存档文件列表并读取LevelName
void GetSaveFiles(const std::wstring& gameFolderPath, std::vector<std::string>& saveFiles) {

    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName); // 转换为 std::string

    // 在遍历之前检查是否已经存在该 folderName
    if (saveFilesCache.find(folderNameStr) != saveFilesCache.end()) {
        // 如果存在，清空缓存
        saveFilesCache[folderNameStr].clear();
    }

    std::wstring savesFolderPath = gameFolderPath + L"\\saves\\";  // 存档文件夹路径

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;

    // 获取存档文件夹中的所有文件
    std::wstring searchPath = savesFolderPath + L"*";  // 查找文件夹下所有文件

    hFind = FindFirstFile(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        std::wcerr << L"无法打开存档文件夹: " << savesFolderPath << std::endl;
        return;
    }

    // 遍历文件夹中的所有文件
    do {
        // 如果是文件夹而不是文件
        if ((findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            // 排除 "." 和 ".."
            if (findFileData.cFileName[0] != L'.') {
                std::wstring folderName = findFileData.cFileName;
                std::wstring levelDatPath = savesFolderPath + folderName + L"\\level.dat";  // level.dat 路径

                // 检查是否存在 level.dat 文件
                if (GetFileAttributes(levelDatPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    std::string filePath = wstring_to_string(levelDatPath);  // 转换为 std::string
                    NbtTagPtr rootTag = DatFileReader::readDatFile(filePath);

                    // 获取 "Data" 子标签
                    NbtTagPtr dataTag = getChildByName(rootTag, "Data");

                    // 获取 "LevelName" 子标签并输出
                    NbtTagPtr levelNameTag = getChildByName(dataTag, "LevelName");
                    std::string levelName = getStringTag(levelNameTag);

                    // 将存档文件名添加到列表
                    saveFiles.push_back(levelName);

                    // 缓存存档文件信息到全局 saveFilesCache
                    FolderData saveInfo = { levelName, wstring_to_string(savesFolderPath + folderName) };
                    saveFilesCache[folderNameStr].push_back(saveInfo);
                }
            }
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
}


// 输出 modListCache 的方法
void PrintModListCache() {
    for (const auto& modEntry : modListCache) { // 遍历所有 mod 条目
        // 输出当前键值对的键（mod名称）
        std::cout << "[" << modEntry.first << "]\n";

        // 遍历该键对应的 vector<FolderData>
        for (const auto& folderData : modEntry.second) {
            std::cout << "  Namespace: " << folderData.namespaceName
                << " | Path: " << folderData.path << "\n";
        }

        std::cout << std::endl; // 不同键之间用空行分隔
    }
}