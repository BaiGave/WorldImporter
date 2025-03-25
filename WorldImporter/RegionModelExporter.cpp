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
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include "EntityBlock.h"

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


void deduplicateVertices(ModelData& data) {
    std::unordered_map<VertexKey, int> vertexMap;
    // 预先分配容量，避免多次rehash
    vertexMap.reserve(data.vertices.size() / 3);
    std::vector<float> newVertices;
    newVertices.reserve(data.vertices.size());
    std::vector<int> indexMap(data.vertices.size() / 3);

    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        float x = data.vertices[i];
        float y = data.vertices[i + 1];
        float z = data.vertices[i + 2];
        // 保留四位小数（转为整数后再比较）
        int rx = static_cast<int>(x * 10000 + 0.5f);
        int ry = static_cast<int>(y * 10000 + 0.5f);
        int rz = static_cast<int>(z * 10000 + 0.5f);
        VertexKey key{ rx, ry, rz };

        auto it = vertexMap.find(key);
        if (it != vertexMap.end()) {
            indexMap[i / 3] = it->second;
        }
        else {
            int newIndex = newVertices.size() / 3;
            vertexMap[key] = newIndex;
            newVertices.push_back(x);
            newVertices.push_back(y);
            newVertices.push_back(z);
            indexMap[i / 3] = newIndex;
        }
    }

    data.vertices = std::move(newVertices);

    // 更新面数据中的顶点索引
    for (auto& idx : data.faces) {
        idx = indexMap[idx];
    }
}

void deduplicateUV(ModelData& model) {
    // 如果没有 UV 坐标，则直接返回
    if (model.uvCoordinates.empty()) {
        return;
    }

    // 使用哈希表记录每个唯一 UV 对应的新索引
    std::unordered_map<UVKey, int> uvMap;
    std::vector<float> newUV;  // 存储去重后的 UV 坐标（每两个元素构成一组）
    // 原始 UV 数组中组的数量（每组有2个元素：u,v）
    int uvCount = model.uvCoordinates.size() / 2;
    // 建立一个映射表，从旧的 UV 索引到新的 UV 索引
    std::vector<int> indexMapping(uvCount, -1);

    for (int i = 0; i < uvCount; i++) {
        float u = model.uvCoordinates[i * 2];
        float v = model.uvCoordinates[i * 2 + 1];
        // 将浮点数转换为整数，保留小数点后6位的精度
        int iu = static_cast<int>(std::round(u * 1000000));
        int iv = static_cast<int>(std::round(v * 1000000));
        UVKey key = { iu, iv };

        auto it = uvMap.find(key);
        if (it == uvMap.end()) {
            // 如果没有找到，则是新 UV，记录新的索引
            int newIndex = newUV.size() / 2;
            uvMap[key] = newIndex;
            newUV.push_back(u);
            newUV.push_back(v);
            indexMapping[i] = newIndex;
        }
        else {
            // 如果已存在，则记录已有的新索引
            indexMapping[i] = it->second;
        }
    }

    // 如果 uvFaces 不为空，则更新 uvFaces 中的索引
    if (!model.uvFaces.empty()) {
        for (int& idx : model.uvFaces) {
            // 注意：这里假设 uvFaces 中的索引都在有效范围内
            idx = indexMapping[idx];
        }
    }

    // 替换掉原有的 uvCoordinates
    model.uvCoordinates = std::move(newUV);
}

void deduplicateFaces(ModelData& data) {
    size_t faceCountNum = data.faces.size() / 4;
    std::vector<FaceKey> keys;
    keys.reserve(faceCountNum);

    // 第一次遍历：计算每个面的规范化键并存入数组（避免重复排序）
    for (size_t i = 0; i < data.faces.size(); i += 4) {
        std::array<int, 4> face = {
            data.faces[i], data.faces[i + 1],
            data.faces[i + 2], data.faces[i + 3]
        };
        std::array<int, 4> sorted = face;
        std::sort(sorted.begin(), sorted.end());
        int matIndex = config.strictDeduplication ? data.materialIndices[i / 4] : -1;
        keys.push_back(FaceKey{ sorted, matIndex });
    }

    // 使用预分配容量的 unordered_map 来统计每个 FaceKey 的出现次数
    std::unordered_map<FaceKey, int, FaceKeyHasher> freq;
    freq.reserve(faceCountNum);
    for (const auto& key : keys) {
        freq[key]++;
    }

    // 第二次遍历：过滤只出现一次的面
    std::vector<int> newFaces;
    newFaces.reserve(data.faces.size());
    std::vector<int> newUvFaces;
    newUvFaces.reserve(data.uvFaces.size());
    std::vector<int> newMaterials;
    newMaterials.reserve(data.materialIndices.size());

    for (size_t i = 0; i < keys.size(); i++) {
        if (freq[keys[i]] == 1) {
            size_t base = i * 4;
            newFaces.insert(newFaces.end(),
                data.faces.begin() + base,
                data.faces.begin() + base + 4);
            newUvFaces.insert(newUvFaces.end(),
                data.uvFaces.begin() + base,
                data.uvFaces.begin() + base + 4);
            newMaterials.push_back(data.materialIndices[i]);
        }
    }

    data.faces.swap(newFaces);
    data.uvFaces.swap(newUvFaces);
    data.materialIndices.swap(newMaterials);
}

void RegionModelExporter::ExportRegionModels(const string& outputName) {
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

    if (config.useChunkPrecision) {
        auto alignTo16 = [](int value) -> int {
            if (value % 16 == 0)
                return value;
            if (value > 0)
                return ((value + 15) / 16) * 16;
            else
                return ((value - 15) / 16) * 16;
            };

        config.minX = alignTo16(xStart);
        config.maxX = alignTo16(xEnd);
        config.minY = alignTo16(yStart);
        config.maxY = alignTo16(yEnd);
        config.minZ = alignTo16(zStart);
        config.maxZ = alignTo16(zEnd);
    }

    RegisterFluidTextures();

    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd, sectionYStart, sectionYEnd;
    blockToChunk(xStart, zStart, chunkXStart, chunkZStart);
    blockToChunk(xEnd, zEnd, chunkXEnd, chunkZEnd);
    blockYToSectionY(yStart, sectionYStart);
    blockYToSectionY(yEnd, sectionYEnd);
    // 计算中心坐标
    int centerX = (chunkXStart + chunkXEnd) / 2;
    int centerZ = (chunkZStart + chunkZEnd) / 2;
    LoadChunks();

    UpdateSkyLightNeighborFlags();
    auto blocks = GetGlobalBlockPalette();
    ProcessBlockstateForBlocks(blocks);

    auto biomeMap = Biome::GenerateBiomeMap(xStart, zStart, xEnd, zEnd);
    // 导出图片
    Biome::ExportToPNG(biomeMap, "foliage.png", BiomeColorType::Foliage);
    Biome::ExportToPNG(biomeMap, "water.png", BiomeColorType::Water);
    Biome::ExportToPNG(biomeMap, "grass.png", BiomeColorType::Grass);
    Biome::ExportToPNG(biomeMap, "dryfoliage.png", BiomeColorType::DryFoliage);
    Biome::ExportToPNG(biomeMap, "waterFog.png", BiomeColorType::WaterFog);
    Biome::ExportToPNG(biomeMap, "fog.png", BiomeColorType::Fog);
    Biome::ExportToPNG(biomeMap, "sky.png", BiomeColorType::Sky);

    
    // 定义半径范围（可以根据需要调整）
    int LOD0renderDistance = config.LOD0renderDistance;
    int LOD1renderDistance = config.LOD1renderDistance+ LOD0renderDistance;
    int LOD2renderDistance = config.LOD2renderDistance+ LOD1renderDistance;
    int LOD3renderDistance = config.LOD3renderDistance+ LOD2renderDistance;

    ModelData finalMergedModel;


    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                // 计算当前区块的中心坐标
                int currentCenterX = chunkX;
                int currentCenterZ = chunkZ;

                // 计算与中心点的距离
                int distance = sqrt(
                    (currentCenterX - centerX) * (currentCenterX - centerX) +
                    (currentCenterZ - centerZ) * (currentCenterZ - centerZ)
                );
                ModelData chunkModel;
                // 根据距离选择生成方法
                if (distance <= LOD0renderDistance) {
                    chunkModel = GenerateChunkModel(chunkX, sectionY, chunkZ);
                }
                else if (distance <= LOD1renderDistance) {
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 1.0f);
                }
                else if (distance <= LOD2renderDistance) {
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 2.0f);
                }
                else if (distance <= LOD3renderDistance) {
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 4.0f);
                }
                else
                {
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 8.0f);
                }


                // 合并到总模型
                if (finalMergedModel.vertices.empty()) {
                    finalMergedModel = chunkModel;
                }
                else {
                    MergeModelsDirectly(finalMergedModel, chunkModel);
                }
            }
        }
    }
    
    
    deduplicateVertices(finalMergedModel);
    deduplicateUV(finalMergedModel);
    deduplicateFaces(finalMergedModel);


    if (!finalMergedModel.vertices.empty()) {
        CreateModelFiles(finalMergedModel, outputName);
        
    }
}

void RegionModelExporter::LoadChunks() {
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

    // 定义半径范围（可以根据需要调整）
    int LOD0renderDistance = config.LOD0renderDistance;
    int LOD1renderDistance = config.LOD1renderDistance + LOD0renderDistance;
    int LOD2renderDistance = config.LOD2renderDistance + LOD1renderDistance;
    int LOD3renderDistance = config.LOD3renderDistance + LOD2renderDistance;

    // 计算最小和最大坐标，以处理范围颠倒的情况
    int min_x = min(xStart, xEnd);
    int max_x = max(xStart, xEnd);
    int min_z = min(zStart, zEnd);
    int max_z = max(zStart, zEnd);
    int min_y = min(yStart, yEnd);
    int max_y = max(yStart, yEnd);

    // 计算分块的范围（每个分块宽度为 16 块）
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd;
    blockToChunk(min_x, min_z, chunkXStart, chunkZStart);
    blockToChunk(max_x, max_z, chunkXEnd, chunkZEnd);
    // 计算中心坐标
    int centerX = (chunkXStart + chunkXEnd) / 2;
    int centerZ = (chunkZStart + chunkZEnd) / 2;
    // 处理可能的负数和范围计算
    chunkXStart = floor((float)min_x / 16.0f);
    chunkXEnd = ceil((float)max_x / 16.0f);
    chunkZStart = floor((float)min_z / 16.0f);
    chunkZEnd = ceil((float)max_z / 16.0f);

    // 计算分段 Y 范围（每个分段高度为 16 块）
    int sectionYStart, sectionYEnd;
    blockYToSectionY(min_y, sectionYStart);
    blockYToSectionY(max_y, sectionYEnd);
    sectionYStart = static_cast<int>(floor((float)min_y / 16.0f));
    sectionYEnd = static_cast<int>(ceil((float)max_y / 16.0f));

    // 加载所有相关的分块和分段
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            LoadAndCacheBlockData(chunkX, chunkZ);
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                int distance = sqrt((chunkX - centerX) * (chunkX - centerX) +
                    (chunkZ - centerZ) * (chunkZ - centerZ));
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
                else
                {
                    chunkLOD = 8.0f;
                }
                g_chunkLODs[std::make_tuple(chunkX, sectionY, chunkZ)] = chunkLOD;
            }
        }
    }
}

void RegionModelExporter::ApplyPositionOffset(ModelData& model, int x, int y, int z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
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


