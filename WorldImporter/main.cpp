#include <iostream>
#include <chrono>
#include <thread>
#include "config.h"
#include "biome_mapping.h"
#include "block.h"
#include "PointCloudExporter.h"
#include <fstream> 
#include <locale> 
#include <codecvt>
#include "global.h"
#include "JarReader.h"
#include "version.h"
#include "fileutils.h"
Config config;  // 定义全局变量

using namespace std;
using namespace chrono;

std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
std::unordered_map<std::string, std::vector<FolderData>> modListCache;
std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
std::unordered_map<std::string, std::vector<FolderData>> saveFilesCache;

// 新增：定义当前整合包的版本全局变量
std::string currentSelectedGameVersion;

void loadAndUpdateConfig() {
    // 加载配置
    config = LoadConfig("config\\config.json");

    // 遍历 versionConfigs 中的所有内容
    for (auto& pair : config.versionConfigs) {
        std::string versionName = pair.first;          // 获取版本名称
        VersionConfig& versionConfig = pair.second;    // 获取对应的 VersionConfig 引用

        // 获取游戏文件夹路径
        std::wstring gameFolderPath = string_to_wstring(versionConfig.gameFolderPath);

        // 获取 mod 列表、资源包列表、保存文件
        std::vector<std::string> mods;
        std::vector<std::string> resourcePacks;
        std::vector<std::string> saves;
        std::string modloader;

        // 获取 minecraft 版本
        std::string minecraftVersion = GetMinecraftVersion(gameFolderPath, modloader);

        // 刷新 mod 列表、资源包列表和保存文件
        GetModList(gameFolderPath, mods, modloader);
        GetResourcePacks(gameFolderPath, resourcePacks);
        GetSaveFiles(gameFolderPath, saves);

        // 更新 versionConfig 中的配置
        versionConfig.minecraftVersion = minecraftVersion;
        versionConfig.modLoaderType = modloader;
        versionConfig.modList = mods;
        versionConfig.resourcePackList = resourcePacks;
        versionConfig.saveGameList = saves;

        // 更新后的配置赋值回 config
        config.versionConfigs[versionName] = versionConfig;
    }

    // 获取 mod 列表
    std::vector<std::string> mods;
    std::vector<std::string> resourcePacks;
    std::vector<std::string> saves;

    std::string modloader;

    // 假设游戏文件夹路径
    std::wstring gameFolderPath = string_to_wstring(config.packagePath);
    std::wstring FolderName = GetFolderNameFromPath(gameFolderPath);
    std::string VersionName = wstring_to_string(FolderName);
    std::string minecraftVersion = GetMinecraftVersion(gameFolderPath, modloader);

    GetModList(gameFolderPath, mods, modloader);
    GetResourcePacks(gameFolderPath, resourcePacks);
    GetSaveFiles(gameFolderPath, saves);

    // 更新 versionConfigs 中的配置
    VersionConfig versionConfig;
    versionConfig.gameFolderPath = wstring_to_string(gameFolderPath);
    versionConfig.minecraftVersion = minecraftVersion;
    versionConfig.modLoaderType = modloader;
    versionConfig.modList = mods;
    versionConfig.resourcePackList = resourcePacks;
    versionConfig.saveGameList = saves;

    config.versionConfigs[VersionName] = versionConfig;

    // 保存更新后的配置文件
    WriteConfig(config, "config\\config.json");


    // 获取当前整合包的版本
    std::string Path = config.versionConfigs[config.selectedGameVersion].gameFolderPath;
    currentSelectedGameVersion = Path;

}
//新建一个.h 和.cpp 文件  ，用于blockstate 方面.json文件   （名字可以就叫blockstate ), 首先需要一个读取方法  输入值是
void init() {
    SetGlobalLocale();
    loadAndUpdateConfig();  // 调用新的方法加载和更新配置

    loadBiomeMapping(config.biomeMappingFile);
    InitializeGlobalBlockPalette();
}

void exportPointCloud() {

    // 创建点云导出器实例，指定输出文件
    PointCloudExporter exporter("output.obj", "block_id_mapping.json");

    // 测量执行时间
    auto start_time = high_resolution_clock::now();

    // 导出点云
    exporter.ExportPointCloud(config.minX, config.maxX, config.minY, config.maxY, config.minZ, config.maxZ);

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    cout << "Total time: " << duration.count() << " milliseconds" << endl;

    // 在程序执行完成后，将 status 改为 1((待机) 并保存
    config.status = 1;

    // 保存更新后的配置文件
    WriteConfig(config, "config\\config.json");

    LoadConfig("config\\config.json");
}

int main() {
    init();
    // 遍历 VersionCache 输出每个 folderName 和对应的 FolderData
    for (const auto& entry : VersionCache) {
        // 获取当前的 folderName
        std::string folderNameStr(entry.first.begin(), entry.first.end());

        // 如果 folderName 与 currentSelectedGameVersion 匹配，才输出该条数据
        if (folderNameStr == currentSelectedGameVersion) {
            std::cout << "Folder: " << folderNameStr << std::endl;

            // 遍历 FolderData 向量并输出内容
            for (const auto& folderData : entry.second) {
                std::cout << "  Namespace: " << folderData.namespaceName << ", Path: " << folderData.path << std::endl;
            }
        }
    }


    while (true) {  
        //status： 
        // 0 代表程序未启动
        // 1 代表待机（每隔一定时间刷新）
        // 2 代表执行导出点云
        
        if (config.status == 1) {
            // 加载配置
            loadAndUpdateConfig();  // 重新加载和更新配置
        }
        else if (config.status == 2) {
            // 如果是 2，执行点云导出逻辑
            exportPointCloud();
        }
        else if (config.status == -1) {
            // 如果 status 为 -1，退出程序
            cout << "退出程序..." << endl;
            break;  // 退出循环并关闭程序
        }
        

        // 每 0.5 秒检查一次
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    return 0;
}
