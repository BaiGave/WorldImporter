#include "version.h"
#include "JarReader.h"
#include "fileutils.h"
#include "dat.h"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// 获取 Minecraft 版本 id
std::string GetMinecraftVersion(const std::wstring& gameFolderPath, std::string& modLoaderType) {
    // 获取整合包的文件夹名作为版本名称
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);  // 这里假设文件夹名就是版本名
    std::string folderNameStr = wstring_to_string(folderName);  // 转换为 std::string

    // 在遍历之前检查是否已经存在该 folderName 对应的缓存
    if (VersionCache.find(folderNameStr) != VersionCache.end()) {
        // 如果存在，清空缓存
        VersionCache[folderNameStr].clear();
    }

    // 构建 version.json 文件的路径
    std::wstring versionJsonPath = gameFolderPath + L"\\" + folderName + L".json";

    // 设置 locale，支持 UTF-8 编码读取文件
    std::ifstream versionFile(versionJsonPath, std::ios::binary);  // 使用 ifstream 读取文件
    versionFile.imbue(std::locale("en_US.UTF-8"));  // 使用 UTF-8 编码读取文件

    if (!versionFile.is_open()) {
        std::cerr << "Could not open the version.json file: " << wstring_to_string(versionJsonPath) << std::endl;
        return "";
    }

    // 读取文件内容并解析为 JSON 对象
    json versionData;
    versionFile >> versionData;  // 使用 nlohmann::json 解析

    versionFile.close();  // 关闭文件

    // 查找是否存在 "forgeclient" 参数在 "game" 数组中
    bool isForgePack = false;
    bool isFabricPack = false;
    bool isNeoForgePack = false;
    if (versionData.contains("arguments") && versionData["arguments"].contains("game")) {
        const auto& gameArgs = versionData["arguments"]["game"];
        if (gameArgs.is_array()) {
            // 遍历 game 数组，检查是否包含 "forgeclient"
            for (const auto& arg : gameArgs) {
                if (arg.is_string() && arg.get<std::string>() == "forgeclient") {
                    isForgePack = true;
                    break;
                }
            }
        }
    }
    if (versionData.contains("arguments") && versionData["arguments"].contains("game")) {
        const auto& gameArgs = versionData["arguments"]["game"];
        if (gameArgs.is_array()) {
            // 遍历 game 数组，检查是否包含 "neoforgeclient"
            for (const auto& arg : gameArgs) {
                if (arg.is_string() && arg.get<std::string>() == "neoforgeclient") {
                    isNeoForgePack = true;
                    break;
                }
            }
        }
    }

    if (versionData.contains("mainClass")) {
        std::string mainClass = versionData["mainClass"];

        if (mainClass == "net.fabricmc.loader.impl.launch.knot.KnotClient") {
            isFabricPack = true;
        }
        else if (mainClass == "net.minecraftforge.bootstrap.ForgeBootstrap")
        {
            isForgePack = true;
        }
        else if (mainClass == "org.quiltmc.loader.impl.launch.knot.KnotClient")
        {
            isFabricPack = true;
        }
    }

    // 设置 modLoaderType 根据是否是 Forge
    if (isForgePack) {
        modLoaderType = "Forge";
    }
    else if (isNeoForgePack)
    {
        modLoaderType = "NeoForge";
    }
    else if (isFabricPack)
    {
        modLoaderType = "Fabric";
    }
    else {
        modLoaderType = "Vanilla";
    }

    // 获取 jar 字段
    if (versionData.contains("jar")) {
        std::string jar = versionData["jar"];
        // 将 std::string 转换为 std::wstring
        std::wstring w_jar = string_to_wstring(jar);

        // 使用获取的版本名构建 .jar 文件路径
        std::wstring jarFilePath = gameFolderPath + L"\\" + w_jar + L".jar";

        // 缓存版本信息到全局 VersionCache
        FolderData versionInfo = { "minecraft", wstring_to_string(jarFilePath) };

        // 获取 folderName 对应的键
        auto& folderDataList = VersionCache[folderNameStr];

        // 检查是否已经有名为 "minecraft" 的 FolderData
        bool found = false;
        for (auto& data : folderDataList) {
            if (data.namespaceName == "minecraft") {
                // 如果找到，更新 path
                data.path = wstring_to_string(jarFilePath);
                found = true;
                break;
            }
        }

        // 如果没有找到 "minecraft"，则插入新的 FolderData
        if (!found) {
            folderDataList.push_back(versionInfo);
        }

        // 使用 JarReader 处理 .jar 文件
        JarReader jarReader(jarFilePath);

        // 根据不同的 mod 类型，获取 modId
        if (jarReader.getModType() == JarReader::ModType::Vanilla) {
            return jarReader.getVanillaVersionId();
        }

        return "";  // 返回版本 id
    }

    std::cerr << "Could not find Minecraft version (id) in version.json" << std::endl;
    return "";
}


// 获取 mod 列表（.jar 文件中的 modId） 
void GetModList(const std::wstring& gameFolderPath, std::vector<std::string>& modList, const std::string& modLoaderType) {
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName); // 转换为 std::string

    // 获取 mods 文件夹路径
    std::wstring modsFolderPath = gameFolderPath + L"\\mods\\";

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;

    // 获取 mods 文件夹中的所有文件，匹配 .jar 文件
    std::wstring searchPath = modsFolderPath + L"*.jar";  // 搜索路径，查找所有 .jar 文件

    hFind = FindFirstFile(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    // 用于存储新扫描到的 mod
    std::unordered_map<std::string, FolderData> newMods;

    // 遍历文件夹中的所有 .jar 文件
    do {
        if ((findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring fileName = findFileData.cFileName;

            // 确保文件名以 .jar 结尾
            if (fileName.find(L".jar") != std::wstring::npos) {
                // 使用 JarReader 处理 .jar 文件
                JarReader jarReader(modsFolderPath + fileName);

                std::string modId;

                if (modLoaderType == "Fabric") {
                    modId = jarReader.getFabricModId();
                }
                else if (modLoaderType == "Forge") {
                    modId = jarReader.getForgeModId();
                }
                else if (modLoaderType == "NeoForge") {
                    modId = jarReader.getNeoForgeModId();
                }

                if (!modId.empty()) {
                    FolderData modInfo = { modId, wstring_to_string(modsFolderPath + fileName) };
                    newMods[modId] = modInfo;
                }
            }
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);

    // 比较新 mod 列表与缓存中的旧 mod 列表
    if (modListCache.find(folderNameStr) != modListCache.end()) {
        // 存在缓存时，比较新增和消失的 mod
        std::unordered_map<std::string, FolderData> currentMods;

        // 将缓存中的 mod 存入 currentMods
        for (const auto& modData : modListCache[folderNameStr]) {
            currentMods[modData.namespaceName] = modData;
        }

        // 查找消失的 mod
        for (const auto& modData : modListCache[folderNameStr]) {
            if (newMods.find(modData.namespaceName) == newMods.end()) {
                std::cout << "Mod disappeared: " << modData.namespaceName << std::endl;
            }
        }

        // 查找新增的 mod，并按顺序添加
        for (const auto& newMod : newMods) {
            if (currentMods.find(newMod.first) == currentMods.end()) {
                std::cout << "New mod found: " << newMod.first << std::endl;
            }
        }
    }

    // 更新 modListCache，并将新 mod 按顺序加入 modList
    std::vector<FolderData> sortedMods;
    for (const auto& newMod : newMods) {
        sortedMods.push_back(newMod.second);
    }

    // 按顺序排序，越前面优先级越高
    std::sort(sortedMods.begin(), sortedMods.end(), [](const FolderData& a, const FolderData& b) {
        return a.namespaceName < b.namespaceName;  // 可以根据其他条件排序
        });

    // 更新 modListCache
    modListCache[folderNameStr] = sortedMods;

    // 更新最终的 modList
    modList.clear();
    for (const auto& sortedMod : sortedMods) {
        modList.push_back(sortedMod.namespaceName);
    }
}

// 获取资源包列表
void GetResourcePacks(const std::wstring& gameFolderPath, std::vector<std::string>& resourcePacks) {
    std::wstring folderName = GetFolderNameFromPath(gameFolderPath);
    std::string folderNameStr = wstring_to_string(folderName); // 转换为 std::string

    // 在遍历之前检查是否已经存在该 folderName
    if (resourcePacksCache.find(folderNameStr) != resourcePacksCache.end()) {
        // 如果存在，清空缓存
        resourcePacksCache[folderNameStr].clear();
    }

    std::wstring resourcePacksFolderPath = gameFolderPath + L"\\resourcepacks\\";  // 资源包文件夹路径

    WIN32_FIND_DATA findFileData;
    HANDLE hFind = INVALID_HANDLE_VALUE;

    // 获取资源包文件夹中的所有文件
    std::wstring searchPath = resourcePacksFolderPath + L"*";  // 查找文件夹下所有文件

    hFind = FindFirstFile(searchPath.c_str(), &findFileData);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    // 遍历文件夹中的所有文件
    do {
        // 如果是文件而不是目录
        if ((findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            std::wstring fileName = findFileData.cFileName;
            resourcePacks.push_back(wstring_to_string(fileName));  // 将文件名添加到资源包列表

            // 缓存资源包信息到全局 resourcePacksCache
            FolderData resourcePackInfo = { wstring_to_string(fileName), wstring_to_string(resourcePacksFolderPath + fileName) };
            resourcePacksCache[folderNameStr].push_back(resourcePackInfo);
        }
    } while (FindNextFile(hFind, &findFileData) != 0);

    FindClose(hFind);
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
