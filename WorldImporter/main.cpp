#include <iostream>
#include <chrono>
#include <thread>
#include "config.h"
#include "biome.h"
#include "block.h"
#include "PointCloudExporter.h"
#include <fstream> 
#include <latch>
#include <locale> 
#include <codecvt>
#include "global.h"
#include "JarReader.h"
#include "version.h"
#include "blockstate.h"
#include "model.h"
#include "texture.h"
#include "fileutils.h"
#include "GlobalCache.h"
#include "RegionModelExporter.h"

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


        mods = versionConfig.modList;
        resourcePacks = versionConfig.resourcePackList;

        
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

    try
    {
        mods = config.versionConfigs[VersionName].modList;
        
    }
    catch (const std::exception&)
    {

    }
    try
    {
        resourcePacks = config.versionConfigs[VersionName].resourcePackList;

    }
    catch (const std::exception&)
    {

    }

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
    currentSelectedGameVersion = config.selectedGameVersion;

}

void init() {
    SetGlobalLocale();
    loadAndUpdateConfig();
    loadSolidBlocks(config.solidBlocksFile);
    InitializeGlobalBlockPalette();
    InitializeAllCaches();
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
    
    // 测量执行时间
    auto start_time = high_resolution_clock::now();
    //std::vector<std::string> blockList = {
    //"grass_block[snowy=false]",
    //"wheat[age=0]",
    //"wheat[age=1]",
    //"wheat[age=2]",
    //"wheat[age=3]",
    //"wheat[age=4]",
    //"oak_sapling",
    //"bamboo_stairs[facing=east,half=bottom,shape=inner_left]",
    //"acacia_button[face=ceiling,facing=east,powered=false]",
    //"anvil[facing=south]",
    //"acacia_fence[east=true,north=true,south=true,west=false]",
    //"redstone_wire[east=up,north=side,south=side,west=none,power=0]",
    //"activator_rail[powered=false,shape=north_south]"
    //};

    //const auto& modelCache = ProcessBlockstateJson("minecraft", blockList);

    //for (const auto& entry : modelCache) {  // 先获取整个条目
    //    const std::string& blockId = entry.first;
    //    const ModelData& currentModel = entry.second;
    //    if (!currentModel.vertices.empty()) {
    //        CreateModelFiles(currentModel, blockId);
    //    }

    //}
    // 生成包含所有使用 cube 模型的方块
    //GenerateSolidsJson("solids.json", {"block/cube_mirrored_all", "block/cube_all","block/cube_column"});
    //RegionModelExporter::ExportRegionModels(
    //    config.minX, config.maxX,
    //    config.minY, config.maxY,
    //    config.minZ, config.maxZ,
    //    "region_models"
    //);
    ////exportPointCloud();
    // 生成从(0,0)到(15,15)区域的群系图，基于地面高度
    auto biomeMap = Biome::GenerateBiomeMap(-50, -50, 150, 150, -1);

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;
    Biome::PrintAllRegisteredBiomes();

    /*int biomeId = GetBiomeId(20, 2, 50);
    std::cout << "生物群系ID: " << biomeId << std::endl;
    int y = GetHeightMapY(1, 1, "MOTION_BLOCKING_NO_LEAVES");
    std::cout << "地形阻挡高度：" << y << std::endl;*/

    //PrintTextureCache(textureCache);
    //PrintModListCache();

    // while (true) {  
        //status： 
        
        // if (config.status == 1) {
        //     // 加载配置
        loadAndUpdateConfig();  // 重新加载和更新配置
        // }
        // else 
        if (config.status == 1) {
            // 如果是 1，导出区域内所有方块模型
            RegionModelExporter::ExportRegionModels(
                config.minX, config.maxX,
                config.minY, config.maxY,
                config.minZ, config.maxZ,
                "region_models"
            );
            // 在程序执行完成后，将 status 改为 0((待机) 并保存
            config.status = 0;
            
            // 保存更新后的配置文件
            WriteConfig(config, "config\\config.json");
            
            LoadConfig("config\\config.json");
        }
        else if (config.status == 2) {
            // 如果是 2，执行点云导出逻辑
            exportPointCloud();
        }
        else if (config.status == 0) {
            // 如果是 0，执行整合包所有方块状态导出逻辑
            ProcessAllBlockstateVariants();
        }
        // else if (config.status == -1) {
        //     // 如果 status 为 -1，退出程序
        //     cout << "退出程序..." << endl;
        //     // break;  // 退出循环并关闭程序
        // }
        

        // 每 0.5 秒检查一次
        // std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // }

    return 0;
}
