#include "RegionModelExporter.h"
#include "coord_conversion.h"
#include "objExporter.h"
#include <regex>
#include <sstream>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono
void deduplicateVertices(ModelData& data) {
    std::unordered_map<std::string, int> vertexMap;
    std::vector<float> newVertices;
    std::vector<int> indexMap;

    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        float x = data.vertices[i + 0];
        float y = data.vertices[i + 1];
        float z = data.vertices[i + 2];
        int roundedX = static_cast<int>(x * 10000 + 0.5);
        int roundedY = static_cast<int>(y * 10000 + 0.5);
        int roundedZ = static_cast<int>(z * 10000 + 0.5);
        std::string key = std::to_string(roundedX) + "," + std::to_string(roundedY) + "," + std::to_string(roundedZ);

        if (vertexMap.find(key) != vertexMap.end()) {
            indexMap.push_back(vertexMap[key]);
        } else {
            int newIndex = newVertices.size() / 3;
            vertexMap[key] = newIndex;
            newVertices.insert(newVertices.end(), {x, y, z});
            indexMap.push_back(newIndex);
        }
    }

    data.vertices = newVertices;

    // 更新面数据中的顶点索引
    for (auto& idx : data.faces) {
        idx = indexMap[idx];
    }
}

void deduplicateFaces(ModelData& data, bool checkMaterial = true) {
    // 键结构需要包含完整信息
    struct FaceKey {
        std::array<int, 4> sortedVerts;
        int materialIndex;
    };

    // 自定义相等比较谓词
    struct KeyEqual {
        bool checkMode;
        explicit KeyEqual(bool mode) : checkMode(mode) {}

        bool operator()(const FaceKey& a, const FaceKey& b) const {
            if (a.sortedVerts != b.sortedVerts) return false;
            return !checkMode || (a.materialIndex == b.materialIndex);
        }
    };

    // 自定义哈希器
    struct KeyHasher {
        bool checkMode;
        explicit KeyHasher(bool mode) : checkMode(mode) {}

        size_t operator()(const FaceKey& k) const {
            size_t seed = 0;
            if (checkMode) {
                seed ^= std::hash<int>()(k.materialIndex) + 0x9e3779b9;
            }
            for (int v : k.sortedVerts) {
                seed ^= std::hash<int>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };

    // 初始化容器时注入检查模式和比较逻辑
    using FaceMap = std::unordered_map<FaceKey, int, KeyHasher, KeyEqual>;
    FaceMap faceCount(10, KeyHasher(checkMaterial), KeyEqual(checkMaterial));

    // 第一次遍历：生成统计
    for (size_t i = 0; i < data.faces.size(); i += 4) {
        std::array<int, 4> face = {
            data.faces[i], data.faces[i + 1],
            data.faces[i + 2], data.faces[i + 3]
        };

        std::array<int, 4> sorted = face;
        std::sort(sorted.begin(), sorted.end());

        FaceKey key{ sorted, checkMaterial ? data.materialIndices[i / 4] : -1 };
        faceCount[key]++;
    }

    // 第二次遍历：过滤数据
    std::vector<int> newFaces, newUvFaces, newMaterials;
    for (size_t i = 0; i < data.faces.size(); i += 4) {
        std::array<int, 4> face = {
            data.faces[i], data.faces[i + 1],
            data.faces[i + 2], data.faces[i + 3]
        };

        std::array<int, 4> sorted = face;
        std::sort(sorted.begin(), sorted.end());

        FaceKey key{ sorted, checkMaterial ? data.materialIndices[i / 4] : -1 };

        if (faceCount[key] == 1) {
            newFaces.insert(newFaces.end(), face.begin(), face.end());
            newUvFaces.insert(newUvFaces.end(),
                data.uvFaces.begin() + i,
                data.uvFaces.begin() + i + 4);
            newMaterials.push_back(data.materialIndices[i / 4]);
        }
    }

    data.faces.swap(newFaces);
    data.uvFaces.swap(newUvFaces);
    data.materialIndices.swap(newMaterials);
}
void RegionModelExporter::ExportRegionModels(int xStart, int xEnd, int yStart, int yEnd,
    int zStart, int zEnd, const string& outputName) {
    // 收集区域内所有唯一方块ID
    LoadChunks(xStart, xEnd, yStart, yEnd, zStart, zEnd);

    auto blocks = GetGlobalBlockPalette();

    // 使用 ProcessBlockstateForBlocks 处理所有方块状态模型
    ProcessBlockstateForBlocks(blocks);

    // 获取区域内的所有区块范围（按16x16x16划分）
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd, sectionYStart, sectionYEnd;
    blockToChunk(xStart, zStart, chunkXStart, chunkZStart);
    blockToChunk(xEnd, zEnd, chunkXEnd, chunkZEnd);
    blockYToSectionY(yStart, sectionYStart);
    blockYToSectionY(yEnd, sectionYEnd);
    auto start = high_resolution_clock::now();  // 新增：开始时间点
    // 遍历每个区块
    ModelData finalMergedModel;
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                // 生成当前区块的子模型
                ModelData chunkModel = GenerateChunkModel(chunkX, sectionY, chunkZ);
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
    auto end = high_resolution_clock::now();  // 新增：结束时间点
    auto duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "模型合并耗时: " << duration.count() << " ms" << endl;  // 新增：输出到控制台
    start = high_resolution_clock::now();  // 新增：开始时间点
    deduplicateVertices(finalMergedModel);
    end = high_resolution_clock::now();  // 新增：结束时间点
    duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "deduplicateVertices: " << duration.count() << " ms" << endl;  // 新增：输出到控制台

    start = high_resolution_clock::now();  // 新增：开始时间点
    // 严格模式：材质+顶点都相同才剔除
    deduplicateFaces(finalMergedModel, true);

    // 宽松模式：仅顶点相同即剔除
    //deduplicateFaces(finalMergedModel, false);

    end = high_resolution_clock::now();  // 新增：结束时间点
    duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "deduplicateFaces: " << duration.count() << " ms" << endl;  // 新增：输出到控制台
    // 导出最终模型
    if (!finalMergedModel.vertices.empty()) {
        CreateModelFiles(finalMergedModel, outputName);
    }
}
ModelData RegionModelExporter::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    ModelData chunkModel;
    // 计算区块内的方块范围
    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;
    // 遍历区块内的每个方块
    for (int x = blockXStart; x < blockXStart + 16; ++x) {
        for (int y = blockYStart; y < blockYStart + 16; ++y) {
            for (int z = blockZStart; z < blockZStart + 16; ++z) {
                bool neighbors[6];
                int id = GetBlockIdWithNeighbors(x, y, z, neighbors);
                string blockName = GetBlockNameById(id);
                string ns = GetBlockNamespaceById(id);

                if (blockName == "minecraft:air") continue;

                // 标准化方块名称（去掉命名空间，处理状态）
                size_t colonPos = blockName.find(':');
                if (colonPos != string::npos) {
                    blockName = blockName.substr(colonPos + 1);
                }


                // 查找预处理的模型
                if (BlockModelCache.find(ns) == BlockModelCache.end()) continue;
                if (BlockModelCache[ns].find(blockName) == BlockModelCache[ns].end()) continue;
                // 创建模型副本并应用位置偏移
                ModelData blockModel = BlockModelCache[ns][blockName];

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

void RegionModelExporter::LoadChunks(int xStart, int xEnd, int yStart, int yEnd, int zStart, int zEnd) {
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
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                LoadCacheBlockDataAutomatically(chunkX, chunkZ, sectionY);
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