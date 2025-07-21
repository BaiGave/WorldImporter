// LODManager.cpp
#include "LODManager.h"
#include "RegionModelExporter.h"
#include "locutil.h"
#include "ObjExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "Fluid.h"
#include "texture.h"
#include <iomanip>
#include <sstream>
#include <regex>
#include <tuple>
#include <chrono>
#include <iostream>
#include <shared_mutex>
#include <filesystem>

#ifdef _WIN32
// Windows平台需要的定义和函数声明
extern "C" {
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
}
#define MAX_PATH 260
#elif defined(__unix__) || defined(__APPLE__)
#include <limits.h>
#define MAX_PATH PATH_MAX
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

using namespace std;
using namespace std::chrono;

std::unordered_map<std::tuple<int, int, int>, ChunkSectionInfo, TupleHash> g_chunkSectionInfoMap;

// 线程安全:共享互斥量定义
std::shared_mutex g_chunkSectionInfoMapMutex;

// 缓存方块ID到颜色的映射
std::unordered_map<std::string, std::string> blockColorCache;

std::mutex blockColorCacheMutex;

// 获取可执行文件路径的辅助函数
std::string getExecutablePathForTexture() {
    std::filesystem::path exePath;
    
    try {
        #ifdef _WIN32
        // Windows平台
        char buffer[MAX_PATH] = { 0 };
        if (GetModuleFileNameA(NULL, buffer, MAX_PATH) != 0) {
            exePath = std::filesystem::path(buffer);
        } else {
            throw std::runtime_error("no exe");
        }
        #elif defined(__APPLE__)
        // macOS平台
        char buffer[MAX_PATH];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        exePath = std::filesystem::canonical(std::filesystem::path(buffer));
        #else
        // Linux平台
        char buffer[MAX_PATH];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        buffer[len] = '\0';
        exePath = std::filesystem::path(buffer);
        #endif
        
        return exePath.string();
    } catch (const std::exception& e) {
        std::cerr << "获取可执行文件路径失败: " << e.what() << std::endl;
        return std::filesystem::current_path().string(); // 失败时返回当前目录
    }
}

std::string GetBlockAverageColor(int blockId, Block currentBlock, int x, int y, int z, const std::string& faceDirection, float gamma = 2.0) {

    Block b = GetBlockById(blockId);
    std::string blockName = b.GetModifiedNameWithNamespace();
    std::string ns = b.GetNamespace();

    size_t colonPos = blockName.find(':');
    if (colonPos != std::string::npos) {
        blockName = blockName.substr(colonPos + 1);
    }
    ModelData blockModel;
    bool isFluid = (fluidDefinitions.find(currentBlock.GetNameAndNameSpaceWithoutState()) != fluidDefinitions.end());
    if (isFluid && currentBlock.level > -1) {
        AssignFluidMaterials(blockModel, currentBlock.name);
    }
    else {
        blockModel = GetRandomModelFromCache(ns, blockName);
    }
    std::string cacheKey = std::to_string(blockId) + ":" + faceDirection;
    std::string textureAverage;

    // 线程安全的缓存访问
    {
        std::lock_guard<std::mutex> lock(blockColorCacheMutex);
        auto it = blockColorCache.find(cacheKey);
        if (it != blockColorCache.end()) {
            textureAverage = it->second;
        }
    }

    // 声明materialIndex变量,使其在整个函数中可见
    int materialIndex = -1;
    
    if (textureAverage.empty()) {
        if (faceDirection == "none") {
            if (!blockModel.materials.empty()) materialIndex = 0;
        }
        else {
            // 将字符串方向转换为枚举
            FaceType targetType = StringToFaceType(faceDirection);
            
            // 查找匹配的面
            for (size_t i = 0; i < blockModel.faces.size(); i++) {
                if (blockModel.faces[i].faceDirection == targetType) {
                    materialIndex = blockModel.faces[i].materialIndex;
                    break;
                }
            }
        }

        if (materialIndex == -1 && !blockModel.materials.empty()) materialIndex = 0;
        if (materialIndex == -1) return "color#0.50 0.50 0.50=";

        std::string materialName = blockModel.materials[materialIndex].name;
        std::string texturePath;
        if (!blockModel.materials.empty()) texturePath = blockModel.materials[materialIndex].texturePath;

        float r = 0.5f, g = 0.5f, b = 0.5f;
        if (!texturePath.empty()) {
            // 使用跨平台方式获取可执行文件路径
            std::string exePath = getExecutablePathForTexture();
            std::string exeDir = std::filesystem::path(exePath).parent_path().string();
            texturePath = exeDir + "//" + texturePath;

            int width, height, channels;
            unsigned char* data = stbi_load(texturePath.c_str(), &width, &height, &channels, 0);
            if (data) {
                float sumR = 0, sumG = 0, sumB = 0;
                int validPixelCount = 0;
                for (int i = 0; i < width * height; ++i) {
                    if (channels >= 4 && data[i * channels + 3] == 0) continue;
                    float r_s = data[i * channels] / 255.0f;
                    float g_s = data[i * channels + 1] / 255.0f;
                    float b_s = data[i * channels + 2] / 255.0f;
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
                    avgR_lin = pow(avgR_lin, gamma);
                    avgG_lin = pow(avgG_lin, gamma);
                    avgB_lin = pow(avgB_lin, gamma);
                    r = (avgR_lin <= 0.0031308f) ? (avgR_lin * 12.92f) : (1.055f * pow(avgR_lin, 1.0f / 2.4f) - 0.055f);
                    g = (avgG_lin <= 0.0031308f) ? (avgG_lin * 12.92f) : (1.055f * pow(avgG_lin, 1.0f / 2.4f) - 0.055f);
                    b = (avgB_lin <= 0.0031308f) ? (avgB_lin * 12.92f) : (1.055f * pow(avgB_lin, 1.0f / 2.4f) - 0.055f);
                }
                stbi_image_free(data);
            }
        }

        // 根据配置的小数位数格式化颜色字符串
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(config.decimalPlaces);
        oss << r << " " << g << " " << b;
        textureAverage = oss.str();

        // 更新缓存
        {
            std::lock_guard<std::mutex> lock(blockColorCacheMutex);
            if (blockColorCache.find(cacheKey) == blockColorCache.end()) {
                blockColorCache[cacheKey] = textureAverage;
            }
        }
    }

    char finalColorStr[128];
    // 检查模型中是否有任何材质需要tint索引(群系着色)
    bool hasTintIndex = false;
    short tintIndexValue = -1;
    
    // 首先检查当前使用的材质
    if (!blockModel.materials.empty() && materialIndex >= 0 && materialIndex < blockModel.materials.size()) {
        tintIndexValue = blockModel.materials[materialIndex].tintIndex;
        hasTintIndex = (tintIndexValue != -1);
    }
    
    // 如果当前材质不需要着色,检查模型中的所有其他材质
    if (!hasTintIndex && !blockModel.materials.empty()) {
        for (const auto& material : blockModel.materials) {
            if (material.tintIndex != -1) {
                hasTintIndex = true;
                tintIndexValue = material.tintIndex;
                break;
            }
        }
    }
    
    if (hasTintIndex && config.useBiomeColors) {
        float textureR, textureG, textureB;
        sscanf(textureAverage.c_str(), "%f %f %f", &textureR, &textureG, &textureB);
        uint32_t hexColor = Biome::GetBiomeColor(x, y, z,tintIndexValue == 2 ? BiomeColorType::Water : BiomeColorType::Foliage);
        float biomeR = ((hexColor >> 16) & 0xFF) / 255.0f;
        float biomeG = ((hexColor >> 8) & 0xFF) / 255.0f;
        float biomeB = (hexColor & 0xFF) / 255.0f;
        float finalR = biomeR * textureR;
        float finalG = biomeG * textureG;
        float finalB = biomeB * textureB;

        // 根据配置的小数位数格式化最终颜色字符串
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(config.decimalPlaces);
        if (isFluid) {
            oss << "color#" << finalR << " " << finalG << " " << finalB << "-" << currentBlock.GetNameAndNameSpaceWithoutState().c_str();
        }
        else {
            oss << "color#" << finalR << " " << finalG << " " << finalB << "=";
        }
        return oss.str();
    }
    else {
        if (isFluid) {
            return  + "color#" + textureAverage + "-" + currentBlock.GetNameAndNameSpaceWithoutState();
        }
        else {
            return "color#" + textureAverage + "=";
        }
    }
}

float LODManager::GetChunkLODAtBlock(int x, int y, int z) {
    int chunkX, chunkZ, sectionY;
    blockToChunk(x, z, chunkX, chunkZ);
    blockYToSectionY(y, sectionY);
    auto key = std::make_tuple(chunkX, sectionY, chunkZ);
    auto it = g_chunkSectionInfoMap.find(key);
    if (it != g_chunkSectionInfoMap.end()) {
        return it->second.lodLevel;
    }
    return 1.0f; // 默认使用高精度, 或者可以考虑返回一个表示未找到的特殊值
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
    else if (currentBlock.level > -1) {
        return FLUID;
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

    // 从下到上遍历每一层(0表示最底层)
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

    // 设置ID(从上到下查找第一个对应类型)
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
        // 如果上方的类型不是空气,则将 level 设置为 0
        if (upperType != AIR) {
            *level = 0;

        }
        else {
            // 如果上方是空气,则返回当前块的层数
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

// 修改后的 IsRegionEmpty 方法:增加 isFluid 参数(默认为 false,用于固体判断)
bool IsRegionEmpty(int x, int y, int z, float lodSize) {
    int height;
    BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodSize, nullptr, &height);
    BlockType upperType = DetermineLODBlockType(x, y + lodSize, z, lodSize);
    if (config.useUnderwaterLOD)
    {
        if (type == BlockType::SOLID && height == 0)
        {
            return false;
        }
    }
    else
    {
        if ((type == BlockType::SOLID|| (type == BlockType::FLUID && upperType != AIR)) && height == 0)
        {
            return false;
        }
    }
    
    return true;
}

// 辅助函数:判断指定区域是否有效 
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

// 修改后的 IsRegionEmpty 方法:增加 isFluid 参数(默认为 false,用于固体判断)
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
// 辅助函数:判断指定区域是否有效 
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

// 修改后的 IsRegionEmpty 方法:增加 isFluid 参数(默认为 false,用于固体判断)
bool IsFluidTopRegionEmpty(int x, int y, int z, float lodSize, float h) {
    int height;
    BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodSize, nullptr, &height);
    if (((type == BlockType::SOLID) && height == 0) || type == BlockType::FLUID)
    {
        return false;
    }
    return true;
}
// 辅助函数:判断指定区域是否有效 
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
    case 0: // 底面(y-方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y - 1;
        dyEnd = y;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 1: // 顶面(y+方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y + baseSize;
        dyEnd = y + baseSize + 1;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 2: // 北面(z-方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z - 1;
        dzEnd = z;
        break;
    case 3: // 南面(z+方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z + baseSize;
        dzEnd = z + baseSize + 1;
        break;
    case 4: // 西面(x-方向)
        dxStart = x - 1;
        dxEnd = x;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 5: // 东面(x+方向)
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
                if (config.useUnderwaterLOD)
                {
                    if (type != SOLID) {
                        return false; // 发现非固体方块,不剔除该面
                    }
                }
                else
                {
                    if (type == BlockType::AIR || (baseSize == 1&& type == BlockType::FLUID))
                    {
                        return false;
                    }
                }
                
            }
        }
    }
    return true; // 区域全为固体,需要剔除该面
}

bool IsFluidFaceOccluded(int faceDir, int x, int y, int z, int baseSize) {
    int dxStart, dxEnd, dyStart, dyEnd, dzStart, dzEnd;

    // 根据面方向设置检测范围
    switch (faceDir) {
    case 0: // 底面(y-方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y - 1;
        dyEnd = y;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 1: // 顶面(y+方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y + baseSize;
        dyEnd = y + baseSize + 1;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 2: // 北面(z-方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z - 1;
        dzEnd = z;
        break;
    case 3: // 南面(z+方向)
        dxStart = x;
        dxEnd = x + baseSize;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z + baseSize;
        dzEnd = z + baseSize + 1;
        break;
    case 4: // 西面(x-方向)
        dxStart = x - 1;
        dxEnd = x;
        dyStart = y;
        dyEnd = y + baseSize;
        dzStart = z;
        dzEnd = z + baseSize;
        break;
    case 5: // 东面(x+方向)
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
                    return false; // 发现非固体方块,不剔除该面
                }
            }
        }
    }
    return true; // 区域全为固体,需要剔除该面
}


// 修改后的 GenerateBox,增加了 boxHeight 参数 
ModelData LODManager::GenerateBox(int x, int y, int z, int baseSize, float boxHeight,
    const std::vector<std::string>& colors) {
    ModelData box;

    float size = static_cast<float>(baseSize);
    float height = static_cast<float>(boxHeight);
    if (colors.size() == 1) {
        // 然后检查上方的 LOD 块 (y + 1)
        BlockType upperType = DetermineLODBlockType(x, y + baseSize, z, baseSize);
        // 如果上方的类型不是空气,则将 level 设置为 0
        if (upperType == AIR) {
            height = height - 0.1f;
        }

    }
    // 构造顶点数组,注意 y 方向使用 boxHeight
    box.vertices = {
        // 底面
        0.0f, 0.0f, 0.0f,
        size, 0.0f, 0.0f,
        size, 0.0f, size,
        0.0f, 0.0f, size,
        // 顶面(高度为 boxHeight)
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

    // 创建临时顶点索引数组,之后会用于创建 Face 结构体
    std::vector<int> tempVertexIndices = {
        0, 3, 2, 1,      // 底面
        4, 7, 6, 5,      // 顶面
        8, 11, 10, 9,    // 北面
        12, 13, 14, 15,  // 南面
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
    std::vector<int> materialIndices; // 临时材质索引数组
    
    if (colors.empty()) {
        Material defaultMaterial;
        defaultMaterial.name = "default_color";
        defaultMaterial.texturePath = "default_color";
        defaultMaterial.tintIndex = -1;
        box.materials = { defaultMaterial };
        materialIndices = { 0, 0, 0, 0, 0, 0 }; // 临时数组,用于后续创建 Face 结构体
    }
    else if (colors.size() == 1 || (colors.size() >= 2 && colors[0] == colors[1])) {
        Material singleMaterial;
        singleMaterial.name = colors[0];
        singleMaterial.texturePath = colors[0];
        singleMaterial.tintIndex = -1;
        box.materials = { singleMaterial };
        materialIndices = { 0, 0, 0, 0, 0, 0 }; // 临时数组,用于后续创建 Face 结构体
    }
    else {
        Material material1, material2;
        material1.name = colors[0];
        material1.texturePath = colors[0];
        material1.tintIndex = -1;
        
        material2.name = colors[1];
        material2.texturePath = colors[1];
        material2.tintIndex = -1;
        
        box.materials = { material1, material2 };
        materialIndices = { 1, 0, 1, 1, 1, 1 }; // 临时数组,用于后续创建 Face 结构体
    }

    // 调整模型位置
    ApplyPositionOffset(box, x, y, z);

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

    // 根据 validFaces 过滤不需要的面,并使用新的 Face 结构体
    ModelData filteredBox;
    filteredBox.vertices = box.vertices;
    filteredBox.uvCoordinates = box.uvCoordinates;
    filteredBox.materials = box.materials;
    
    // 定义 faceDirections,与索引对应
    FaceType faceDirections[6] = { DOWN, UP, NORTH, SOUTH, WEST, EAST };
    
    for (int faceIdx = 0; faceIdx < 6; ++faceIdx) {
        if (validFaces[faceIdx]) {
            Face face;
            // 设置顶点索引
            face.vertexIndices = {
                tempVertexIndices[faceIdx * 4],
                tempVertexIndices[faceIdx * 4 + 1],
                tempVertexIndices[faceIdx * 4 + 2],
                tempVertexIndices[faceIdx * 4 + 3]
            };
            
            // 设置UV索引
            int uvBase = faceIdx * 4;
            face.uvIndices = { uvBase, uvBase + 1, uvBase + 2, uvBase + 3 };
            
            // 设置材质索引和面方向
            face.materialIndex = materialIndices[faceIdx];
            face.faceDirection = faceDirections[faceIdx];
            
            // 添加到结果模型
            filteredBox.faces.push_back(face);
        }
    }
    return filteredBox;
}

// 检查方块是否应该使用原始模型
bool LODManager::ShouldUseOriginalModel(const std::string& blockName) {
    // 标准化方块名称（移除状态信息）
    std::string normalizedName = blockName;
    size_t bracketPos = normalizedName.find('[');
    if (bracketPos != std::string::npos) {
        normalizedName = normalizedName.substr(0, bracketPos);
    }
    
    // 检查是否在配置的lod1Blocks列表中
    return config.lod1Blocks.find(normalizedName) != config.lod1Blocks.end();
}