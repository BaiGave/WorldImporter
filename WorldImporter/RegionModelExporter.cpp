#include "RegionModelExporter.h"
#include "coord_conversion.h"
#include "objExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "fluid.h"
#include "LODManager.h"
#include "texture.h"
#include <iomanip>  // 用于 std::setw 和 std::setfill
#include <sstream>  // 用于 std::ostringstream
#include <regex>
#include <tuple>
#include <future>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include "EntityBlock.h"
#include "ModelDeduplicator.h"
#include "chunk_group_allocator.h"

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono

// 自定义哈希函数，用于std::pair<int, int>
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator ()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        return h1 ^ (h2 << 1);
    }
};

// 自定义哈希函数，用于std::pair<int, int, int>
struct triple_hash {
    template <class T1, class T2, class T3>
    std::size_t operator ()(const std::tuple<T1, T2, T3>& t) const {
        auto h1 = std::hash<T1>{}(std::get<0>(t));
        auto h2 = std::hash<T2>{}(std::get<1>(t));
        auto h3 = std::hash<T3>{}(std::get<2>(t));
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};


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
        };

    if (config.useChunkPrecision) {
        config.minX = alignTo16(xStart); config.maxX = alignTo16(xEnd);
        config.minY = alignTo16(yStart); config.maxY = alignTo16(yEnd);
        config.minZ = alignTo16(zStart); config.maxZ = alignTo16(zEnd);
    }

    // 预处理阶段
    RegisterFluidTextures();

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
        return (task.lodLevel == 0.0f)
            ? GenerateChunkModel(task.chunkX, task.sectionY, task.chunkZ)
            : GenerateLODChunkModel(task.chunkX, task.sectionY, task.chunkZ, task.lodLevel);
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
    std::vector<std::future<void>> futures;
    

    for (const auto& group : chunkGroups) {
        futures.push_back(std::async(std::launch::async, [&, group]() {
            ModelData groupModel;
            std::unordered_map<string, string> localMaterials;

            // 合并组内所有区块模型
            for (const auto& task : group.tasks) {
                ModelData chunkModel = processModel(task);
                if (groupModel.vertices.empty()) {
                    groupModel = std::move(chunkModel);
                }
                else {
                    MergeModelsDirectly(groupModel, chunkModel);
                }
            }

            if (groupModel.vertices.empty()) return;

            if (config.exportFullModel) {
                // 合并到完整模型（线程安全）
                mergeToFinalModel(std::move(groupModel));
            }
            else {
                DeduplicateModel(groupModel);

                const string groupFileName = outputName +
                    "_x" + to_string(group.startX) +
                    "_z" + to_string(group.startZ);

                // 创建模型文件并收集材质
                CreateMultiModelFiles(groupModel, groupFileName, localMaterials, outputName);
                recordMaterials(localMaterials);
            }
            }));
    }

    // 等待所有线程完成
    for (auto& future : futures) {
        future.wait();
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
    // 加载所有相关的分块和分段
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            LoadAndCacheBlockData(chunkX, chunkZ);
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                int distance = sqrt((chunkX - config.LODCenterX) * (chunkX - config.LODCenterX) +
                    (chunkZ - config.LODCenterZ) * (chunkZ - config.LODCenterZ));
                float chunkLOD = 1.0f;
                if (distance <= LOD0renderDistance) {
                    chunkLOD = 0.0f;
                }
                else if (distance <= LOD1renderDistance) {
                    chunkLOD = 1.0f;
                }
                else if (distance <= LOD2renderDistance) {
                    chunkLOD = 2.0f;
                }
                else if (distance <= LOD3renderDistance) {
                    chunkLOD = 4.0f;
                }
                else {
                    chunkLOD = 8.0f;
                }
                g_chunkLODs[std::make_tuple(chunkX, sectionY, chunkZ)] = chunkLOD;
            }
        }
    }
}


ModelData RegionModelExporter::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    ModelData chunkModel;
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
            int currentY = GetHeightMapY(x, z, "WORLD_SURFACE")-64;
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
                        
                        if (!blockModel.faceDirections.empty())
                        {
                            for (size_t i = 0; i < blockModel.faceDirections.size(); i += 4)
                            {
                                blockModel.faceDirections[i] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 1] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 2] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 3] = "DO_NOT_CULL";
                            }
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
                const std::unordered_map<std::string, int> directionToNeighborIndex = {
                    {"down", 1},  // 假设neighbors[1]对应下方
                    {"up", 0},    // neighbors[0]对应上方
                    {"north", 4}, // neighbors[4]对应北
                    {"south", 5}, // neighbors[5]对应南
                    {"west", 2},  // neighbors[2]对应西
                    {"east", 3}   // neighbors[3]对应东
                };

                // 检查faceDirections是否已初始化
                if (blockModel.faceDirections.empty()) {
                    continue;
                }

                // 检查faces大小是否为4的倍数
                if (blockModel.faces.size() % 4 != 0) {
                    throw std::runtime_error("faces size is not a multiple of 4");
                }

                // 遍历所有面（每4个顶点索引构成一个面）
                for (size_t faceIdx = 0; faceIdx < blockModel.faces.size() / 4; ++faceIdx) {
                    // 检查faceIdx是否超出范围
                    if (faceIdx * 4 >= blockModel.faceDirections.size()) {
                        throw std::runtime_error("faceIdx out of range");
                    }

                    std::string dir = blockModel.faceDirections[faceIdx * 4]; // 取第一个顶点的方向
                    // 如果是 "DO_NOT_CULL"，保留该面
                    if (dir == "DO_NOT_CULL") {
                        validFaceIndices.push_back(faceIdx);
                    }
                    else {
                        auto it = directionToNeighborIndex.find(dir);
                        if (it != directionToNeighborIndex.end()) {
                            int neighborIdx = it->second;
                            if (!neighbors[neighborIdx]) { // 如果邻居存在（非空气），跳过该面
                                continue;
                            }
                        }
                        validFaceIndices.push_back(faceIdx);
                    }

                }

                // 重建面数据（顶点、UV、材质）
                ModelData filteredModel;
                for (int faceIdx : validFaceIndices) {
                    // 提取原面数据（4个顶点索引）
                    for (int i = 0; i < 4; ++i) {
                        filteredModel.faces.push_back(blockModel.faces[faceIdx * 4 + i]);
                        filteredModel.uvFaces.push_back(blockModel.uvFaces[faceIdx * 4 + i]);
                    }
                    // 材质索引
                    filteredModel.materialIndices.push_back(blockModel.materialIndices[faceIdx]);
                    // 方向记录（每个顶点重复方向，这里仅记录一次）
                    filteredModel.faceDirections.push_back(blockModel.faceDirections[faceIdx * 4]);
                }

                // 顶点和UV数据保持不变（后续合并时会去重）
                filteredModel.vertices = blockModel.vertices;
                filteredModel.uvCoordinates = blockModel.uvCoordinates;
                filteredModel.materialNames = blockModel.materialNames;
                filteredModel.texturePaths = blockModel.texturePaths;

                // 使用过滤后的模型
                blockModel = filteredModel;
            

                
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


