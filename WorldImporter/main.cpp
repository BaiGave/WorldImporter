#include "init.h"
#include "PointCloudExporter.h"
#include "RegionModelExporter.h"
Config config;  // 定义全局变量

using namespace std;
using namespace chrono;


int main() {
    init();
    auto start_time = high_resolution_clock::now();
    if (config.status == 1) {
        // 如果是 1,导出区域内所有方块模型
        RegionModelExporter::ExportModels("region_models");
    }
    else if (config.status == 2) {
        // 如果是 2,执行点云导出逻辑
        PointCloudExporter::ExportPointCloudToFile("output.obj", "block_id_mapping.json");
    }
    else if (config.status == 0) {
        // 如果是 0,执行整合包所有方块状态导出逻辑
        ProcessAllBlockstateVariants();
    }
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;
    return 0;
}
