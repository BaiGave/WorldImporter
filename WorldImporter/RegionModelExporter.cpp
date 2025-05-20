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
#include "ModelDeduplicator.h"
#include "hashutils.h"
#include "ChunkLoader.h"
#include "ChunkGenerator.h"
#include "ChunkGroupAllocator.h"
using namespace std;
using namespace std::chrono;  // 新增:方便使用 chrono


void RegionModelExporter::ExportModels(const string& outputName) {
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


    // 预先计算所有区块的LOD等级
    // 扩大区块范围,使其比将要导入的区块大一圈 (与ChunkLoader一致)
    int expandedChunkXStart = chunkXStart - 1;
    int expandedChunkXEnd = chunkXEnd + 1;
    int expandedChunkZStart = chunkZStart - 1;
    int expandedChunkZEnd = chunkZEnd + 1;


    // 预先计算所有区块的LOD等级
    ChunkLoader::CalculateChunkLODs(expandedChunkXStart, expandedChunkXEnd, expandedChunkZStart, expandedChunkZEnd,
        sectionYStart, sectionYEnd);
    // 生成区块组 (使用原始范围，因为GenerateChunkGroups内部会从g_chunkLODs读取)
    ChunkGroupAllocator::GenerateChunkGroups(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd, sectionYStart, sectionYEnd);

    // 加载区块数据 (使用扩展后的范围)
    ChunkLoader::LoadChunks(expandedChunkXStart, expandedChunkXEnd, expandedChunkZStart, expandedChunkZEnd,
        sectionYStart, sectionYEnd);

    

    ProcessBlockstateForBlocks(GetGlobalBlockPalette());

    // 初始化生物群系地图尺寸
    Biome::InitializeBiomeMap(xStart, zStart, xEnd, zEnd);

    // 用于跟踪已处理的区块，避免重复生成生物群系数据
    std::unordered_set<std::pair<int, int>, pair_hash> processedBiomeChunks;
    std::mutex biomeMutex;

    

    // 模型处理阶段
    ModelData finalMergedModel;
    std::unordered_map<string, string> uniqueMaterials;

    
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

    // 创建线程池处理区块组
    {
        unsigned numThreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::atomic<size_t> groupIndex{0};
        std::vector<std::thread> threads;
        threads.reserve(numThreads);

        for (unsigned i = 0; i < numThreads; ++i) {
            threads.emplace_back([&]() {
                while (true) {
                    size_t idx = groupIndex.fetch_add(1);
                    if (idx >= ChunkGroupAllocator::g_chunkGroups.size()) break;
                    const auto& group = ChunkGroupAllocator::g_chunkGroups[idx];
                    ModelData groupModel;
                    groupModel.vertices.reserve(4096 * group.tasks.size());
                    groupModel.faces.reserve(8192 * group.tasks.size());
                    groupModel.uvCoordinates.reserve(4096 * group.tasks.size());
                    std::unordered_map<string, string> localMaterials;

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
                    }
                    if (groupModel.vertices.empty()) continue;
                    if (config.exportFullModel) {
                        mergeToFinalModel(std::move(groupModel));
                    } else {
                        ModelDeduplicator::DeduplicateModel(groupModel);
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
    }
    // 导出不同类型的生物群系颜色图片
    Biome::ExportToPNG("foliage.png", BiomeColorType::Foliage);
    Biome::ExportToPNG("dry_foliage.png", BiomeColorType::DryFoliage);
    Biome::ExportToPNG("water.png", BiomeColorType::Water);
    Biome::ExportToPNG("grass.png", BiomeColorType::Grass);
    Biome::ExportToPNG("waterFog.png", BiomeColorType::WaterFog);
    Biome::ExportToPNG("fog.png", BiomeColorType::Fog);
    Biome::ExportToPNG("sky.png", BiomeColorType::Sky);
    // 最终导出处理
    if (config.exportFullModel && !finalMergedModel.vertices.empty()) {
        ModelDeduplicator::DeduplicateModel(finalMergedModel);
        CreateModelFiles(finalMergedModel, outputName);
    }
    else if (!uniqueMaterials.empty()) {
        CreateSharedMtlFile(uniqueMaterials, outputName);
    }
}


