// ChunkGenerator.cpp
#include "ChunkGenerator.h"
#include "RegionModelExporter.h"
#include "locutil.h"
#include "ObjExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "model.h"
#include "Fluid.h"
#include "LODManager.h"
#include "texture.h"
#include <iomanip>
#include <sstream>
#include <regex>
#include <tuple>
#include <future>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include "ModelDeduplicator.h"
#include "ChunkGroupAllocator.h"
#include <utility>
#include "hashutils.h"
#include "ChunkLoader.h"

using namespace std;
using namespace std::chrono;

std::unordered_set<std::pair<int, int>, pair_hash> processedChunks;
std::mutex entityCacheMutex; // 互斥量,确保线程安全

ModelData ChunkGenerator::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    // 从RegionModelExporter.cpp中复制GenerateChunkModel的实现
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

                int id = GetBlockIdWithNeighbors(x, y, z,neighbors.data(),fluidLevels.data());
                Block currentBlock = GetBlockById(id);
                string blockName = currentBlock.GetModifiedNameWithNamespace();
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
                

                string ns = currentBlock.GetNamespace();

                // 标准化方块名称(去掉命名空间,处理状态)
                size_t colonPos = blockName.find(':');
                if (colonPos != string::npos) {
                    blockName = blockName.substr(colonPos + 1);
                }

                ModelData blockModel;
                ModelData liquidModel;
                if (currentBlock.level > -1) {
                    blockModel = GetRandomModelFromCache(ns, blockName);

                    if (blockModel.vertices.empty()) {
                        // 如果是流体方块,生成流体模型
                        liquidModel = GenerateFluidModel(fluidLevels);
                        AssignFluidMaterials(liquidModel, currentBlock.name);
                        blockModel = liquidModel;
                    }
                    else
                    {
                        // 如果是流体方块,生成流体模型
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
                    // 如果是DO_NOT_CULL,保留该面
                    if (dir == FaceType::DO_NOT_CULL) {
                        validFaceIndices.push_back(faceIdx);
                    }
                    else {
                        auto it = neighborIndexMap.find(dir);
                        if (it != neighborIndexMap.end()) {
                            int neighborIdx = it->second;
                            if (!neighbors[neighborIdx]) { // 如果邻居存在(非空气),跳过该面
                                continue;
                            }
                        }
                        validFaceIndices.push_back(faceIdx);
                    }
                }

                // 重建面数据(使用新的Face结构体)
                ModelData filteredModel;
                filteredModel.faces.reserve(validFaceIndices.size());

                for (int faceIdx : validFaceIndices) {
                    // 直接复制Face结构体
                    filteredModel.faces.push_back(blockModel.faces[faceIdx]);
                }

                // 顶点和UV数据保持不变(后续合并时会去重)
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

    if (EntityBlockCache.find(chunkKey) != EntityBlockCache.end()) {
        const auto& entityBlocks = EntityBlockCache[chunkKey];
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

ModelData ChunkGenerator::GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize) {
    // 从RegionModelExporter.cpp中复制GenerateLODChunkModel的实现
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