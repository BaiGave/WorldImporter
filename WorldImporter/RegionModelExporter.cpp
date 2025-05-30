#include "RegionModelExporter.h"
#include "locutil.h"
#include "ObjExporter.h"
#include "include/stb_image.h"
#include "LODManager.h"
#include "biome.h"
#include <regex>
#include <tuple>
#include <future>
#include <thread>
#include <atomic>
#include <unordered_set>
#include "ModelDeduplicator.h"
#include "hashutils.h"
#include "ChunkLoader.h"
#include "ChunkGenerator.h"
#include "ChunkGroupAllocator.h"
#include <limits>
#include <mutex>
#include <shared_mutex>
#include "block.h"
#include "TaskMonitor.h"
using namespace std;
using namespace std::chrono;  // 新增:方便使用 chrono


void RegionModelExporter::ExportModels(const string& outputName) {
    // 初始化任务监控
    auto& monitor = GetTaskMonitor();
    monitor.Reset();
    monitor.SetStatus(TaskStatus::INITIALIZING, "准备导出区域模型");

    // 初始化坐标范围
    const int xStart = config.minX, xEnd = config.maxX;
    const int yStart = config.minY, yEnd = config.maxY;
    const int zStart = config.minZ, zEnd = config.maxZ;

    // 使用 Config 中存储的区块和 Section 坐标范围
    const int chunkXStart = config.chunkXStart;
    const int chunkXEnd = config.chunkXEnd;
    const int chunkZStart = config.chunkZStart;
    const int chunkZEnd = config.chunkZEnd;

    const int sectionYStart = config.sectionYStart;
    const int sectionYEnd = config.sectionYEnd;

    // 扩大区块范围,使其比将要导入的区块大一圈 (与ChunkLoader一致)
    int expandedChunkXStart = chunkXStart - 1;
    int expandedChunkXEnd = chunkXEnd + 1;
    int expandedChunkZStart = chunkZStart - 1;
    int expandedChunkZEnd = chunkZEnd + 1;

    // 计算总区块数量
    int totalChunksX = expandedChunkXEnd - expandedChunkXStart + 1;
    int totalChunksZ = expandedChunkZEnd - expandedChunkZStart + 1;
    int totalChunks = totalChunksX * totalChunksZ;
    monitor.UpdateProgress("区块LOD计算", 0, totalChunks);

    // 预先计算所有区块的LOD等级
    ChunkLoader::CalculateChunkLODs(expandedChunkXStart, expandedChunkXEnd, expandedChunkZStart, expandedChunkZEnd,
        sectionYStart, sectionYEnd);
    
    monitor.UpdateProgress("区块LOD计算", totalChunks, totalChunks, "LOD计算完成");

    // 根据区块数量自动拆分批次(内部会先生成区块组)
    monitor.SetStatus(TaskStatus::GENERATING_CHUNK_BATCHES, "生成区块批次");
    const size_t MAX_TASKS_PER_BATCH = config.maxTasksPerBatch; // 使用配置中的值
    ChunkGroupAllocator::GenerateChunkBatches(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd,sectionYStart, sectionYEnd, MAX_TASKS_PER_BATCH);

    // 更新批次生成状态
    size_t totalBatches = ChunkGroupAllocator::g_chunkBatches.size();
    size_t totalChunkGroups = ChunkGroupAllocator::g_chunkGroups.size();
    
    // 初始化生物群系地图尺寸
    Biome::InitializeBiomeMap(xStart, zStart, xEnd, zEnd);
    
    // 定义辅助:统计当前已加载的区块(忽略 SectionY)
    auto CountLoadedChunks = []() -> size_t {
        std::shared_lock<std::shared_mutex> readLock(g_chunkSectionInfoMapMutex);
        std::unordered_set<std::pair<int, int>, pair_hash> chunkSet;
        for (const auto& entry : g_chunkSectionInfoMap) {
            if (!entry.second.isLoaded.load(std::memory_order_relaxed)) continue;
            int cx = std::get<0>(entry.first);
            int cz = std::get<2>(entry.first);
            chunkSet.emplace(cx, cz);
        }
        return chunkSet.size();
    };

    // 用于跟踪已处理的区块，避免重复生成生物群系数据
    std::unordered_set<std::pair<int, int>, pair_hash> processedBiomeChunks;
    std::mutex biomeMutex;

    // 模型处理阶段
    ModelData finalMergedModel;
    std::unordered_map<string, string> uniqueMaterials;

    // 计算所有批次的总任务数
    size_t totalTasksAllBatches = 0;
    for (const auto& batch : ChunkGroupAllocator::g_chunkBatches) {
        for (const auto& group : batch.groups) {
            totalTasksAllBatches += group.tasks.size();
        }
    }
    
    // 输出总体信息
    std::cout << "总批次数: " << totalBatches 
              << ", 总区块组数: " << totalChunkGroups 
              << ", 总任务数: " << totalTasksAllBatches << std::endl;
    
    // 全局进度计数器
    std::atomic<size_t> globalCompletedTasks{0};
    
    // 初始化全局进度
    monitor.UpdateProgress("总体进度", 0, totalTasksAllBatches);
    
    auto processModel = [](const ChunkTask& task) -> ModelData {
        // 如果 LOD0renderDistance 为 0 且是普通区块,跳过生成
        if (config.LOD0renderDistance == 0 && task.lodLevel == 0.0f) {
            // LOD0 禁用时,将中央区块按 LOD1 生成
            return ChunkGenerator::GenerateLODChunkModel(task.chunkX, task.sectionY, task.chunkZ, 1.0f);
        }
        if (task.lodLevel == 0.0f) {
            return ChunkGenerator::GenerateChunkModel(task.chunkX, task.sectionY, task.chunkZ);
        } else {
            return ChunkGenerator::GenerateLODChunkModel(task.chunkX, task.sectionY, task.chunkZ, task.lodLevel);
        }
    };

    std::mutex finalModelMutex;
    std::mutex materialsMutex;
    std::mutex progressMutex;

    // 线程安全的合并操作
    auto mergeToFinalModel = [&](ModelData&& model) {
        std::lock_guard<std::mutex> lock(finalModelMutex);
        if (finalMergedModel.vertices.empty()) {
            finalMergedModel = std::move(model);
        }
        else {
            MergeModelsDirectly(finalMergedModel, model);
        }
        };

    // 线程安全的材质记录
    auto recordMaterials = [&](const std::unordered_map<string, string>& newMaterials) {
        std::lock_guard<std::mutex> lock(materialsMutex);
        uniqueMaterials.insert(newMaterials.begin(), newMaterials.end());
        };

    // 按批次处理区块组
    size_t batchId = 0;

    auto get_batch_expanded_coords = [&](const ChunkBatch& b) -> std::tuple<int, int, int, int> {
        return std::make_tuple(b.chunkXStart - 1, b.chunkXEnd + 1, b.chunkZStart - 1, b.chunkZEnd + 1);
    };

    for (size_t current_batch_idx = 0; current_batch_idx < ChunkGroupAllocator::g_chunkBatches.size(); ++current_batch_idx) {
        const auto& batch = ChunkGroupAllocator::g_chunkBatches[current_batch_idx];
        batchId = current_batch_idx + 1;
        
        // 更新批次进度
        monitor.SetStatus(TaskStatus::PROCESSING_BATCH, "处理批次 " + to_string(batchId) + "/" + to_string(totalBatches));
        monitor.UpdateProgress("批次处理", current_batch_idx + 1, totalBatches);
        
        // ---------- 加载当前批次(含一圈边界) ----------
        monitor.SetStatus(TaskStatus::LOADING_CHUNKS, "加载批次 " + to_string(batchId) + " 区块");
        int bExpXStart, bExpXEnd, bExpZStart, bExpZEnd;
        std::tie(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd) = get_batch_expanded_coords(batch);

        size_t beforeLoad = CountLoadedChunks();
        ChunkLoader::LoadChunks(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd,
                                sectionYStart, sectionYEnd);
        size_t afterLoad = CountLoadedChunks();
        size_t newlyLoaded = (afterLoad > beforeLoad) ? (afterLoad - beforeLoad) : 0;

        // 处理天空光照邻居标志(在模型线程前执行,避免写冲突)
        UpdateSkyLightNeighborFlags();

        // ---------- 处理当前批次 ----------
        monitor.SetStatus(TaskStatus::GENERATING_MODELS, "生成批次 " + to_string(batchId) + " 模型");
        const auto& groupsInBatch = batch.groups;
        
        // 计算当前批次的任务数
        size_t tasksInCurrentBatch = 0;
        for (const auto& group : groupsInBatch) {
            tasksInCurrentBatch += group.tasks.size();
        }
        
        // 重置当前批次的完成任务计数
        std::atomic<size_t> batchCompletedTasks{0};

        unsigned numThreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::atomic<size_t> groupIndex{0};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (unsigned i = 0; i < numThreads; ++i) {
            threads.emplace_back([&]() {
                while (true) {
                    size_t idx = groupIndex.fetch_add(1);
                    if (idx >= groupsInBatch.size()) break;
                    const auto& group = groupsInBatch[idx];
                    ModelData groupModel;
                    groupModel.vertices.reserve(4096 * group.tasks.size());
                    groupModel.faces.reserve(8192 * group.tasks.size());
                    groupModel.uvCoordinates.reserve(4096 * group.tasks.size());
                    std::unordered_map<string, string> localMaterials;

                    // 记录当前组内需要处理的任务数
                    size_t tasksInCurrentGroup = group.tasks.size();
                    size_t processedInGroup = 0;

                    // 合并组内所有区块模型
                    for (const auto& task : group.tasks) {
                        // 为当前区块生成生物群系地图数据 (如果尚未生成)
                        std::pair<int, int> chunkKey = {task.chunkX, task.chunkZ};
                        {
                            std::lock_guard<std::mutex> lock(biomeMutex);
                            if (processedBiomeChunks.find(chunkKey) == processedBiomeChunks.end()) {
                                // 计算当前区块的方块坐标范围
                                int blockXStart = task.chunkX * 16;
                                int blockXEnd = blockXStart + 15;
                                int blockZStart = task.chunkZ * 16;
                                int blockZEnd = blockZStart + 15;
                                // 生成该区块的生物群系地图数据
                                Biome::GenerateBiomeMap(blockXStart, blockZStart, blockXEnd, blockZEnd);
                                processedBiomeChunks.insert(chunkKey);
                            }
                        }

                        ModelData chunkModel;
                            chunkModel.vertices.reserve(4096);
                            chunkModel.faces.reserve(8192);
                            chunkModel.uvCoordinates.reserve(4096);
                            chunkModel = processModel(task);
                        if (groupModel.vertices.empty()) {
                            groupModel = std::move(chunkModel);
                        } else {
                            MergeModelsDirectly(groupModel, chunkModel);
                        }
                        
                        // 更新批次完成任务计数
                        batchCompletedTasks.fetch_add(1);
                        processedInGroup++;
                        
                        // 更新全局完成任务计数
                        size_t globalCompleted = globalCompletedTasks.fetch_add(1) + 1;
                        
                        // 每100个任务或组内处理完成时才更新一次全局进度
                        if (globalCompleted % 100 == 0 || 
                            globalCompleted == totalTasksAllBatches || 
                            processedInGroup == tasksInCurrentGroup) {
                            
                            // 使用互斥锁确保同一时间只有一个线程更新进度
                            {
                                std::lock_guard<std::mutex> progressLock(progressMutex);
                                // 更新全局进度
                                monitor.UpdateProgress("总体进度", globalCompleted, totalTasksAllBatches);
                                
                                // 同时显示当前批次进度
                                size_t batchCompleted = batchCompletedTasks.load();
                                std::string batchInfo = "批次 " + to_string(batchId) + "/" + to_string(totalBatches) + 
                                                      " (" + to_string(batchCompleted) + "/" + to_string(tasksInCurrentBatch) + ")";
                                monitor.UpdateProgress("批次进度", batchCompleted, tasksInCurrentBatch, batchInfo);
                            }
                        }
                    }
                    if (groupModel.vertices.empty()) continue;
                    if (config.exportFullModel) {
                        mergeToFinalModel(std::move(groupModel));
                    } else {
                        // 去重处理
                        {
                            monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices");
                            ModelDeduplicator::DeduplicateVertices(groupModel);
                            
                            monitor.SetStatus(TaskStatus::DEDUPLICATING_UV, "DeduplicateUV");
                            ModelDeduplicator::DeduplicateUV(groupModel);
                            
                            monitor.SetStatus(TaskStatus::DEDUPLICATING_FACES, "DeduplicateFaces");
                            ModelDeduplicator::DeduplicateFaces(groupModel);
                            
                            if (config.useGreedyMesh) {
                                monitor.SetStatus(TaskStatus::GREEDY_MESHING, "GreedyMesh");
                                ModelDeduplicator::GreedyMesh(groupModel);
                            }
                        }
                        
                        const string groupFileName = outputName +
                            "_x" + to_string(group.startX) +
                            "_z" + to_string(group.startZ);
                        CreateMultiModelFiles(groupModel, groupFileName, localMaterials, outputName);
                        recordMaterials(localMaterials);
                    }
                }
            });
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        // ---------- 卸载当前批次 ----------
        size_t beforeUnload = CountLoadedChunks();

        // 构建需要为未来批次保留的扩展区块集合
        std::unordered_set<std::pair<int, int>, pair_hash> retain_for_future_batches;
        for (size_t future_batch_idx = current_batch_idx + 1; future_batch_idx < ChunkGroupAllocator::g_chunkBatches.size(); ++future_batch_idx) {
            const auto& future_batch = ChunkGroupAllocator::g_chunkBatches[future_batch_idx];
            int fbExpXStart, fbExpXEnd, fbExpZStart, fbExpZEnd;
            std::tie(fbExpXStart, fbExpXEnd, fbExpZStart, fbExpZEnd) = get_batch_expanded_coords(future_batch);
            for (int cx = fbExpXStart; cx <= fbExpXEnd; ++cx) {
                for (int cz = fbExpZStart; cz <= fbExpZEnd; ++cz) {
                    retain_for_future_batches.insert({cx, cz});
                }
            }
        }

        ChunkLoader::UnloadChunks(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd, sectionYStart, sectionYEnd, retain_for_future_batches);
        size_t afterUnload = CountLoadedChunks();
        size_t unloadedCnt = (beforeUnload > afterUnload) ? (beforeUnload - afterUnload) : 0;

        std::cout << "批次" << batchId << "完成：新加载区块 " << newlyLoaded << "," << unloadedCnt << "," << afterUnload<<std::endl;
    }

    // 导出不同类型的生物群系颜色图片
    monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "BiomeExportToPNG");
    Biome::ExportToPNG("foliage.png", BiomeColorType::Foliage);
    Biome::ExportToPNG("dry_foliage.png", BiomeColorType::DryFoliage);
    Biome::ExportToPNG("water.png", BiomeColorType::Water);
    Biome::ExportToPNG("grass.png", BiomeColorType::Grass);
    Biome::ExportToPNG("waterFog.png", BiomeColorType::WaterFog);
    Biome::ExportToPNG("fog.png", BiomeColorType::Fog);
    Biome::ExportToPNG("sky.png", BiomeColorType::Sky);
    // 最终导出处理
    if (config.exportFullModel && !finalMergedModel.vertices.empty()) {
        monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateModel");
        ModelDeduplicator::DeduplicateModel(finalMergedModel);
        
        monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "CreateModelFiles");
        CreateModelFiles(finalMergedModel, outputName);
    }
    else if (!uniqueMaterials.empty()) {
        monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "CreateSharedMtlFile");
        CreateSharedMtlFile(uniqueMaterials, outputName);
    }
    
    monitor.SetStatus(TaskStatus::COMPLETED, "Finished");
}


