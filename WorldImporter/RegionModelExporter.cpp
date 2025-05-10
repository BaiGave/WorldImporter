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

    // 计算LOD范围
    const int L0 = config.LOD0renderDistance;
    const int L1 = L0 + config.LOD1renderDistance;
    const int L2 = L1 + config.LOD2renderDistance;
    const int L3 = L2 + config.LOD3renderDistance;

    // 区块对齐处理
    auto alignTo16 = [](int value) {
        if (value % 16 == 0) return value;
        return (value > 0) ? ((value + 15) / 16 * 16) : ((value - 15) / 16 * 16);
        };

    // 处理分组模型导出
    auto DeduplicateModel = [](ModelData& model) {
        ModelDeduplicator::DeduplicateVertices(model);
        ModelDeduplicator::DeduplicateUV(model);
        ModelDeduplicator::DeduplicateFaces(model);
        if(config.useGreedyMesh) {
            ModelDeduplicator::GreedyMesh(model);
        }
    };

    if (config.useChunkPrecision) {
        config.minX = alignTo16(xStart); config.maxX = alignTo16(xEnd);
        config.minY = alignTo16(yStart); config.maxY = alignTo16(yEnd);
        config.minZ = alignTo16(zStart); config.maxZ = alignTo16(zEnd);
    }

    // 计算区块坐标范围
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd;
    blockToChunk(xStart, zStart, chunkXStart, chunkZStart);
    blockToChunk(xEnd, zEnd, chunkXEnd, chunkZEnd);

    int sectionYStart, sectionYEnd;
    blockYToSectionY(yStart, sectionYStart);
    blockYToSectionY(yEnd, sectionYEnd);

    // 自动计算LOD中心
    if (config.isLODAutoCenter) {
        config.LODCenterX = (chunkXStart + chunkXEnd) / 2;
        config.LODCenterZ = (chunkZStart + chunkZEnd) / 2;
    }
    {
        size_t effectiveXCount = (chunkXEnd - chunkXStart + 1) + 2;
        size_t effectiveZCount = (chunkZEnd - chunkZStart + 1) + 2;
        size_t secCount = sectionYEnd - sectionYStart + 1;
        g_chunkLODs.reserve(effectiveXCount * effectiveZCount * secCount);
    }

    // 加载区块数据
    ChunkLoader::LoadChunks(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd,
        sectionYStart, sectionYEnd, L0, L1, L2, L3);

    UpdateSkyLightNeighborFlags();
    ProcessBlockstateForBlocks(GetGlobalBlockPalette());

    Biome::ExportAllToPNG(xStart, zStart, xEnd, zEnd);

    // 模型处理阶段
    ModelData finalMergedModel;
    std::unordered_map<string, string> uniqueMaterials;

    // 生成区块组
    const auto chunkGroups = ChunkGroupAllocator::GenerateChunkGroups(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd,
        sectionYStart, sectionYEnd, L0, L1, L2, L3);

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
                    if (idx >= chunkGroups.size()) break;
                    const auto& group = chunkGroups[idx];
                    ModelData groupModel;
                    groupModel.vertices.reserve(4096 * group.tasks.size());
                    groupModel.faces.reserve(8192 * group.tasks.size());
                    groupModel.uvCoordinates.reserve(4096 * group.tasks.size());
                    std::unordered_map<string, string> localMaterials;

                    // 合并组内所有区块模型
                    for (const auto& task : group.tasks) {
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
                        DeduplicateModel(groupModel);
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

    // 最终导出处理
    if (config.exportFullModel && !finalMergedModel.vertices.empty()) {
        DeduplicateModel(finalMergedModel);
        CreateModelFiles(finalMergedModel, outputName);
    }
    else if (!uniqueMaterials.empty()) {
        createSharedMtlFile(uniqueMaterials, outputName);
    }
}


