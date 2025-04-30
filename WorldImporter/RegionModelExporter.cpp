#include "RegionModelExporter.h"
#include "locutil.h"
#include "ObjExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "Fluid.h"
#include "LODManager.h"
#include "texture.h"
#include <iomanip>  // 用于 std::setw 和 std::setfill
#include <sstream>  // 用于 std::ostringstream
#include <regex>
#include <tuple>
#include <future>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include <thread>
#include <atomic>
#include "ModelDeduplicator.h"
#include "ChunkGroupAllocator.h"
#include <utility> // 支持 std::move
#include "hashutils.h"

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono

std::unordered_set<std::pair<int, int>, pair_hash> processedChunks; // 存储已处理的块的集合
std::mutex entityCacheMutex; // 互斥量，确保线程安全

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

    // Optimize performance: reserve g_chunkLODs to avoid rehashing
    {
        size_t effectiveXCount = (chunkXEnd - chunkXStart + 1) + 2;
        size_t effectiveZCount = (chunkZEnd - chunkZStart + 1) + 2;
        size_t secCount = sectionYEnd - sectionYStart + 1;
        g_chunkLODs.reserve(effectiveXCount * effectiveZCount * secCount);
    }

    // 加载区块数据
    LoadChunks(chunkXStart, chunkXEnd, chunkZStart, chunkZEnd,
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
        // 如果 LOD0renderDistance 为 0 且是普通区块，跳过生成
        if (config.LOD0renderDistance == 0 && task.lodLevel == 0.0f) {
            // LOD0 禁用时，将中央区块按 LOD1 生成
            return GenerateLODChunkModel(task.chunkX, task.sectionY, task.chunkZ, 1.0f);
        }
        if (task.lodLevel == 0.0f) {
            return GenerateChunkModel(task.chunkX, task.sectionY, task.chunkZ);
        } else {
            return GenerateLODChunkModel(task.chunkX, task.sectionY, task.chunkZ, task.lodLevel);
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
                    std::unordered_map<string, string> localMaterials;

                    // 合并组内所有区块模型
                    for (const auto& task : group.tasks) {
                        ModelData chunkModel = processModel(task);
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

void RegionModelExporter::LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd,
    int LOD0renderDistance, int LOD1renderDistance,
    int LOD2renderDistance, int LOD3renderDistance) {
    // Optimize performance: precompute squared LOD distances
    int L0d2 = LOD0renderDistance * LOD0renderDistance;
    int L1d2 = LOD1renderDistance * LOD1renderDistance;
    int L2d2 = LOD2renderDistance * LOD2renderDistance;
    int L3d2 = LOD3renderDistance * LOD3renderDistance;

    // 扩大区块范围，使其比将要导入的区块大一圈
    chunkXStart--;
    chunkXEnd++;
    chunkZStart--;
    chunkZEnd++;

    // 加载所有相关的分块和分段
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            LoadAndCacheBlockData(chunkX, chunkZ);
            int dx = chunkX - config.LODCenterX;
            int dz = chunkZ - config.LODCenterZ;
            int dist2 = dx * dx + dz * dz;
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                // Optimize performance: use squared distance
                float chunkLOD = 0.0f;
                if (config.activeLOD)
                {
                    if (dist2 <= L0d2) {
                        chunkLOD = 0.0f;
                    }
                    else if (dist2 <= L1d2) {
                        chunkLOD = 1.0f;
                    }
                    else if (dist2 <= L2d2) {
                        chunkLOD = 2.0f;
                    }
                    else if (dist2 <= L3d2) {
                        chunkLOD = 4.0f;
                    }
                    else {
                        chunkLOD = 8.0f;
                    }
                }

                g_chunkLODs[std::make_tuple(chunkX, sectionY, chunkZ)] = chunkLOD;
            }
        }
    }
}

ModelData RegionModelExporter::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    ModelData chunkModel;
    static const std::unordered_map<FaceType, int> neighborIndexMap = {
        {FaceType::DOWN, 1}, {FaceType::UP, 0}, {FaceType::NORTH, 4}, 
        {FaceType::SOUTH, 5}, {FaceType::WEST, 2}, {FaceType::EAST, 3}
    };
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;
    
    // 计算区块内的方块范围
    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;
   
    
    // 遍历区块内的每个方块
    for (int x = blockXStart; x < blockXStart + 16; ++x) {
        for (int z = blockZStart; z < blockZStart + 16; ++z) {
            for (int y = blockYStart; y < blockYStart + 16; ++y) {
                
                // 检查当前方块是否在导出区域内
                if (x < xStart || x > xEnd || y < yStart || y > yEnd || z < zStart || z > zEnd) {
                    continue; // 跳过不在导出区域内的方块
                }
                
                
                std::array<bool, 6> neighbors; // 邻居是否为空气
                std::array<int, 10> fluidLevels; // 流体液位

                int id = GetBlockIdWithNeighbors(
                    x, y, z,
                    neighbors.data(),
                    fluidLevels.data()
                );
                Block currentBlock = GetBlockById(id);
                string blockName = GetBlockNameById(id);
                if (blockName == "minecraft:air" ) continue;

                if (config.exportLightBlockOnly)
                {
                    string processed = blockName;

                    // 提取命名空间
                    size_t colonPos = processed.find(':');
                    string ns = "minecraft"; // 默认命名空间
                    if (colonPos != string::npos) {
                        ns = processed.substr(0, colonPos);
                        processed = processed.substr(colonPos + 1);
                    }

                    // 提取方块ID和状态
                    size_t bracketPos = processed.find('[');
                    string LN = processed.substr(0, bracketPos);


                    if (LN != "light") {
                        continue;
                    }
                }
                if (config.cullCave)
                {
                    if (GetSkyLight(x, y, z) == -1)continue;
                }
                

                string ns = GetBlockNamespaceById(id);

                // 标准化方块名称（去掉命名空间，处理状态）
                size_t colonPos = blockName.find(':');
                if (colonPos != string::npos) {
                    blockName = blockName.substr(colonPos + 1);
                }

                ModelData blockModel;
                ModelData liquidModel;
                if (currentBlock.level > -1) {
                    blockModel = GetRandomModelFromCache(ns, blockName);

                    if (blockModel.vertices.empty()) {
                        // 如果是流体方块，生成流体模型
                        liquidModel = GenerateFluidModel(fluidLevels);
                        AssignFluidMaterials(liquidModel, currentBlock.name);
                        blockModel = liquidModel;
                    }
                    else
                    {
                        // 如果是流体方块，生成流体模型
                        liquidModel = GenerateFluidModel(fluidLevels);
                        AssignFluidMaterials(liquidModel, currentBlock.name);
                        
                        // 将所有面的方向设置为不剔除
                        for (auto& face : blockModel.faces)
                        {
                            face.faceDirection = FaceType::DO_NOT_CULL;
                        }

                        blockModel = MergeFluidModelData(blockModel, liquidModel);
                    }

                }
                else
                {
                    // 处理其他方块
                    blockModel = GetRandomModelFromCache(ns, blockName);
                }
                
                // 剔除被遮挡的面
                std::vector<int> validFaceIndices;
                validFaceIndices.reserve(blockModel.faces.size());

                // 遍历所有面
                for (size_t faceIdx = 0; faceIdx < blockModel.faces.size(); ++faceIdx) {
                    // 检查faceIdx是否超出范围
                    if (faceIdx >= blockModel.faces.size()) {
                        throw std::runtime_error("faceIdx out of range");
                    }

                    FaceType dir = blockModel.faces[faceIdx].faceDirection; // 获取面的方向
                    // 如果是DO_NOT_CULL，保留该面
                    if (dir == FaceType::DO_NOT_CULL) {
                        validFaceIndices.push_back(faceIdx);
                    }
                    else {
                        auto it = neighborIndexMap.find(dir);
                        if (it != neighborIndexMap.end()) {
                            int neighborIdx = it->second;
                            if (!neighbors[neighborIdx]) { // 如果邻居存在（非空气），跳过该面
                                continue;
                            }
                        }
                        validFaceIndices.push_back(faceIdx);
                    }
                }

                // 重建面数据（使用新的Face结构体）
                ModelData filteredModel;
                filteredModel.faces.reserve(validFaceIndices.size());

                for (int faceIdx : validFaceIndices) {
                    // 直接复制Face结构体
                    filteredModel.faces.push_back(blockModel.faces[faceIdx]);
                }

                // 顶点和UV数据保持不变（后续合并时会去重）
                filteredModel.vertices = blockModel.vertices;
                filteredModel.uvCoordinates = blockModel.uvCoordinates;
                filteredModel.materials = blockModel.materials;

                // 使用过滤后的模型
                blockModel = std::move(filteredModel);
            
                ApplyPositionOffset(blockModel, x, y, z);

                // 合并到主模型
                if (chunkModel.vertices.empty()) {
                    chunkModel = blockModel;
                }
                else {
                    MergeModelsDirectly(chunkModel, blockModel);
                }
            }
        }
    }

    
    auto chunkKey = std::make_pair(chunkX, chunkZ);
    {
        std::lock_guard<std::mutex> lock(entityCacheMutex);
        if (processedChunks.find(chunkKey) != processedChunks.end()) {
            return chunkModel;
        }
    }

    if (entityBlockCache.find(chunkKey) != entityBlockCache.end()) {
        const auto& entityBlocks = entityBlockCache[chunkKey];
        for (const auto& entity : entityBlocks) {
            ModelData EntityModel;
            if (entity != nullptr) {
                EntityModel = entity->GenerateModel();
                if (chunkModel.vertices.empty()) {
                    chunkModel = EntityModel;
                }
                else {
                    MergeModelsDirectly(chunkModel, EntityModel);
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(entityCacheMutex);
        processedChunks.insert(chunkKey);
    }
    return chunkModel;
}


//————————————————————————————————————————————————————————————
//——————————————————————————LOD区块逻辑————————————————————————————
//————————————————————————————————————————————————————————————

ModelData RegionModelExporter::GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize) {
    ModelData chunkModel;
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;

    int lodBlockSize = static_cast<int>(lodSize);

    for (int x = blockXStart; x < blockXStart + 16; x += lodBlockSize) {
        for (int z = blockZStart; z < blockZStart + 16; z += lodBlockSize) {
            for (int y = blockYStart; y < blockYStart + 16; y += lodBlockSize) {
                // 边界检查
                if (x < xStart || x + lodBlockSize > xEnd ||
                    z < zStart || z + lodBlockSize > zEnd ||
                    y < yStart || y + lodBlockSize > yEnd)
                    continue;

                if (config.cullCave) {
                    if (GetSkyLight(x, y, z) == -1)
                        continue;
                }
                int id = -1;
                int level=0;
                BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodBlockSize, &id, &level);
                std::vector<std::string> color = LODManager::GetBlockColor(x, y, z, id, type);
                level = (lodBlockSize - (level));
                // 如果块类型是固体
                if (type == SOLID) {
                    ModelData solidBox = LODManager::GenerateBox(x, y, z, lodBlockSize, level, color);
                    MergeModelsDirectly(chunkModel, solidBox);
                }
                if (type ==FLUID)
                {
                    ModelData solidBox = LODManager::GenerateBox(x, y, z, lodBlockSize, level, color);
                    MergeModelsDirectly(chunkModel, solidBox);
                }
                
                
            }
        }
    }
    return chunkModel;
}


