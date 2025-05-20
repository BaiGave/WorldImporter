#include "init.h"
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
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;
    return 0;
}
