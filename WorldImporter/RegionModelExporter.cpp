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
#include <tuple>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include "EntityBlock.h"

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono

struct TupleHash {
    std::size_t operator()(const std::tuple<int, int, int>& t) const {
        auto h1 = std::hash<int>()(std::get<0>(t));
        auto h2 = std::hash<int>()(std::get<1>(t));
        auto h3 = std::hash<int>()(std::get<2>(t));
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

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


// 全局变量：存储每个块（chunkX, sectionY, chunkZ）对应的 LOD 值
static std::unordered_map<std::tuple<int, int, int>, float, TupleHash> g_chunkLODs;

// 缓存方块ID到颜色的映射
std::unordered_map<std::string, std::string> blockColorCache;

std::string GetBlockAverageColor(int blockId, Block currentBlock, int x, int y, int z, const std::string& faceDirection, float gamma = 2.0f) {
    std::string ns = GetBlockNamespaceById(blockId);
    std::string blockName = GetBlockNameById(blockId);
    // 标准化方块名称（去掉命名空间，处理状态）
    size_t colonPos = blockName.find(':');
    if (colonPos != std::string::npos) {
        blockName = blockName.substr(colonPos + 1);
    }
    ModelData blockModel;
    // 判断当前方块是否是注册流体或已有level标记
    bool isFluid = (fluidDefinitions.find(currentBlock.GetNameAndNameSpaceWithoutState()) != fluidDefinitions.end());
    if (isFluid && currentBlock.level > -1) {
        AssignFluidMaterials(blockModel, currentBlock.name);
    }
    else {
        // 处理其他方块
        blockModel = GetRandomModelFromCache(ns, blockName);
    }
    // 构建缓存键，包含面方向信息
    std::string cacheKey = std::to_string(blockId) + ":" + faceDirection;

    

    std::string textureAverage;

    // 先从缓存中获取纹理图片的平均颜色
    if (blockColorCache.find(cacheKey) != blockColorCache.end()) {
        textureAverage= blockColorCache[cacheKey];
    }
    else
    {
        // 根据面方向获取材质索引
        int materialIndex = -1;
        if (faceDirection == "none") {
            // 如果面方向为none，取第一个材质
            if (!blockModel.materialNames.empty()) {
                materialIndex = 0;
            }
        }
        else {
            // 根据面方向在faceNames中查找对应的材质索引
            for (size_t i = 0; i < blockModel.faceNames.size(); ++i) {
                if (blockModel.faceNames[i] == faceDirection) {
                    // 每个面的材质索引对应4个顶点，取第一个顶点的材质索引
                    materialIndex = blockModel.materialIndices[i / 4];
                    break;
                }
            }
        }

        // 如果没有找到材质索引，取第一个材质
        if (materialIndex == -1 && !blockModel.materialNames.empty()) {
            materialIndex = 0;
        }

        // 如果没有材质信息，返回默认颜色
        if (materialIndex == -1) {
            return "color#0.500 0.500 0.500";
        }

        // 获取材质名称
        std::string materialName = blockModel.materialNames[materialIndex];



        // 获取纹理路径
        std::string texturePath;

        if (!blockModel.texturePaths.empty()) {
            texturePath = blockModel.texturePaths[materialIndex];
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
        blockColorCache[cacheKey] = textureAverage;
    }

    char finalColorStr[128];
    // 如果需要群系颜色混合，则每次都进行混合计算，不缓存混合后的结果
    if (blockModel.tintindex != -1) {
        // 解析缓存的图片平均颜色
        float textureR, textureG, textureB;
        sscanf(textureAverage.c_str(), "%f %f %f", &textureR, &textureG, &textureB);
        uint32_t hexColor;
        // 获取当前坐标的群系颜色（十六进制），转换为 0-1 范围的 RGB
        if (blockModel.tintindex == 2) {
            hexColor = Biome::GetBiomeColor(x, y, z, BiomeColorType::Water);
        }
        else {
            hexColor = Biome::GetBiomeColor(x, y, z, BiomeColorType::Foliage);
        }

        float biomeR = ((hexColor >> 16) & 0xFF) / 255.0f;
        float biomeG = ((hexColor >> 8) & 0xFF) / 255.0f;
        float biomeB = (hexColor & 0xFF) / 255.0f;

        // 正片叠底混合（乘法混合）：各通道相乘
        float finalR = biomeR * textureR;
        float finalG = biomeG * textureG;
        float finalB = biomeB * textureB;

        if (isFluid) {
            // 流体格式：color#r g b-流体名
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%.3f %.3f %.3f-%s", finalR, finalG, finalB, currentBlock.GetNameAndNameSpaceWithoutState().c_str());
        }
        else {
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%.3f %.3f %.3f", finalR, finalG, finalB);
        }
        return std::string(finalColorStr);
    }
    else {
        // 不需要群系混合，直接返回并缓存纹理图片的平均颜色
        if (isFluid) {
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%s-%s", textureAverage.c_str()), currentBlock.GetNameAndNameSpaceWithoutState().c_str();
        }
        else {
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%s", textureAverage.c_str());
        }
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
                    chunkLOD = 1.0f;    
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

float RegionModelExporter::GetChunkLODAtBlock(int x, int y, int z) {
    int chunkX, chunkZ, sectionY;
    blockToChunk(x, z, chunkX, chunkZ);
    blockYToSectionY(y, sectionY);
    auto key = std::make_tuple(chunkX, sectionY, chunkZ);
    if (g_chunkLODs.find(key) != g_chunkLODs.end()) {
        return g_chunkLODs[key];
    }
    return 1.0f; // 默认使用高精度
}

// 辅助函数：判断指定区域是否有效 
bool RegionModelExporter::IsRegionValid(int x, int y, int z, float lodSize, bool ignoreCompressed) {
    // 边界检查
    if (x < config.minX || x + lodSize > config.maxX ||
        z < config.minZ || z + lodSize > config.maxZ ||
        y < config.minY || y + lodSize > config.maxY) {
        if (config.keepBoundary) {
            return false;
        }
        return true;
    }
    // 当要求忽略高度压缩时，先检查目标区域是否为高度压缩块
    if (ignoreCompressed) {
        int airLayers = 0;
        for (int layer = lodSize - 1; layer >= 0; --layer) {
            bool layerAllAir = true;
            for (int dx = 0; dx < lodSize; ++dx) {
                for (int dz = 0; dz < lodSize; ++dz) {
                    int blockId = GetBlockId(x + dx, y + layer, z + dz);
                    Block currentBlock = GetBlockById(blockId);
                    if (currentBlock.name != "minecraft:air") {
                        layerAllAir = false;
                        break;
                    }
                }
                if (!layerAllAir)
                    break;
            }
            if (layerAllAir)
                airLayers++;
            else
                break;
        }
        // 如果有效高度不足（即高度压缩了），则认为该区域为空
        if (lodSize - airLayers < lodSize)
            return false;
    }
    return !IsRegionEmpty(x, y, z, lodSize);
}

// 修改后的 IsRegionEmpty 方法：增加 isFluid 参数（默认为 false 用于固体判断）
bool RegionModelExporter::IsRegionEmpty(int x, int y, int z, float lodSize, bool isFluid) {
    // 仅检测有效高度范围内的方块
    for (int dx = 0; dx < lodSize; ++dx) {
        for (int dz = 0; dz < lodSize; ++dz) {
            for (int dy = 0; dy < lodSize; ++dy) {
                int blockId = GetBlockId(x + dx, y + dy, z + dz);
                Block currentBlock = GetBlockById(blockId);
                if (isFluid) {
                    // 流体判断：只要不是 air 就认为区域非空
                    if (currentBlock.name != "minecraft:air")
                        return false;
                }
                else {
                    // 固体判断：依据原有条件
                    if ((!currentBlock.air || (currentBlock.level != -1 && !config.useUnderwaterLOD)))
                        return false;
                }
            }
        }
    }
    return true;
}


bool RegionModelExporter::IsFluidNeighborEmpty(int nx, int ny, int nz, float size) {
    if (nx < config.minX || nx + size > config.maxX ||
        ny < config.minY || ny + size > config.maxY ||
        nz < config.minZ || nz + size > config.maxZ) {
        return config.keepBoundary;
    }
    return IsRegionEmpty(nx, ny, nz, size, true);
}

// 修改后的 GenerateBox，增加了 boxHeight 参数
ModelData RegionModelExporter::GenerateBox(int x, int y, int z, int baseSize, int boxHeight,
    const std::vector<std::string>& colors, bool isFluid) {
    ModelData box;

    float size = static_cast<float>(baseSize);
    float height = static_cast<float>(boxHeight);
    // 构造顶点数组，注意 y 方向使用 boxHeight
    box.vertices = {
        // 底面
        0.0f, 0.0f, 0.0f,
        size, 0.0f, 0.0f,
        size, 0.0f, size,
        0.0f, 0.0f, size,
        // 顶面（高度为 boxHeight）
        0.0f, height, 0.0f,
        size, height, 0.0f,
        size, height, size,
        0.0f, height, size,
        // 北面
        0.0f, 0.0f, 0.0f,
        size, 0.0f, 0.0f,
        size, height, 0.0f,
        0.0f, height, 0.0f,
        // 南面
        0.0f, 0.0f, size,
        size, 0.0f, size,
        size, height, size,
        0.0f, height, size,
        // 西面
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, size,
        0.0f, height, size,
        0.0f, height, 0.0f,
        // 东方
        size, 0.0f, 0.0f,
        size, 0.0f, size,
        size, height, size,
        size, height, 0.0f
    };

    box.faces = {
        0, 3, 2, 1,      // 底面
        4, 7, 6, 5,      // 顶面
        8, 11, 10, 9,    // 北面
        12, 15, 14, 13,  // 南面
        16, 17, 18, 19,  // 西面
        20, 23, 22, 21   // 东方
    };

    box.uvCoordinates = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f
    };

    // 材质设置
    if (colors.empty()) {
        box.materialNames = { "default_color" };
        box.texturePaths = { "default_color" };
        box.materialIndices = { 0, 0, 0, 0, 0, 0 };
    }
    else if (colors.size() == 1 || (colors.size() >= 2 && colors[0] == colors[1])) {
        box.materialNames = { colors[0] };
        box.texturePaths = { colors[0] };
        box.materialIndices = { 0, 0, 0, 0, 0, 0 };
    }
    else {
        box.materialNames = { colors[0], colors[1] };
        box.texturePaths = { colors[0], colors[1] };
        box.materialIndices = { 1, 0, 1, 1, 1, 1 };
    }

    // 调整模型位置
    ApplyPositionOffset(box, x, y, z);

    // 面剔除逻辑
    std::vector<bool> validFaces(6, true);
    if (!isFluid) {
        // 顶面照常判断，不忽略高度压缩
        validFaces[1] = IsRegionValid(x, y + baseSize, z, baseSize) ? false : true;
        // 下、东西、南北方向传入 ignoreCompressed = true
        validFaces[0] = IsRegionValid(x, y - baseSize, z, baseSize, true) ? false : true; // 底面
        validFaces[4] = IsRegionValid(x - baseSize, y, z, baseSize, true) ? false : true; // 西面
        validFaces[5] = IsRegionValid(x + baseSize, y, z, baseSize, true) ? false : true; // 东方
        validFaces[2] = IsRegionValid(x, y, z - baseSize, baseSize, true) ? false : true; // 北面
        validFaces[3] = IsRegionValid(x, y, z + baseSize, baseSize, true) ? false : true; // 南面

        for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
            int nx = x, ny = y, nz = z;
            switch (faceIdx) {
            case 0: ny = y - baseSize; break;
            case 1: ny = y + baseSize; break;
            case 2: nz = z - baseSize; break;
            case 3: nz = z + baseSize; break;
            case 4: nx = x - baseSize; break;
            case 5: nx = x + baseSize; break;
            }
            float neighborLOD = GetChunkLODAtBlock(nx, ny, nz);
            // 如果相邻区块的 lod 小于当前区块的 lod，并且当前区块足够大（lodBlockSize >= 2）
            if (neighborLOD < baseSize - 0.001f && baseSize >= 2) {
                bool shouldGenerate = false;
                float refinedLodSize = baseSize / 2.0f; // 细分后的 lod 大小
                // 将临界面划分为 2x2 共4个子区域进行判断
                for (int i = 0; i < 2; ++i) {
                    for (int j = 0; j < 2; ++j) {
                        bool subRegionValid = false;
                        switch (faceIdx) {
                        case 0: // 底面，邻接区块在 y - lodBlockSize
                            subRegionValid = IsRegionValid(x + i * refinedLodSize, y - baseSize, z + j * refinedLodSize, refinedLodSize);
                            break;
                        case 1: // 顶面，邻接区块在 y + lodBlockSize
                            subRegionValid = IsRegionValid(x + i * refinedLodSize, y + baseSize, z + j * refinedLodSize, refinedLodSize);
                            break;
                        case 2: // 北面，邻接区块在 z - lodBlockSize（面在 x-y 平面）
                            subRegionValid = IsRegionValid(x + i * refinedLodSize, y + j * refinedLodSize, z - baseSize, refinedLodSize);
                            break;
                        case 3: // 南面，邻接区块在 z + lodBlockSize
                            subRegionValid = IsRegionValid(x + i * refinedLodSize, y + j * refinedLodSize, z + baseSize, refinedLodSize);
                            break;
                        case 4: // 西面，邻接区块在 x - lodBlockSize（面在 y-z 平面）
                            subRegionValid = IsRegionValid(x - baseSize, y + i * refinedLodSize, z + j * refinedLodSize, refinedLodSize);
                            break;
                        case 5: // 东方，邻接区块在 x + lodBlockSize
                            subRegionValid = IsRegionValid(x + baseSize, y + i * refinedLodSize, z + j * refinedLodSize, refinedLodSize);
                            break;
                        }
                        // 如果任一子区域为空（即 IsRegionValid 返回 false），则认为该面需要生成
                        if (!subRegionValid) {
                            shouldGenerate = true;
                            break;
                        }
                    }
                    if (shouldGenerate) break;
                }
                validFaces[faceIdx] = shouldGenerate;
            }
           
        }

    }
    else {
        validFaces[0] = IsFluidNeighborEmpty(x, y - baseSize, z, baseSize);
        validFaces[1] = IsFluidNeighborEmpty(x, y + baseSize, z, baseSize);
        validFaces[2] = IsFluidNeighborEmpty(x, y, z - baseSize, baseSize);
        validFaces[3] = IsFluidNeighborEmpty(x, y, z + baseSize, baseSize);
        validFaces[4] = IsFluidNeighborEmpty(x - baseSize, y, z, baseSize);
        validFaces[5] = IsFluidNeighborEmpty(x + baseSize, y, z, baseSize);
    }

    // 根据 validFaces 过滤不需要的面
    ModelData filteredBox;
    for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
        if (validFaces[faceIdx]) {
            for (int i = 0; i < 4; ++i) {
                filteredBox.faces.push_back(box.faces[faceIdx * 4 + i]);
                filteredBox.uvFaces.push_back(box.uvCoordinates[faceIdx * 4 + i]);
            }
            filteredBox.materialIndices.push_back(box.materialIndices[faceIdx]);
        }
    }
    filteredBox.vertices = box.vertices;
    filteredBox.uvCoordinates = box.uvCoordinates;
    filteredBox.materialNames = box.materialNames;
    filteredBox.texturePaths = box.texturePaths;
    return filteredBox;
}


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

                // 新增：从上往下扫描当前区域内连续全为空气的层数
                int airLayers = 0;
                for (int layer = lodBlockSize - 1; layer >= 0; layer--) {
                    bool layerAllAir = true;
                    for (int dx = 0; dx < lodBlockSize; dx++) {
                        for (int dz = 0; dz < lodBlockSize; dz++) {
                            int blockId = GetBlockId(x + dx, y + layer, z + dz);
                            Block currentBlock = GetBlockById(blockId);
                            if (currentBlock.name != "minecraft:air") {
                                layerAllAir = false;
                                break;
                            }
                        }
                        if (!layerAllAir) break;
                    }
                    if (layerAllAir)
                        airLayers++;
                    else
                        break;
                }
                int effectiveHeight = lodBlockSize - airLayers;
                // 如果整个区域都是空气，则跳过
                if (effectiveHeight <= 0)
                    continue;

                // 检查当前区域内是否有固体或流体
                bool hasSolid = false;
                bool hasFluid = false;
                int solidId;
                int maxFluidSurfaceLocal = -1;
                std::vector<std::string> fluidColor = { "#FFFFFF" };

                // 仅扫描有效高度部分
                for (int dx = 0; dx < lodBlockSize; ++dx) {
                    for (int dz = 0; dz < lodBlockSize; ++dz) {
                        for (int dy = effectiveHeight - 1; dy >= 0; --dy) {
                            int blockId = GetBlockId(x + dx, y + dy, z + dz);
                            Block currentBlock = GetBlockById(blockId);
                            if (currentBlock.name == "minecraft:air")
                                continue;
                            if (currentBlock.level == -1) {
                                if (!hasSolid) {
                                    solidId = blockId;
                                    hasSolid = true;
                                }
                            }
                            else {
                                if (!hasFluid) {
                                    fluidColor[0] = GetBlockAverageColor(blockId, currentBlock, x, y, z, "none");
                                    hasFluid = true;
                                }
                                if (currentBlock.level == 0) {
                                    if (dy > maxFluidSurfaceLocal)
                                        maxFluidSurfaceLocal = dy;
                                }
                            }
                        }
                    }
                }

                std::vector<std::string> solidColor = { "#FFFFFF", "#CCCCCC" };
                if (hasSolid) {
                    solidColor[0] = GetBlockAverageColor(solidId, GetBlockById(solidId), x, y, z, "up");
                    solidColor[1] = GetBlockAverageColor(solidId, GetBlockById(solidId), x, y, z, "north");
                }

                bool generateFluid = false;
                if (hasFluid) {
                    bool allTopFluid = true;
                    for (int dx = 0; dx < lodBlockSize; ++dx) {
                        for (int dz = 0; dz < lodBlockSize; ++dz) {
                            bool foundNonAir = false;
                            for (int dy = effectiveHeight - 1; dy >= 0; --dy) {
                                int blockId = GetBlockId(x + dx, y + dy, z + dz);
                                Block currentBlock = GetBlockById(blockId);
                                if (currentBlock.name != "minecraft:air") {
                                    foundNonAir = true;
                                    if (currentBlock.level == -1)
                                        allTopFluid = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (allTopFluid)
                        generateFluid = true;
                }

                // 根据 solid/fluid 状态生成对应的模型块，此处调用新的 GenerateBox，并传入 effectiveHeight 作为高度参数
                if (generateFluid && lodBlockSize == 1) {
                    ModelData fluidBox = GenerateBox(x, y, z, lodBlockSize, effectiveHeight, fluidColor, true);
                    MergeModelsDirectly(chunkModel, fluidBox);
                }
                else if (hasSolid) {
                    ModelData solidBox = GenerateBox(x, y, z, lodBlockSize, effectiveHeight, solidColor, false);
                    MergeModelsDirectly(chunkModel, solidBox);
                }
                else if (hasFluid) {
                    ModelData fluidBox = GenerateBox(x, y, z, lodBlockSize, effectiveHeight, fluidColor, true);
                    MergeModelsDirectly(chunkModel, fluidBox);
                }
            }
        }
    }
    return chunkModel;
}





