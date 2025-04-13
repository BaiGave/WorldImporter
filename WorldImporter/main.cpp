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
#include "JarReader.h"
#include "blockstate.h"
#include "model.h"
#include "texture.h"
#include "fileutils.h"
#include "GlobalCache.h"
#include "RegionModelExporter.h"
#include "include/json.hpp"
#include <windows.h>
#include <string>

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;

void DeleteTexturesFolder() {
    // 获取当前可执行文件所在的目录
    wchar_t cwd[MAX_PATH];
    if (GetModuleFileName(NULL, cwd, MAX_PATH) == 0) {
        return;
    }

    // 提取目录路径
    std::wstring exePath(cwd);
    size_t lastSlash = exePath.find_last_of(L"\\/");
    std::wstring exeDir = exePath.substr(0, lastSlash);

    // 构建textures文件夹的路径
    std::wstring texturesPath = exeDir + L"\\textures";
    std::wstring biomeTexPath = exeDir + L"\\biomeTex";

    // 删除textures文件夹
    if (GetFileAttributes(texturesPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHFILEOPSTRUCT fileOp;
        ZeroMemory(&fileOp, sizeof(fileOp));
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = (texturesPath + L"\0").c_str(); // 必须以双空字符结尾
        fileOp.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperation(&fileOp);
    }

    // 删除biomeTex文件夹
    if (GetFileAttributes(biomeTexPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        SHFILEOPSTRUCT fileOp;
        ZeroMemory(&fileOp, sizeof(fileOp));
        fileOp.wFunc = FO_DELETE;
        fileOp.pFrom = (biomeTexPath + L"\0").c_str(); // 必须以双空字符结尾
        fileOp.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        SHFileOperation(&fileOp);
    }
}
void init() {
    DeleteTexturesFolder();
    SetGlobalLocale();
    config = LoadConfig("config\\config.json");
    InitializeAllCaches();
    LoadSolidBlocks(config.solidBlocksFile);
    LoadFluidBlocks(config.fluidsFile);
    RegisterFluidTextures();
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

}


int main() {
    auto start_time = high_resolution_clock::now();
    init();
    // 测量执行时间
    //auto start_time = high_resolution_clock::now();
    // 生成包含所有使用 cube 模型的方块
    //GenerateSolidsJson("solids.json", {"block/cube_mirrored_all", "block/cube_all","block/cube_column"});
    /*RegionModelExporter::ExportRegionModels(
        config.minX, config.maxX,
        config.minY, config.maxY,
        config.minZ, config.maxZ,
        false,
        "region_models"
    );*/
    ////exportPointCloud();    
    //ProcessAllBlockstateVariants();
    //auto end_time = high_resolution_clock::now();
    //auto duration = duration_cast<milliseconds>(end_time - start_time);
    //cout << "Total time: " << duration.count() << " milliseconds" << endl;
    
    //auto biomeMap = Biome::GenerateBiomeMap(-237, -335, 460, 269, 64);
    // 导出图片
    //Biome::ExportToPNG(biomeMap, "biome_map.png",BiomeColorType::DryFoliage);
    //Biome::PrintAllRegisteredBiomes();
    
    if (config.status == 1) {
        // 如果是 1，导出区域内所有方块模型
        RegionModelExporter::ExportModels("region_models");
    }
    else if (config.status == 2) {
        // 如果是 2，执行点云导出逻辑
        exportPointCloud();
    }
    else if (config.status == 0) {
        // 如果是 0，执行整合包所有方块状态导出逻辑
        ProcessAllBlockstateVariants();
    }
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;
    return 0;
}
