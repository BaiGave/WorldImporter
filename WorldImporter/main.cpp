#include "PointCloudExporter.h"
#include "RegionModelExporter.h"

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;

void init() {
    // 删除纹理文件夹可以并行执行
    std::jthread folder_deletion_thread([&]() {
        DeleteTexturesFolder();
    });
    
    // 配置必须先加载
    SetGlobalLocale();
    config = LoadConfig("config\\config.json");
    
    // 配置加载完成后，再初始化缓存
    InitializeAllCaches();
    LoadSolidBlocks(config.solidBlocksFile);
    LoadFluidBlocks(config.fluidsFile);
    RegisterFluidTextures();
    InitializeGlobalBlockPalette();
    
    // 等待删除纹理文件夹的线程完成
    if (folder_deletion_thread.joinable()) {
        folder_deletion_thread.join();
    }
}

int main() {
    auto start_time = high_resolution_clock::now();
    init();
    
    
    if (config.status == 1) {
        // 如果是 1，导出区域内所有方块模型
        RegionModelExporter::ExportModels("region_models");
    }
    else if (config.status == 2) {
        // 如果是 2，执行点云导出逻辑
        PointCloudExporter::ExportPointCloudToFile("output.obj", "block_id_mapping.json");
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
