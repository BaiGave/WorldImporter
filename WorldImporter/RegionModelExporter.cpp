#include "ModCT.h"
#include "RegionModelExporter.h"
#include "locutil.h"
#include "objExporter.h"
#include "blocktint.h"
#include "include/stb_image.h"
#include "LODManager.h"
#include "biome.h"
#include "logutil.h"
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
#include "Occlusion.h"
using namespace std;
using namespace std::chrono;  // 新增:方便使用 chrono


void RegionModelExporter::ExportModels(const string& outputName) {
    CrafterLog::StageTimer export_stage("区域模型导出");
    auto export_t0 = std::chrono::high_resolution_clock::now();
    std::cout << "========== 区域模型导出开始 ==========" << std::endl;
    std::cout << "  输出名: " << outputName << std::endl;
    std::cout.flush();

    // 初始化任务监控
    auto& monitor = GetTaskMonitor();
    monitor.Reset();
    monitor.SetStatus(TaskStatus::INITIALIZING, "准备导出区域模型");

    // 初始化坐标范围
    const int xStart = config.minX, xEnd = config.maxX;
    const int yStart = config.minY, yEnd = config.maxY;
    const int zStart = config.minZ, zEnd = config.maxZ;
    std::cout << "  区域范围 X[" << xStart << "," << xEnd << "] "
              << "Y[" << yStart << "," << yEnd << "] "
              << "Z[" << zStart << "," << zEnd << "]" << std::endl;
    std::cout.flush();

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
    { CrafterLog::StageTimer t("计算区块LOD等级");
    ChunkLoader::CalculateChunkLODs(expandedChunkXStart, expandedChunkXEnd, expandedChunkZStart, expandedChunkZEnd,
        sectionYStart, sectionYEnd);
    }
    monitor.UpdateProgress("区块LOD计算", totalChunks, totalChunks, "LOD计算完成");

    // 根据区块数量自动拆分批次(内部会先生成区块组)
    monitor.SetStatus(TaskStatus::GENERATING_CHUNK_BATCHES, "生成区块批次");
    const size_t MAX_TASKS_PER_BATCH = config.maxTasksPerBatch; // 使用配置中的值
    { CrafterLog::StageTimer t("生成区块批次");
    ChunkGroupAllocator::GenerateChunkBatches(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd,sectionYStart, sectionYEnd, MAX_TASKS_PER_BATCH);
    }

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

    // 用于跟踪已处理的区块,避免重复生成生物群系数据(仅在批次加载阶段串行使用)
    std::unordered_set<std::pair<int, int>, pair_hash> processedBiomeChunks;

    // 模型处理阶段
    ModelData finalMergedModel;
    std::unordered_map<string, string> uniqueMaterials;
    std::unordered_map<string, TintResult> uniqueTints;

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
    std::cout << "========== 开始按批次处理 ==========" << std::endl;
    std::cout.flush();

    // 全局进度计数器
    std::atomic<size_t> globalCompletedTasks{0};
    std::atomic<size_t> failedWorkers{0};

    // 初始化全局进度
    monitor.UpdateProgress("总体进度", 0, totalTasksAllBatches);

    auto processModel = [](const ChunkTask& task) -> ModelData {
        // 如果 activeLOD 为 false,则始终生成完整模型
        if (!config.activeLOD) {
            return ChunkGenerator::GenerateChunkModel(task.chunkX, task.sectionY, task.chunkZ);
        }

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
    std::unordered_map<std::string, int> finalMaterialLookup;
    std::mutex materialsMutex;
    std::mutex progressMutex;

    // 线程安全的合并操作
    auto mergeToFinalModel = [&](ModelData&& model) {
        std::lock_guard<std::mutex> lock(finalModelMutex);
        if (finalMergedModel.vertices.empty()) {
            finalMergedModel = std::move(model);
        }
        else {
            MergeModelsDirectly(finalMergedModel, model, &finalMaterialLookup);
        }
        };

    // 线程安全的材质记录
    auto recordMaterials = [&](const std::unordered_map<string, string>& newMaterials,
                                const std::unordered_map<string, TintResult>& newTints) {
        std::lock_guard<std::mutex> lock(materialsMutex);
        for (const auto& nm : newMaterials)
            uniqueMaterials.emplace(nm.first, nm.second);
        for (const auto& nt : newTints)
            uniqueTints.emplace(nt.first, nt.second);
    };

    // 按批次处理区块组
    size_t batchId = 0;

    auto get_batch_expanded_coords = [&](const ChunkBatch& b) -> std::tuple<int, int, int, int> {
        return std::make_tuple(b.chunkXStart - 1, b.chunkXEnd + 1, b.chunkZStart - 1, b.chunkZEnd + 1);
    };

    // 预计算每个边界区块最后一次被哪个批次使用。旧实现每完成一个批次都
    // 扫描全部未来批次，复杂度接近 O(B²)，自动小批次时会明显拖慢。
    std::unordered_map<std::pair<int, int>, size_t, pair_hash> chunkLastUseBatch;
    for (size_t idx = 0; idx < ChunkGroupAllocator::g_chunkBatches.size(); ++idx) {
        int ex0, ex1, ez0, ez1;
        std::tie(ex0, ex1, ez0, ez1) = get_batch_expanded_coords(ChunkGroupAllocator::g_chunkBatches[idx]);
        for (int cx = ex0; cx <= ex1; ++cx) {
            for (int cz = ez0; cz <= ez1; ++cz) {
                chunkLastUseBatch[{cx, cz}] = idx;
            }
        }
    }

    // 运行时遮挡表的增量构建进度（按全局调色板下标）
    size_t lastOcclusionBuilt = 0;

    for (size_t current_batch_idx = 0; current_batch_idx < ChunkGroupAllocator::g_chunkBatches.size(); ++current_batch_idx) {
        const auto& batch = ChunkGroupAllocator::g_chunkBatches[current_batch_idx];
        batchId = current_batch_idx + 1;
        const auto batch_start = CrafterLog::now_ms();
        const auto failures_before = failedWorkers.load();
        CrafterLog::Context batch_context("batch=" + to_string(batchId) + "/" + to_string(totalBatches));

        // 更新批次进度
        monitor.SetStatus(TaskStatus::PROCESSING_BATCH, "处理批次 " + to_string(batchId) + "/" + to_string(totalBatches));
        monitor.UpdateProgress("批次处理", current_batch_idx + 1, totalBatches);
        std::cout << "▶ 批次 " << batchId << "/" << totalBatches << " 开始" << std::endl;
        std::cout.flush();

        // ---------- 加载当前批次(含一圈边界) ----------
        monitor.SetStatus(TaskStatus::LOADING_CHUNKS, "加载批次 " + to_string(batchId) + " 区块");
        int bExpXStart, bExpXEnd, bExpZStart, bExpZEnd;
        std::tie(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd) = get_batch_expanded_coords(batch);

        size_t beforeLoad = CountLoadedChunks();
        globalPaletteFrozen.store(false, std::memory_order_release);
        blockstateCachesFrozen.store(false, std::memory_order_release);
        { CrafterLog::StageTimer timer("批次数据加载");
        ChunkLoader::LoadChunks(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd,
                                sectionYStart, sectionYEnd);
        ModCT_FlushCopycatWarm();
        }
        globalPaletteFrozen.store(true, std::memory_order_release);
        blockstateCachesFrozen.store(true, std::memory_order_release);
        ChunkGenerator::PrepareBlockNames();
        size_t afterLoad = CountLoadedChunks();

        // 模型解析完成后、进入多线程模型阶段前，增量构建运行时遮挡表
        // （每唯一 block state 一次几何+纹理判定，之后查询为 O(1)）
        BuildBlockOcclusionTable(lastOcclusionBuilt);
        lastOcclusionBuilt = globalBlockPalette.size();
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

        // 预生成批次内所有区块的生物群系地图(串行执行)。
        // 旧实现在模型线程内懒加载区块并写群系数据,与其它模型线程
        // 无锁读取 sectionCache/群系表形成数据竞争,这里统一提前到加载阶段。
        {
            CrafterLog::StageTimer timer("批次群系计算");
            std::unordered_set<std::pair<int, int>, pair_hash> batchChunks;
            for (const auto& group : groupsInBatch) {
                for (const auto& task : group.tasks) {
                    batchChunks.emplace(task.chunkX, task.chunkZ);
                }
            }
            for (const auto& ck : batchChunks) {
                if (processedBiomeChunks.find(ck) != processedBiomeChunks.end()) continue;
                // LoadChunks completed this phase. A missing section/heightmap
                // is legitimate (empty/sparse/absent chunk), not a reload request.
                // Lazy reloads here reparsed the same chunk hundreds of times
                // and could mutate a palette already frozen for readers.
                Biome::GenerateBiomeMap(ck.first * 16, ck.second * 16, ck.first * 16 + 15, ck.second * 16 + 15);
                processedBiomeChunks.insert(ck);
            }
        }

        // 重置当前批次的完成任务计数
        std::atomic<size_t> batchCompletedTasks{0};

        // Automatic mode chooses workers from CPU and memory (up to 32).
        // Manual/old configs still default to 1; actual concurrency is also
        // limited by the number of independently schedulable groups.
        const unsigned numThreads = std::max<unsigned>(1,
            std::min<unsigned>(static_cast<unsigned>(config.modelThreads),
                               static_cast<unsigned>(std::max<size_t>(1,
                                   config.exportFullModel ? tasksInCurrentBatch : groupsInBatch.size()))));
        // 通知去重层限制内部并行度,避免线程数量爆炸
        SetModelThreadBudget(static_cast<int>(numThreads));
        std::atomic<size_t> groupIndex{0};
        // Full exports merge in task order after joining. Completion-order
        // merging made coincident-face selection/GreedyMesh depend on timing.
        // Full-file output does not need group-local meshes: schedule section
        // tasks globally so empty/complex/water sections cannot strand workers.
        std::vector<const ChunkTask*> flatTasks;
        if (config.exportFullModel) {
            flatTasks.reserve(tasksInCurrentBatch);
            for (const auto& group : groupsInBatch)
                for (const auto& task : group.tasks) flatTasks.push_back(&task);
        }
        CrafterLog::debug("batch=" + to_string(batchId) + " groups=" + to_string(groupsInBatch.size()) +
                          " tasks=" + to_string(tasksInCurrentBatch) + " model_threads=" + to_string(numThreads) +
                          " loaded_chunks=" + to_string(afterLoad));
        std::vector<ModelData> completedModels(flatTasks.size());
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        auto workerStart = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < numThreads; ++i) {
            threads.emplace_back([&, worker_id = i]() {
                CrafterLog::Context worker_context("batch=" + to_string(batchId) + " worker=" + to_string(worker_id));
                string active_work = "operation=claim_task";
                const ChunkTask* active_task = nullptr;
                auto diagnostic_work = [&]() {
                    return active_work + (active_task ? " chunkX=" + to_string(active_task->chunkX) +
                        " sectionY=" + to_string(active_task->sectionY) + " chunkZ=" + to_string(active_task->chunkZ) +
                        " lod=" + to_string(active_task->lodLevel) : "");
                };
                const auto worker_start = CrafterLog::now_ms();
                try {
                    if (config.exportFullModel) {
                        active_work = "operation=generate_model";
                        for (;;) {
                            const size_t idx = groupIndex.fetch_add(1, std::memory_order_relaxed);
                            if (idx >= flatTasks.size()) break;
                            const auto& task = *flatTasks[idx];
                            active_task = &task;
                            completedModels[idx] = processModel(task);
                            const size_t batchDone = batchCompletedTasks.fetch_add(1) + 1;
                            const size_t done = globalCompletedTasks.fetch_add(1) + 1;
                            if (done % 100 == 0 || done == totalTasksAllBatches) {
                                std::lock_guard<std::mutex> progressLock(progressMutex);
                                monitor.UpdateProgress("总体进度", done, totalTasksAllBatches);
                                monitor.UpdateProgress("批次进度", batchDone, tasksInCurrentBatch);
                            }
                        }
                        CrafterLog::contextual_debug("operation=worker_exit status=success duration_ms=" + to_string(CrafterLog::now_ms() - worker_start));
                        return;
                    }
                    while (true) {
                        size_t idx = groupIndex.fetch_add(1);
                        if (idx >= groupsInBatch.size()) break;
                        const auto& group = groupsInBatch[idx];
                        active_work = "operation=group group=" + to_string(idx) + " startX=" + to_string(group.startX) + " startZ=" + to_string(group.startZ);
                        ModelData groupModel;
                        // The first nonempty section moves its arrays here;
                        // reserving before that move only allocates discarded memory.
                        std::unordered_map<std::string, int> groupMaterialLookup;
                        std::unordered_map<string, string> localMaterials;
                        std::unordered_map<string, TintResult> localTints;

                        // 记录当前组内需要处理的任务数
                        size_t tasksInCurrentGroup = group.tasks.size();
                        size_t processedInGroup = 0;

                        // 合并组内所有区块模型
                        for (const auto& task : group.tasks) {
                            // processModel 返回可移动对象；提前 reserve 随即会被移动赋值
                            // 丢弃，旧代码每个 section 都做了三次无效分配。
                            active_task = &task;
                            ModelData chunkModel = processModel(task);
                            if (groupModel.vertices.empty()) {
                                groupModel = std::move(chunkModel);
                            } else {
                                MergeModelsDirectly(groupModel, chunkModel, &groupMaterialLookup);
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
                        for (const auto& mat : groupModel.materials)
                            localTints[mat.name] = mat.tint;
                        if (config.exportFullModel) {
                            completedModels[idx] = std::move(groupModel);
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
                            {
                                // 分组模型各自剪掉无面引用的材质，避免写进共享 mtl
                                const std::unordered_set<std::string> overlayKeep = GetOverlayReferencedMaterialNames();
                                SeparateCoincidentFaces(groupModel);
                                PruneUnusedMaterials(groupModel, &overlayKeep);
                            }
                            active_task = nullptr;
                            active_work = "operation=write_group output=" + groupFileName;
                            CrafterLog::contextual_debug(active_work + " faces=" + to_string(groupModel.faces.size()) + " materials=" + to_string(localMaterials.size()));
                            CreateMultiModelFiles(groupModel, groupFileName, localMaterials, outputName);
                            recordMaterials(localMaterials, localTints);
                        }
                    }
                } catch (const std::exception& e) {
                    // 单个分组处理异常不应导致整个进程终止，跳过该组并记录错误
                    failedWorkers.fetch_add(1);
                    CrafterLog::contextual_error(diagnostic_work() + " status=error error=" + e.what() +
                        " duration_ms=" + to_string(CrafterLog::now_ms() - worker_start) + " action=worker_stopped_remaining_tasks_may_be_unprocessed");
                } catch (...) {
                    failedWorkers.fetch_add(1);
                    CrafterLog::contextual_error(diagnostic_work() + " status=error error=unknown action=worker_stopped_remaining_tasks_may_be_unprocessed");
                }
            });
        }
        for (auto& t : threads) {
            if (t.joinable()) t.join();
        }

        std::cout << "[perf] 模型工作线程耗时 " << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - workerStart).count() << " ms" << std::endl;
        if (config.exportFullModel) {
            CrafterLog::StageTimer timer("批次模型合并");
            for (auto& model : completedModels) {
                if (model.vertices.empty()) continue;
                mergeToFinalModel(std::move(model));
                // Release each source before merging the next one.
                model = ModelData{};
            }
        }

        if (config.exportFullModel) {
            const size_t meshBytes = finalMergedModel.vertices.capacity() * sizeof(float) +
                finalMergedModel.uvCoordinates.capacity() * sizeof(float) +
                finalMergedModel.faces.capacity() * sizeof(Face);
            std::cout << "[perf] 累计模型: faces=" << finalMergedModel.faces.size()
                      << " arrayCapacityMiB=" << meshBytes / (1024 * 1024) << std::endl;
        }
        // ---------- 卸载当前批次 ----------
        size_t beforeUnload = CountLoadedChunks();

        // 仅检查当前批次范围内的区块是否还有未来用途，查询预计算的最后
        // 使用批次即可，避免重复扫描全部未来批次。
        std::unordered_set<std::pair<int, int>, pair_hash> retain_for_future_batches;
        for (int cx = bExpXStart; cx <= bExpXEnd; ++cx) {
            for (int cz = bExpZStart; cz <= bExpZEnd; ++cz) {
                auto lastUse = chunkLastUseBatch.find({cx, cz});
                if (lastUse != chunkLastUseBatch.end() && lastUse->second > current_batch_idx) {
                    retain_for_future_batches.insert({cx, cz});
                }
            }
        }

        ChunkLoader::UnloadChunks(bExpXStart, bExpXEnd, bExpZStart, bExpZEnd, sectionYStart, sectionYEnd, retain_for_future_batches);
        size_t afterUnload = CountLoadedChunks();
        size_t unloadedCnt = (beforeUnload > afterUnload) ? (beforeUnload - afterUnload) : 0;

        const auto batch_failures = failedWorkers.load() - failures_before;
        const string batch_summary = "operation=batch_summary batch=" + to_string(batchId) +
            " expected_tasks=" + to_string(tasksInCurrentBatch) + " completed_tasks=" + to_string(batchCompletedTasks.load()) +
            " failed_workers=" + to_string(batch_failures) + " duration_ms=" + to_string(CrafterLog::now_ms() - batch_start) +
            " loaded_chunks=" + to_string(newlyLoaded) + " unloaded_chunks=" + to_string(unloadedCnt) + " retained_chunks=" + to_string(afterUnload);
        if (batch_failures || batchCompletedTasks.load() != tasksInCurrentBatch)
            CrafterLog::warn(batch_summary + " status=incomplete");
        else CrafterLog::info(batch_summary + " status=success");
        std::cout << "■ 批次 " << batchId << "/" << totalBatches
                  << " 完成:新加载区块 " << newlyLoaded
                  << ", 卸载 " << unloadedCnt
                  << ", 剩余 " << afterUnload << std::endl;
        std::cout.flush();
    }

    if (failedWorkers.load()) CrafterLog::error("operation=export_summary status=incomplete failed_workers=" + to_string(failedWorkers.load()) +
        " completed_tasks=" + to_string(globalCompletedTasks.load()) + " expected_tasks=" + to_string(totalTasksAllBatches));
    std::cout << "========== 全部批次处理完成 ==========" << std::endl;
    std::cout << "  统计:唯一材质 " << uniqueMaterials.size()
              << ",唯一 tint " << uniqueTints.size()
              << ",总顶点 " << finalMergedModel.vertices.size()
              << ",模组材质复用 " << GetModMaterialReuseCount() << " 次"
              << "(" << (config.mergeModTextures ? "开" : "关") << ")" << std::endl;
    std::cout.flush();

    // 导出不同类型的生物群系颜色图片
    { CrafterLog::StageTimer t("导出生物群系贴图");
    monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "BiomeExportToPNG");
    Biome::ExportToPNG("foliage.png", BiomeColorType::Foliage);
    Biome::ExportToPNG("dry_foliage.png", BiomeColorType::DryFoliage);
    Biome::ExportToPNG("water.png", BiomeColorType::Water);
    Biome::ExportToPNG("grass.png", BiomeColorType::Grass);
    Biome::ExportToPNG("waterFog.png", BiomeColorType::WaterFog);
    Biome::ExportToPNG("fog.png", BiomeColorType::Fog);
    Biome::ExportToPNG("sky.png", BiomeColorType::Sky);
    }

    // tint 诊断（只提示，不改色）：
    //  1) 群系色型缺 biomeTex 图 → Blender 侧取不到颜色，表现为发白/发灰；
    //  2) 面写了 tintindex 但查不到色型 / 原版色型方块模型没有 tintindex → 游戏里也不染，
    //     这里显式列出，避免再靠"猜色型"去修。
    { CrafterLog::StageTimer t("tint 诊断");
    CheckBiomeTexCoverage(uniqueTints, &finalMergedModel);
    PrintTintDiagnostics();
    }
    // Group workers have joined. The final mesh is now the only task; restore
    // internal CPU parallelism instead of retaining the former outer budget.
    SetModelThreadBudget(1);
    // 最终导出处理
    if (config.exportFullModel && !finalMergedModel.vertices.empty()) {
        { CrafterLog::StageTimer t("顶点去重");
        monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateModel");
        ModelDeduplicator::DeduplicateModel(finalMergedModel);
        }
        { CrafterLog::StageTimer t("分离共面正反面");
        const size_t separatedFaces = SeparateCoincidentFaces(finalMergedModel);
        std::cout << "分离共面正反面: 处理 " << separatedFaces << " 张面（避免 Blender 判重时丢面）" << std::endl;
        }
        { CrafterLog::StageTimer t("材质剪枝");
        const std::unordered_set<std::string> overlayKeep = GetOverlayReferencedMaterialNames();
        const size_t prunedMaterials = PruneUnusedMaterials(finalMergedModel, &overlayKeep);
        std::cout << "材质剪枝: 移除 " << prunedMaterials << " 个无引用材质, 剩余 "
                  << finalMergedModel.materials.size() << " 个" << std::endl;
        }
        { CrafterLog::StageTimer t("写入模型文件");
        monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "CreateModelFiles");
        CreateModelFiles(finalMergedModel, outputName);
        }
        // 全模型路径同样输出 tint.json
        { CrafterLog::StageTimer t("写入 tint.json");
        std::unordered_map<string, TintResult> fullModelTints;
        for (const auto& mat : finalMergedModel.materials)
            fullModelTints[mat.name] = mat.tint;
        CreateTintJsonFile(fullModelTints);
        }
        { CrafterLog::StageTimer t("写入 overlay.json");
        CreateOverlayJsonFile();
        }
        { CrafterLog::StageTimer t("写入 doubleSided.json");
        CreateDoubleSidedJsonFile();
        }
    }
    else if (!uniqueMaterials.empty()) {
        { CrafterLog::StageTimer t("写入共享 mtl");
        monitor.SetStatus(TaskStatus::EXPORTING_MODELS, "CreateSharedMtlFile");
        CreateSharedMtlFile(uniqueMaterials, outputName, uniqueTints);
        }
        { CrafterLog::StageTimer t("写入 overlay.json");
        CreateOverlayJsonFile();
        }
        { CrafterLog::StageTimer t("写入 doubleSided.json");
        CreateDoubleSidedJsonFile();
        }
    }

    // 超长材质名映射（Blender 63 字节截断后，插件靠它还原原名做分类）
    { CrafterLog::StageTimer t("写入 materials.json");
    WriteMaterialsJson();
    }

    monitor.SetStatus(TaskStatus::COMPLETED, "Finished");
    auto export_t1 = std::chrono::high_resolution_clock::now();
    auto export_ms = std::chrono::duration_cast<std::chrono::milliseconds>(export_t1 - export_t0).count();
    std::cout << "========== 区域模型导出完成 (" << export_ms << "ms) ==========" << std::endl;
    std::cout.flush();
}


