// LODManager.cpp
#include "LODManager.h"
#include "RegionModelExporter.h"
#include "coord_conversion.h"
#include "objExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "fluid.h"
#include "texture.h"
#include <iomanip>
#include <sstream>
#include <regex>
#include <tuple>
#include <chrono>
#include <iostream>
#include "EntityBlock.h"

using namespace std;
using namespace std::chrono;
// 全局变量：存储每个块（chunkX, sectionY, chunkZ）对应的 LOD 值
std::unordered_map<std::tuple<int, int, int>, float, TupleHash> g_chunkLODs;

// 缓存方块ID到颜色的映射
std::unordered_map<std::string, std::string> blockColorCache;


std::string GetBlockAverageColor(int blockId, Block currentBlock, int x, int y, int z, const std::string& faceDirection, float gamma=2.0) {
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
        textureAverage = blockColorCache[cacheKey];
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
            // 流体格式：流体名-color#r g b
            snprintf(finalColorStr, sizeof(finalColorStr), "%s-color#%.3f %.3f %.3f", currentBlock.GetNameAndNameSpaceWithoutState().c_str(), finalR, finalG, finalB);
        }
        else {
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%.3f %.3f %.3f", finalR, finalG, finalB);
        }
        return std::string(finalColorStr);
    }
    else {
        // 不需要群系混合，直接返回并缓存纹理图片的平均颜色
        if (isFluid) {
            snprintf(finalColorStr, sizeof(finalColorStr), "%s-color#%s", currentBlock.GetNameAndNameSpaceWithoutState().c_str(), textureAverage.c_str());
        }
        else {
            snprintf(finalColorStr, sizeof(finalColorStr), "color#%s", textureAverage.c_str());
        }
        return std::string(finalColorStr);
    }
}
float LODManager::GetChunkLODAtBlock(int x, int y, int z) {
    int chunkX, chunkZ, sectionY;
    blockToChunk(x, z, chunkX, chunkZ);
    blockYToSectionY(y, sectionY);
    auto key = std::make_tuple(chunkX, sectionY, chunkZ);
    if (g_chunkLODs.find(key) != g_chunkLODs.end()) {
        return g_chunkLODs[key];
    }
    return 1.0f; // 默认使用高精度
}

BlockType GetBlockType(int x, int y, int z) {
    int currentId = GetBlockId(x, y, z);
    Block currentBlock = GetBlockById(currentId);

    if (currentBlock.name == "minecraft:air") {
        return AIR;
    }
    else if (currentBlock.level > -1) {
        return FLUID;
    }
    else {
        return SOLID;
    }
}

BlockType GetBlockType2(int x, int y, int z) {
    int currentId = GetBlockId(x, y, z);
    Block currentBlock = GetBlockById(currentId);

    if (!currentBlock.air && currentBlock.level == -1) {
        return SOLID;
    }
    else
    {
        return AIR;
    }
}

// 确定 LOD 块类型的函数
BlockType DetermineLODBlockType(int x, int y, int z, int lodBlockSize, int* id = nullptr, int* level = nullptr) {
    int airLayers = 0;          // 纯空气层数
    int fluidLayers = 0;        // 流体层数
    bool hasSolidBelow = false; // 当前层下方是否存在固体层
    BlockType result = AIR;

    // 从下到上遍历每一层（0表示最底层）
    for (int dy = lodBlockSize - 1; dy >= 0; --dy) {
        int currentAir = 0, currentFluid = 0, currentSolid = 0;

        // 统计当前层各类型数量
        for (int dx = 0; dx < lodBlockSize; ++dx) {
            for (int dz = 0; dz < lodBlockSize; ++dz) {
                BlockType type = GetBlockType(x + dx, y + dy, z + dz);
                if (type == AIR)       currentAir++;
                else if (type == FLUID) currentFluid++;
                else if (type == SOLID) currentSolid++;
            }
        }

        // 判断当前层类型
        const int total = lodBlockSize * lodBlockSize;
        bool isAirLayer = (currentAir == total);
        bool isFluidLayer = !isAirLayer && (currentFluid >= currentSolid);

        if (isAirLayer) {
            airLayers++;
        }
        else if (isFluidLayer) {
            fluidLayers++;
            // 发现流体层且下方有固体时立即返回
            if (hasSolidBelow) {
                // 查找第一个流体块作为ID
                if (id) {
                    for (int dx = 0; dx < lodBlockSize; ++dx) {
                        for (int dz = 0; dz < lodBlockSize; ++dz) {
                            if (GetBlockType(x + dx, y + dy, z + dz) == FLUID) {
                                *id = GetBlockId(x + dx, y + dy, z + dz);
                                goto SET_LEVEL_AND_RETURN;
                            }
                        }
                    }
                }
            SET_LEVEL_AND_RETURN:
                if (level) *level = airLayers;
                return FLUID;
            }
        }
        else {
            hasSolidBelow = true; // 标记遇到固体层
        }
    }

    // 最终类型判断
    if (fluidLayers > 0)       result = FLUID;
    else if (hasSolidBelow)    result = SOLID;
    else                       result = AIR;

    // 设置ID（从上到下查找第一个对应类型）
    if (id) {
        *id = 0;
        for (int dy = lodBlockSize - 1; dy >= 0; --dy) {
            for (int dx = 0; dx < lodBlockSize; ++dx) {
                for (int dz = 0; dz < lodBlockSize; ++dz) {
                    BlockType type = GetBlockType(x + dx, y + dy, z + dz);
                    if (type == result) {
                        *id = GetBlockId(x + dx, y + dy, z + dz);
                        goto SET_LEVEL;
                    }
                }
            }
        }
    }

SET_LEVEL:
    if (level) {
        *level = (result == SOLID) ? (airLayers + fluidLayers) : airLayers;
    }

    return result;
}

BlockType LODManager::DetermineLODBlockTypeWithUpperCheck(int x, int y, int z, int lodBlockSize, int* id, int* level) {
    // 首先检查当前块
    int currentLevel = 0;
    BlockType currentType = DetermineLODBlockType(x, y, z, lodBlockSize, id, &currentLevel);

    // 然后检查上方的 LOD 块 (y + 1)
    BlockType upperType = DetermineLODBlockType(x, y + lodBlockSize, z, lodBlockSize);

    if (level != nullptr) {
        // 如果上方的类型不是空气，则将 level 设置为 0
        if (upperType != AIR) {
            *level = 0;

        }
        else {
            // 如果上方是空气，则返回当前块的层数
            *level = currentLevel;

        }

    }


    // 返回当前块的类型
    return currentType;
}

std::vector<std::string> LODManager::GetBlockColor(int x, int y, int z, int id, BlockType blockType) {
    Block currentBlock = GetBlockById(id);

    if (blockType == FLUID) {
        return {GetBlockAverageColor(id, currentBlock, x, y, z, "none") };
    }
    else {
        std::string upColor = GetBlockAverageColor(id, currentBlock, x, y, z, "up");
        std::string northColor = GetBlockAverageColor(id, currentBlock, x, y, z, "north");
        return { upColor,northColor };  // 使用不同的颜色组合
    }
}

// 修改后的 IsRegionEmpty 方法：增加 isFluid 参数（默认为 false，用于固体判断）
bool IsRegionEmpty(int x, int y, int z, float lodSize) {
    int height;
    BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodSize, nullptr, &height);
    if (type == BlockType::SOLID && height == 0)
    {
        return false;
    }
    return true;
}

// 辅助函数：判断指定区域是否有效 
bool IsRegionValid(int x, int y, int z, float lodSize) {
    // 边界检查
    if (x < config.minX || x + lodSize > config.maxX ||
        z < config.minZ || z + lodSize > config.maxZ ||
        y < config.minY || y + lodSize > config.maxY) {
        if (config.keepBoundary)
            return false;
        return true;
    }

    return !IsRegionEmpty(x, y, z, lodSize);
}

// 修改后的 IsRegionEmpty 方法：增加 isFluid 参数（默认为 false，用于固体判断）
bool IsFluidRegionEmpty(int x, int y, int z, float lodSize, float h) {
    int height;
    BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodSize, nullptr, &height);
    BlockType upperType = DetermineLODBlockType(x, y + lodSize, z, lodSize);
    if ((type == BlockType::SOLID || (type == BlockType::FLUID && upperType != AIR)) && height == 0)
    {
        return false;
    }
    return true;
}
// 辅助函数：判断指定区域是否有效 
bool IsFluidRegionValid(int x, int y, int z, float lodSize, float h) {
    // 边界检查
    if (x < config.minX || x + lodSize > config.maxX ||
        z < config.minZ || z + lodSize > config.maxZ ||
        y < config.minY || y + lodSize > config.maxY) {
        if (config.keepBoundary)
            return false;
        return true;
    }

    return !IsFluidRegionEmpty(x, y, z, lodSize, h);
}

// 修改后的 IsRegionEmpty 方法：增加 isFluid 参数（默认为 false，用于固体判断）
bool IsFluidTopRegionEmpty(int x, int y, int z, float lodSize, float h) {
    int height;
    BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodSize, nullptr, &height);
    if (((type == BlockType::SOLID) && height == 0) || type == BlockType::FLUID)
    {
        return false;
    }
    return true;
}
// 辅助函数：判断指定区域是否有效 
bool IsFluidTopRegionValid(int x, int y, int z, float lodSize, float h) {
    // 边界检查
    if (x < config.minX || x + lodSize > config.maxX ||
        z < config.minZ || z + lodSize > config.maxZ ||
        y < config.minY || y + lodSize > config.maxY) {
        if (config.keepBoundary)
            return false;
        return true;
    }

    return !IsFluidTopRegionEmpty(x, y, z, lodSize, h);
}

bool IsFaceOccluded(int faceDir, int x, int y, int z, int baseSize) {
    int dxStart, dxEnd, dyStart, dyEnd, dzStart, dzEnd;

    // 根据面方向设置检测范围
    switch (faceDir) {
    case 0: // 底面（y-方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y - 1;
        dyEnd = y;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 1: // 顶面（y+方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y + baseSize;
        dyEnd = y + baseSize + 1;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 2: // 北面（z-方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z - 1;
        dzEnd = z;
        break;
    case 3: // 南面（z+方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z + baseSize;
        dzEnd = z + baseSize + 1;
        break;
    case 4: // 西面（x-方向）
        dxStart = x - 1;
        dxEnd = x;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 5: // 东面（x+方向）
        dxStart = x + baseSize;
        dxEnd = x + baseSize + 1;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    default:
        return false;
    }


    // 遍历检测区域内的所有方块
    for (int dx = dxStart; dx < dxEnd; ++dx) {
        for (int dy = dyStart; dy < dyEnd; ++dy) {
            for (int dz = dzStart; dz < dzEnd; ++dz) {
                BlockType type = GetBlockType2(dx, dy, dz);
                if (type != SOLID) {
                    return false; // 发现非固体方块，不剔除该面
                }
            }
        }
    }
    return true; // 区域全为固体，需要剔除该面
}

bool IsFluidFaceOccluded(int faceDir, int x, int y, int z, int baseSize) {
    int dxStart, dxEnd, dyStart, dyEnd, dzStart, dzEnd;

    // 根据面方向设置检测范围
    switch (faceDir) {
    case 0: // 底面（y-方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y - 1;
        dyEnd = y;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 1: // 顶面（y+方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y + baseSize;
        dyEnd = y + baseSize + 1;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 2: // 北面（z-方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z - 1;
        dzEnd = z;
        break;
    case 3: // 南面（z+方向）
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z + baseSize;
        dzEnd = z + baseSize + 1;
        break;
    case 4: // 西面（x-方向）
        dxStart = x - 1;
        dxEnd = x;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 5: // 东面（x+方向）
        dxStart = x + baseSize;
        dxEnd = x + baseSize + 1;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    default:
        return false;
    }


    // 遍历检测区域内的所有方块
    for (int dx = dxStart; dx < dxEnd; ++dx) {
        for (int dy = dyStart; dy < dyEnd; ++dy) {
            for (int dz = dzStart; dz < dzEnd; ++dz) {
                BlockType type = GetBlockType2(dx, dy, dz);
                if (type != SOLID) {
                    return false; // 发现非固体方块，不剔除该面
                }
            }
        }
    }
    return true; // 区域全为固体，需要剔除该面
}


// 修改后的 GenerateBox，增加了 boxHeight 参数 
ModelData LODManager::GenerateBox(int x, int y, int z, int baseSize, float boxHeight,
    const std::vector<std::string>& colors) {
    ModelData box;

    float size = static_cast<float>(baseSize);
    float height = static_cast<float>(boxHeight);
    if (colors.size() == 1) {
        // 然后检查上方的 LOD 块 (y + 1)
        BlockType upperType = DetermineLODBlockType(x, y + baseSize, z, baseSize);
        // 如果上方的类型不是空气，则将 level 设置为 0
        if (upperType == AIR) {
            height = height - 0.1f;
        }

    }
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
    RegionModelExporter::ApplyPositionOffset(box, x, y, z);

    // 面剔除逻辑
    std::vector<bool> validFaces(6, true);
    if (colors.size() == 1) {
        // 顶面照常判断
        validFaces[1] = IsFluidTopRegionValid(x, y + baseSize, z, baseSize, boxHeight) ? false : true;
        // 下、东西、南北方向传入
        validFaces[0] = IsFluidRegionValid(x, y - baseSize, z, baseSize, boxHeight) ? false : true; // 底面
        validFaces[4] = IsFluidRegionValid(x - baseSize, y, z, baseSize, boxHeight) ? false : true; // 西面
        validFaces[5] = IsFluidRegionValid(x + baseSize, y, z, baseSize, boxHeight) ? false : true; // 东方
        validFaces[2] = IsFluidRegionValid(x, y, z - baseSize, baseSize, boxHeight) ? false : true; // 北面
        validFaces[3] = IsFluidRegionValid(x, y, z + baseSize, baseSize, boxHeight) ? false : true; // 南面
    }
    else if (colors.size() >= 2) {
        // 顶面照常判断
        validFaces[1] = IsRegionValid(x, y + baseSize, z, baseSize) ? false : true;
        // 下、东西、南北方向传入
        validFaces[0] = IsRegionValid(x, y - baseSize, z, baseSize) ? false : true; // 底面
        validFaces[4] = IsRegionValid(x - baseSize, y, z, baseSize) ? false : true; // 西面
        validFaces[5] = IsRegionValid(x + baseSize, y, z, baseSize) ? false : true; // 东方
        validFaces[2] = IsRegionValid(x, y, z - baseSize, baseSize) ? false : true; // 北面
        validFaces[3] = IsRegionValid(x, y, z + baseSize, baseSize) ? false : true; // 南面

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
            float neighborLOD = LODManager::GetChunkLODAtBlock(nx, ny, nz);
            // 当相邻LOD更小时进行精确检测
            if (neighborLOD != baseSize && baseSize >= 1) {
                bool isOccluded = IsFaceOccluded(faceIdx, x, y, z, baseSize);
                validFaces[faceIdx] = !isOccluded; // 遮挡时设为false
            }
        }
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