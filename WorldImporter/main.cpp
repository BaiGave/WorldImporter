#include <iostream>
#include <chrono>
#include "config.h"
#include "biome_mapping.h"
#include "block.h"
#include "PointCloudExporter.h"
#include <fstream> 
#include <locale> 
#include <codecvt>

using namespace std;
using namespace chrono;
//#include <windows.h>


int main() {
    //SetConsoleOutputCP(65001);
    
    
    // 创建并写入配置到文件
    Config config;
    config.directoryPath = "D:\\.minecraft\\saves\\新的世界2";
    config.biomeMappingFile = "config\\mappings\\biomes_mapping.json";
    config.minX = -203;
    config.minY = -64;
    config.minZ = -64;
    config.maxX = -24;
    config.maxY = 103;
    config.maxZ = 227;
    config.status = 0;

    // 写入配置文件
    WriteConfig(config, "config\\config.txt");

    // 加载配置
    config = LoadConfig("config\\config.txt");


    // 加载生物群系映射
    loadBiomeMapping(config.biomeMappingFile);

    InitializeGlobalBlockPalette();

    // 创建点云导出器实例，指定输出文件
    PointCloudExporter exporter("output.obj", "block_id_mapping.json");

    // 测量执行时间
    auto start_time = high_resolution_clock::now();

    // 导出点云
    exporter.ExportPointCloud(config.minX, config.maxX, config.minY, config.maxY, config.minZ,  config.maxZ);

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    cout << "Total time: " << duration.count() << " milliseconds" << endl;

    return 0;
}
