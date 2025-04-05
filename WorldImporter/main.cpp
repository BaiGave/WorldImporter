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
#include "blockstate.h"
#include "model.h"
#include "texture.h"
#include "fileutils.h"
#include "GlobalCache.h"
#include "RegionModelExporter.h"
#include "include/json.hpp"

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;


void init() {
    SetGlobalLocale();
    config = LoadConfig("config\\config.json");
    LoadSolidBlocks(config.solidBlocksFile);
    LoadFluidBlocks(config.fluidsFile);
    RegisterFluidTextures();
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
