#include "RegionModelExporter.h"
#include "coord_conversion.h"
#include "objExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "fluid.h"
#include "texture.h"
#include <iomanip>  // 用于 std::setw 和 std::setfill
#include <sstream>  // 用于 std::ostringstream
#include <regex>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include "EntityBlock.h"

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono


// 缓存方块ID到颜色的映射
std::unordered_map<int, std::string> blockColorCache;

std::string GetBlockAverageColor(int blockId, Block currentBlock, int x, int y, int z, float gamma = 2.0f) {
    std::string ns = GetBlockNamespaceById(blockId);
    std::string blockName = GetBlockNameById(blockId);
    // 标准化方块名称（去掉命名空间，处理状态）
    size_t colonPos = blockName.find(':');
    if (colonPos != std::string::npos) {
        blockName = blockName.substr(colonPos + 1);
    }
    ModelData blockModel;
    if (currentBlock.level > -1) {
        AssignFluidMaterials(blockModel, currentBlock.name);
    }
    else {
        // 处理其他方块
        blockModel = GetRandomModelFromCache(ns, blockName);
    }

    // 先获取纹理图片的平均颜色（格式："r g b"）
    std::string textureAverage;
    if (blockColorCache.find(blockId) != blockColorCache.end()) {
        textureAverage = blockColorCache[blockId];
    }
    else {
        std::string texturePath;
        if (!blockModel.texturePaths.empty()) {
            texturePath = blockModel.texturePaths[0];
        }
        // 默认值：当纹理加载失败或无纹理路径时使用
        float r = 0.5f, g = 0.5f, b = 0.5f;
        if (!texturePath.empty()) {
            char buffer[MAX_PATH];
            GetModuleFileNameA(NULL, buffer, MAX_PATH);
            std::string exePath(buffer);
            size_t pos = exePath.find_last_of("\\/");
            std::string exeDir = exePath.substr(0, pos);
            texturePath = exeDir + "//" + texturePath;

            int width, height, channels;
            unsigned char* data = stbi_load(texturePath.c_str(), &width, &height, &channels, 0);
            if (data) {
                float sumR = 0, sumG = 0, sumB = 0;
                int validPixelCount = 0;
                int totalPixelCount = width * height;
                for (int i = 0; i < totalPixelCount; ++i) {
                    // 若有 alpha 通道且该像素 alpha 为 0，则跳过
                    if (channels >= 4 && data[i * channels + 3] == 0)
                        continue;
                    // 先将原始 sRGB 值转换到 [0,1]
                    float r_s = data[i * channels] / 255.0f;
                    float g_s = data[i * channels + 1] / 255.0f;
                    float b_s = data[i * channels + 2] / 255.0f;
                    // sRGB 转换到线性空间
                    float r_lin = (r_s <= 0.04045f) ? (r_s / 12.92f) : pow((r_s + 0.055f) / 1.055f, 2.4f);
                    float g_lin = (g_s <= 0.04045f) ? (g_s / 12.92f) : pow((g_s + 0.055f) / 1.055f, 2.4f);
                    float b_lin = (b_s <= 0.04045f) ? (b_s / 12.92f) : pow((b_s + 0.055f) / 1.055f, 2.4f);
                    sumR += r_lin;
                    sumG += g_lin;
                    sumB += b_lin;
                    validPixelCount++;
                }
                if (validPixelCount > 0) {
                    float avgR_lin = sumR / validPixelCount;
                    float avgG_lin = sumG / validPixelCount;
                    float avgB_lin = sumB / validPixelCount;

                    // 应用伽马校正（降低伽马）
                    avgR_lin = pow(avgR_lin, gamma);
                    avgG_lin = pow(avgG_lin, gamma);
                    avgB_lin = pow(avgB_lin, gamma);

                    // 线性空间转换回 sRGB
                    r = (avgR_lin <= 0.0031308f) ? (avgR_lin * 12.92f) : (1.055f * pow(avgR_lin, 1.0f / 2.4f) - 0.055f);
                    g = (avgG_lin <= 0.0031308f) ? (avgG_lin * 12.92f) : (1.055f * pow(avgG_lin, 1.0f / 2.4f) - 0.055f);
                    b = (avgB_lin <= 0.0031308f) ? (avgB_lin * 12.92f) : (1.055f * pow(avgB_lin, 1.0f / 2.4f) - 0.055f);
                }
                stbi_image_free(data);
            }
        }
        char avgColorStr[64];
        snprintf(avgColorStr, sizeof(avgColorStr), "%.3f %.3f %.3f", r, g, b);
        textureAverage = avgColorStr;
        // 仅缓存图片的平均颜色
        blockColorCache[blockId] = textureAverage;
    }

    // 如果需要群系颜色混合，则每次都进行混合计算，不缓存混合后的结果
    if (blockModel.tintindex != -1) {
        // 解析缓存的图片平均颜色
        float textureR, textureG, textureB;
        sscanf(textureAverage.c_str(), "%f %f %f", &textureR, &textureG, &textureB);
        uint32_t hexColor;
        // 获取当前坐标的群系颜色（十六进制），转换为 0-1 范围的 RGB
        if (blockModel.tintindex == 2) {
            hexColor = Biome::GetColor(GetBiomeId(x, y, z), BiomeColorType::Water);
        }
        else {
            hexColor = Biome::GetColor(GetBiomeId(x, y, z), BiomeColorType::Foliage);
        }

        float biomeR = ((hexColor >> 16) & 0xFF) / 255.0f;
        float biomeG = ((hexColor >> 8) & 0xFF) / 255.0f;
        float biomeB = (hexColor & 0xFF) / 255.0f;

        // 正片叠底混合（乘法混合）：各通道相乘
        float finalR = biomeR * textureR;
        float finalG = biomeG * textureG;
        float finalB = biomeB * textureB;

        char blendedColorStr[128];
        snprintf(blendedColorStr, sizeof(blendedColorStr), "color#%.3f %.3f %.3f", finalR, finalG, finalB);
        return std::string(blendedColorStr);
    }
    else {
        // 不需要群系混合，直接返回并缓存纹理图片的平均颜色
        char finalColorStr[128];
        snprintf(finalColorStr, sizeof(finalColorStr), "color#%s", textureAverage.c_str());
        return std::string(finalColorStr);
    }
}

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
    auto start = high_resolution_clock::now();
    LoadChunks();
    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    std::cout << "LoadChunks耗时: " << duration.count() << " ms" << std::endl;

    UpdateSkyLightNeighborFlags();
    auto blocks = GetGlobalBlockPalette();
    ProcessBlockstateForBlocks(blocks);

    

    
    // 定义半径范围（可以根据需要调整）
    int radius = 6; // 半径为16个区块

    ModelData finalMergedModel;

    start = high_resolution_clock::now();
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
                if (distance <= radius) {
                    // 距离中心在 radius 范围内，使用高精度生成
                    chunkModel = GenerateChunkModel(chunkX, sectionY, chunkZ);
                }
                else if (distance <= radius * 2) {
                    // 距离在 radius ~ 2*radius 之间，LOD参数为1.0f
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 1.0f);
                }
                else if (distance <= radius * 4) {
                    // 距离在 2*radius ~ 4*radius 之间，LOD参数为2.0f
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 2.0f);
                }
                else {
                    // 距离超过 4*radius，使用最低精度生成，LOD参数为4.0f
                    chunkModel = GenerateLODChunkModel(chunkX, sectionY, chunkZ, 4.0f);
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
    end = high_resolution_clock::now();
    duration = duration_cast<milliseconds>(end - start);
    cout << "模型合并耗时: " << duration.count() << " ms" << endl;

    start = high_resolution_clock::now();
    deduplicateVertices(finalMergedModel);
    deduplicateUV(finalMergedModel);
    end = high_resolution_clock::now();
    duration = duration_cast<milliseconds>(end - start);
    cout << "deduplicateVertices: " << duration.count() << " ms" << endl;

    start = high_resolution_clock::now();
    deduplicateFaces(finalMergedModel);
    end = high_resolution_clock::now();
    duration = duration_cast<milliseconds>(end - start);
    cout << "deduplicateFaces: " << duration.count() << " ms" << endl;


    if (!finalMergedModel.vertices.empty()) {
        CreateModelFiles(finalMergedModel, outputName);
        auto biomeMap = Biome::GenerateBiomeMap(xStart, zStart, xEnd, zEnd);
        // 导出图片
        Biome::ExportToPNG(biomeMap, "foliage.png",BiomeColorType::Foliage);
        Biome::ExportToPNG(biomeMap, "water.png", BiomeColorType::Water);
        Biome::ExportToPNG(biomeMap, "grass.png", BiomeColorType::Grass);
        Biome::ExportToPNG(biomeMap, "dryfoliage.png", BiomeColorType::DryFoliage);
        Biome::ExportToPNG(biomeMap, "waterFog.png", BiomeColorType::WaterFog);
        Biome::ExportToPNG(biomeMap, "fog.png", BiomeColorType::Fog);
        Biome::ExportToPNG(biomeMap, "sky.png", BiomeColorType::Sky);
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

ModelData RegionModelExporter::GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize) {
    ModelData chunkModel;
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

    // 计算区块内的方块范围（这里以 16 为例）
    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;

    // 辅助 lambda 判断坐标是否在检测范围内
    auto inRange = [&](int cx, int cy, int cz) -> bool {
        return (cx >= xStart && cx < xEnd &&
            cy >= yStart && cy < yEnd &&
            cz >= zStart && cz < zEnd);
        };

    // 辅助 lambda 判断指定位置是否为空气
    auto isAir = [&](int cx, int cy, int cz) -> bool {
        int id = GetBlockId(cx, cy, cz);
        Block b = GetBlockById(id);
        return (b.air);
        };

    // 遍历区块内每个大区域（注意：这里以 lodSize 为步长）
    for (int x = blockXStart; x < blockXStart + 16; x += (int)lodSize) {
        for (int z = blockZStart; z < blockZStart + 16; z += (int)lodSize) {
            for (int y = blockYStart; y < blockYStart + 16; y += (int)lodSize) {
                // 检查大区域是否在导出区域内
                if (x < xStart || x + (int)lodSize > xEnd ||
                    y < yStart || y + (int)lodSize > yEnd ||
                    z < zStart || z + (int)lodSize > zEnd) {
                    continue;
                }
                if (config.cullCave && GetSkyLight(x, y, z) == -1)
                    continue;

                // 用局部坐标表示，初始范围设为整个区域
                int localMinX = (int)lodSize, localMaxX = 0;
                int localMinY = (int)lodSize, localMaxY = 0;
                int localMinZ = (int)lodSize, localMaxZ = 0;
                std::string color; // 记录第一个非空气块的颜色
                bool isFluidBlock = false; // 标记是否为流体块

                for (int dx = 0; dx < (int)lodSize; ++dx) {
                    for (int dy = 0; dy < (int)lodSize; ++dy) {
                        for (int dz = 0; dz < (int)lodSize; ++dz) {
                            int currentX = x + dx;
                            int currentY = y + dy;
                            int currentZ = z + dz;
                            // 超出检测范围视为空气
                            if (!inRange(currentX, currentY, currentZ))
                                continue;
                            if (config.cullCave && GetSkyLight(currentX, currentY, currentZ) == -1)
                                continue;
                            int id = GetBlockId(currentX, currentY, currentZ);
                            Block currentBlock = GetBlockById(id);
                            std::string currentBaseName = currentBlock.GetNameAndNameSpaceWithoutState();

                            bool isFluid = fluidDefinitions.find(currentBaseName) != fluidDefinitions.end();
                            bool hasFluidData = (currentBlock.level != -1);

                            if (currentBlock.name != "minecraft:air") {
                                localMinX = std::fmin(localMinX, dx);
                                localMaxX = std::fmax(localMaxX, dx + 1); // 注意：max 记录的是最后非空气块后的位置
                                localMinY = std::fmin(localMinY, dy);
                                localMaxY = std::fmax(localMaxY, dy + 1);
                                localMinZ = std::fmin(localMinZ, dz);
                                localMaxZ = std::fmax(localMaxZ, dz + 1);
                                if (color.empty())
                                    color = GetBlockAverageColor(id, currentBlock, currentX, currentY, currentZ);

                                // 判断是否为流体块
                                if (isFluid && hasFluidData) {
                                    isFluidBlock = true;
                                }
                            }
                        }
                    }
                }
                // 此时 [localMinX, localMaxX), [localMinY, localMaxY), [localMinZ, localMaxZ) 就是包含所有非空气块的最小包围盒

                // 根据包围盒检测各个面的可见性（检测相邻区域是否为空气）
                bool topVisible = false;
                for (int dx = localMinX; dx < localMaxX && !topVisible; ++dx) {
                    for (int dz = localMinZ; dz < localMaxZ && !topVisible; ++dz) {
                        int tx = x + dx;
                        int ty = y + localMaxY; // 顶面外侧
                        int tz = z + dz;
                        if (!inRange(tx, ty, tz)) {
                            if (!config.keepBoundary) {
                                topVisible = false;
                                goto topFaceCheckEnd;
                            }
                            else {
                                topVisible = true;
                                break;
                            }
                        }
                        if (isAir(tx, ty, tz)) {
                            topVisible = true;
                            break;
                        }
                    }
                }
            topFaceCheckEnd:

                bool bottomVisible = false;
                for (int dx = localMinX; dx < localMaxX && !bottomVisible; ++dx) {
                    for (int dz = localMinZ; dz < localMaxZ && !bottomVisible; ++dz) {
                        int bx = x + dx;
                        int by = y + localMinY - 1; // 底面外侧
                        int bz = z + dz;
                        if (!inRange(bx, by, bz)) {
                            if (!config.keepBoundary) {
                                bottomVisible = false;
                                goto bottomFaceCheckEnd;
                            }
                            else {
                                bottomVisible = true;
                                break;
                            }
                        }
                        if (isAir(bx, by, bz)) {
                            bottomVisible = true;
                            break;
                        }
                    }
                }
            bottomFaceCheckEnd:

                bool northVisible = false;
                for (int dx = localMinX; dx < localMaxX && !northVisible; ++dx) {
                    for (int dy = localMinY; dy < localMaxY && !northVisible; ++dy) {
                        int nx = x + dx;
                        int ny = y + dy;
                        int nz = z + localMinZ - 1; // 北面外侧
                        if (!inRange(nx, ny, nz)) {
                            if (!config.keepBoundary) {
                                northVisible = false;
                                goto northFaceCheckEnd;
                            }
                            else {
                                northVisible = true;
                                break;
                            }
                        }
                        if (isAir(nx, ny, nz)) {
                            northVisible = true;
                            break;
                        }
                    }
                }
            northFaceCheckEnd:

                bool southVisible = false;
                for (int dx = localMinX; dx < localMaxX && !southVisible; ++dx) {
                    for (int dy = localMinY; dy < localMaxY && !southVisible; ++dy) {
                        int sx = x + dx;
                        int sy = y + dy;
                        int sz = z + localMaxZ; // 南面外侧
                        if (!inRange(sx, sy, sz)) {
                            if (!config.keepBoundary) {
                                southVisible = false;
                                goto southFaceCheckEnd;
                            }
                            else {
                                southVisible = true;
                                break;
                            }
                        }
                        if (isAir(sx, sy, sz)) {
                            southVisible = true;
                            break;
                        }
                    }
                }
            southFaceCheckEnd:

                bool westVisible = false;
                for (int dy = localMinY; dy < localMaxY && !westVisible; ++dy) {
                    for (int dz = localMinZ; dz < localMaxZ && !westVisible; ++dz) {
                        int wx = x + localMinX - 1; // 西面外侧
                        int wy = y + dy;
                        int wz = z + dz;
                        if (!inRange(wx, wy, wz)) {
                            if (!config.keepBoundary) {
                                westVisible = false;
                                goto westFaceCheckEnd;
                            }
                            else {
                                westVisible = true;
                                break;
                            }
                        }
                        if (isAir(wx, wy, wz)) {
                            westVisible = true;
                            break;
                        }
                    }
                }
            westFaceCheckEnd:

                bool eastVisible = false;
                for (int dy = localMinY; dy < localMaxY && !eastVisible; ++dy) {
                    for (int dz = localMinZ; dz < localMaxZ && !eastVisible; ++dz) {
                        int ex = x + localMaxX; // 东面外侧
                        int ey = y + dy;
                        int ez = z + dz;
                        if (!inRange(ex, ey, ez)) {
                            if (!config.keepBoundary) {
                                eastVisible = false;
                                goto eastFaceCheckEnd;
                            }
                            else {
                                eastVisible = true;
                                break;
                            }
                        }
                        if (isAir(ex, ey, ez)) {
                            eastVisible = true;
                            break;
                        }
                    }
                }
            eastFaceCheckEnd:

                // 如果所有面都不可见，则跳过该区域
                if (!topVisible && !bottomVisible && !northVisible &&
                    !southVisible && !westVisible && !eastVisible) {
                    continue;
                }

                // 根据包围盒生成各面顶点数据
                ModelData largeBlockModel;
                int vertexOffset = 0;
                std::vector<float> vertices;
                std::vector<int> faces;
                std::vector<int> uvFaces;
                std::vector<std::string> materialNames;
                std::vector<std::string> texturePaths;
                std::vector<int> materialIndices;

                auto addFace = [&](const float faceVerts[12]) {
                    for (int i = 0; i < 12; ++i)
                        vertices.push_back(faceVerts[i]);
                    faces.push_back(vertexOffset);
                    faces.push_back(vertexOffset + 1);
                    faces.push_back(vertexOffset + 2);
                    faces.push_back(vertexOffset + 3);
                    uvFaces.insert(uvFaces.end(), { vertexOffset, vertexOffset + 1, vertexOffset + 2, vertexOffset + 3 });
                    materialNames.push_back(color);
                    texturePaths.push_back(color);
                    materialIndices.push_back(0);
                    vertexOffset += 4;
                    };

                // 生成各面顶点数据（各面坐标均基于包围盒的局部坐标）
                if (bottomVisible) {
                    // 底面： (localMinX, localMinY, localMinZ) -> (localMaxX, localMinY, localMinZ) -> (localMaxX, localMinY, localMaxZ) -> (localMinX, localMinY, localMaxZ)
                    float bottomFace[12] = {
                        (float)localMinX, (float)localMinY, (float)localMinZ,
                        (float)localMaxX, (float)localMinY, (float)localMinZ,
                        (float)localMaxX, (float)localMinY, (float)localMaxZ,
                        (float)localMinX, (float)localMinY, (float)localMaxZ
                    };
                    addFace(bottomFace);
                }
                if (topVisible) {
                    // 顶面： (localMinX, localMaxY, localMinZ) -> (localMinX, localMaxY, localMaxZ) -> (localMaxX, localMaxY, localMaxZ) -> (localMaxX, localMaxY, localMinZ)
                    float topFace[12] = {
                        (float)localMinX, (float)localMaxY, (float)localMinZ,
                        (float)localMinX, (float)localMaxY, (float)localMaxZ,
                        (float)localMaxX, (float)localMaxY, (float)localMaxZ,
                        (float)localMaxX, (float)localMaxY, (float)localMinZ
                    };
                    addFace(topFace);
                }
                if (northVisible) {
                    // 北面（朝 -z）： (localMinX, localMinY, localMinZ) -> (localMinX, localMaxY, localMinZ) -> (localMaxX, localMaxY, localMinZ) -> (localMaxX, localMinY, localMinZ)
                    float northFace[12] = {
                        (float)localMinX, (float)localMinY, (float)localMinZ,
                        (float)localMinX, (float)localMaxY, (float)localMinZ,
                        (float)localMaxX, (float)localMaxY, (float)localMinZ,
                        (float)localMaxX, (float)localMinY, (float)localMinZ
                    };
                    addFace(northFace);
                }
                if (southVisible) {
                    // 南面（朝 +z）： (localMinX, localMinY, localMaxZ) -> (localMaxX, localMinY, localMaxZ) -> (localMaxX, localMaxY, localMaxZ) -> (localMinX, localMaxY, localMaxZ)
                    float southFace[12] = {
                        (float)localMinX, (float)localMinY, (float)localMaxZ,
                        (float)localMaxX, (float)localMinY, (float)localMaxZ,
                        (float)localMaxX, (float)localMaxY, (float)localMaxZ,
                        (float)localMinX, (float)localMaxY, (float)localMaxZ
                    };
                    addFace(southFace);
                }
                if (westVisible) {
                    // 西面（朝 -x）： (localMinX, localMinY, localMinZ) -> (localMinX, localMinY, localMaxZ) -> (localMinX, localMaxY, localMaxZ) -> (localMinX, localMaxY, localMinZ)
                    float westFace[12] = {
                        (float)localMinX, (float)localMinY, (float)localMinZ,
                        (float)localMinX, (float)localMinY, (float)localMaxZ,
                        (float)localMinX, (float)localMaxY, (float)localMaxZ,
                        (float)localMinX, (float)localMaxY, (float)localMinZ
                    };
                    addFace(westFace);
                }
                if (eastVisible) {
                    // 东面（朝 +x）： (localMaxX, localMinY, localMinZ) -> (localMaxX, localMaxY, localMinZ) -> (localMaxX, localMaxY, localMaxZ) -> (localMaxX, localMinY, localMaxZ)
                    float eastFace[12] = {
                        (float)localMaxX, (float)localMinY, (float)localMinZ,
                        (float)localMaxX, (float)localMaxY, (float)localMinZ,
                        (float)localMaxX, (float)localMaxY, (float)localMaxZ,
                        (float)localMaxX, (float)localMinY, (float)localMaxZ
                    };
                    addFace(eastFace);
                }

                // 整理生成的面数据到模型，并应用位置偏移（x,y,z 为大区域在世界中的起始位置）
                largeBlockModel.vertices = vertices;
                largeBlockModel.faces = faces;
                largeBlockModel.uvFaces = uvFaces;
                largeBlockModel.materialNames = materialNames;
                largeBlockModel.texturePaths = texturePaths;
                largeBlockModel.materialIndices = materialIndices;

                ApplyPositionOffset(largeBlockModel, x, y, z);

                // 如果是流体块，单独处理流体的LOD生成逻辑
                if (isFluidBlock) {
                    // 处理流体块的LOD生成逻辑
                    // ...
                }
                else {
                    // 处理普通方块的LOD生成逻辑
                    // ...
                }

                if (chunkModel.vertices.empty())
                    chunkModel = largeBlockModel;
                else
                    MergeModelsDirectly(chunkModel, largeBlockModel);
            }
        }
    }
    return chunkModel;
}
void RegionModelExporter::LoadChunks() {
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

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
            // 加载并缓存整个 chunk 的所有子区块
            LoadAndCacheBlockData(chunkX, chunkZ);
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