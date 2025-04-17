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

void DeleteDirectory(const std::wstring& path) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFile((path + L"\\*").c_str(), &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findFileData.cFileName, L".") != 0 && wcscmp(findFileData.cFileName, L"..") != 0) {
                std::wstring filePath = path + L"\\" + findFileData.cFileName;
                if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    DeleteDirectory(filePath); // 递归删除子目录
                } else {
                    // 解除文件占用并删除文件
                    if (!DeleteFile(filePath.c_str())) {
                        // 如果删除失败，尝试解除占用
                        MoveFileEx(filePath.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    }
                }
            }
        } while (FindNextFile(hFind, &findFileData) != 0);
        FindClose(hFind);
    }
    // 删除空目录
    RemoveDirectory(path.c_str());
}

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
        DeleteDirectory(texturesPath);
    }

    // 删除biomeTex文件夹
    if (GetFileAttributes(biomeTexPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteDirectory(biomeTexPath);
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
// 自定义哈希函数（已在你的代码中定义）
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};


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
