#include "init.h"
#include "RegionModelExporter.h"
#include "MemoryMonitor.h" // 包含内存监控头文件
#include "block.h"         // 包含 block.h 以访问缓存及其互斥锁的 extern 声明
#include "TaskMonitor.h"   // 包含任务监控器头文件

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;


int main() {
    init();

    // 启动内存监控
    // 需要确保 block.cpp 中定义的缓存和互斥锁能够通过 extern 声明被访问
    //MemoryMonitor::StartMonitoring(
    //    sectionCache, sectionCacheMutex, 
    //    EntityBlockCache, entityBlockCacheMutex, 
    //    heightMapCache, heightMapCacheMutex
    //);

    // 初始化任务监控器
    //auto& monitor = GetTaskMonitor();
    
    auto start_time = high_resolution_clock::now();
    if (config.status == 1) {
        // 如果是 1,导出区域内所有方块模型
        RegionModelExporter::ExportModels("region_models");
    }
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;

    // 停止内存监控
    //MemoryMonitor::StopMonitoring();

    return 0;
}
