#include "init.h"
#include "RegionModelExporter.h"
#include "MemoryMonitor.h" // 包含内存监控头文件
#include "block.h"         // 包含 block.h 以访问缓存及其互斥锁的 extern 声明
#include "TaskMonitor.h"   // 包含任务监控器头文件
#include "EntityExporter.h"
#include "EntityModelExporter.h"
#include "PaintingExporter.h"
#include "logutil.h"

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;


int main(int argc, char** argv) {
    CrafterLog::ConsoleCapture capture;
    CrafterLog::environment(argc > 0 ? argv[0] : "WorldImporter");
    const auto process_start = CrafterLog::now_ms();
    try {
        CrafterLog::StageTimer total("WorldImporter 全流程");
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
            CrafterLog::StageTimer stage("方块模型导出");
            RegionModelExporter::ExportModels("region_models");
        }
        if (config.status == 1 && config.exportEntityModels) {
            CrafterLog::StageTimer stage("实体模型导出");
            EntityModelExporter::Export("entities");
        }
        if (config.importEntities) {
            CrafterLog::StageTimer stage("实体数据导出");
            EntityExporter::Export("entities.json");
        }
        { CrafterLog::StageTimer stage("画作导出"); PaintingExporter::Export(); }
        auto end_time = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end_time - start_time);
        cout << "Total time: " << duration.count() << " milliseconds" << endl;

        // 停止内存监控
        //MemoryMonitor::StopMonitoring();

        const auto error_count = CrafterLog::logger().errors();
        const auto warning_count = CrafterLog::logger().warnings();
        CrafterLog::logger().write(error_count ? "WARN" : "INFO",
            string("operation=process_exit returncode=0 status=") + (error_count ? "completed_with_errors" : "success") +
            " errors=" + to_string(error_count) + " warnings=" + to_string(warning_count) +
            " duration_ms=" + to_string(CrafterLog::now_ms() - process_start));
        return 0;
    } catch (const nlohmann::json::exception& e) {
        CrafterLog::error(std::string("operation=process_exit returncode=1 error_type=json error=") + e.what() + " duration_ms=" + std::to_string(CrafterLog::now_ms() - process_start));
        return 1;
    } catch (const std::exception& e) {
        CrafterLog::error(std::string("operation=process_exit returncode=1 error_type=std_exception error=") + e.what() + " duration_ms=" + std::to_string(CrafterLog::now_ms() - process_start));
        return 1;
    } catch (...) {
        CrafterLog::error("operation=process_exit returncode=1 error_type=unknown duration_ms=" + std::to_string(CrafterLog::now_ms() - process_start));
        return 1;
    }
}
