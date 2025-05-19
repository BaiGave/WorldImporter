#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image_write.h"
#include "include/stb_image.h"
#include "biome.h"
#include "block.h"
#include "GlobalCache.h"
#include "locutil.h"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <string>
#include "hashutils.h"

// 初始化静态成员
std::unordered_map<std::string, BiomeInfo> Biome::biomeRegistry;
std::shared_mutex Biome::registryMutex;

bool SaveColormapToFile(const std::vector<unsigned char>& pixelData,const std::string& namespaceName,const std::string& colormapName,std::string& savePath) {
    // 检查是否找到了纹理数据
    if (!pixelData.empty()) {

        // 获取当前工作目录(即 exe 所在的目录)
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string exePath = std::string(buffer);

        // 获取 exe 所在目录
        size_t pos = exePath.find_last_of("\\/");
        std::string exeDir = exePath.substr(0, pos);

        // 如果传入了 savePath, 则使用 savePath 作为保存目录,否则默认使用当前 exe 目录
        if (savePath.empty()) {
            savePath = exeDir + "\\colormap";  // 默认保存到 exe 目录下的 textures 文件夹
        }
        else {
            savePath = exeDir + "\\" + savePath;  // 使用提供的 savePath 路径
        }

        // 创建保存目录(如果不存在)
        if (GetFileAttributesA(savePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 文件夹不存在,创建它
            if (!CreateDirectoryA(savePath.c_str(), NULL)) {
                std::cerr << "Failed to create directory: " << savePath << std::endl;
                return false;
            }
        }

        // 处理 blockId,去掉路径部分,保留最后的文件名
        size_t lastSlashPos = colormapName.find_last_of("/\\");
        std::string fileName = (lastSlashPos == std::string::npos) ? colormapName : colormapName.substr(lastSlashPos + 1);

        // 构建保存路径,使用处理后的 blockId 作为文件名
        std::string filePath = savePath + "\\" + fileName + ".png";
        std::ofstream outputFile(filePath, std::ios::binary);

        //返回savePath,作为value
        savePath = filePath;

        if (outputFile.is_open()) {
            outputFile.write(reinterpret_cast<const char*>(pixelData.data()), pixelData.size());
        }
        else {
            std::cerr << "Failed to open output file!" << std::endl;
            return false;
        }
    }
    else {
        std::cerr << "Failed to retrieve texture for " << colormapName << std::endl;
        return false;
    }
}


int GetBiomeId(int blockX, int blockY, int blockZ) {
    // 将世界坐标转换为区块坐标
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    // 将方块的Y坐标转换为子区块索引
    int sectionY;
    blockYToSectionY(blockY, sectionY);

    // 创建缓存键
    auto blockKey = std::make_tuple(chunkX, chunkZ, sectionY);

    // 检查 SectionCache 中是否存在对应的区块数据
    if (sectionCache.find(blockKey) == sectionCache.end()) {
        LoadAndCacheBlockData(chunkX, chunkZ);
    }

    const auto& biomeData = sectionCache[blockKey].biomeData;

    int biomeX = mod16(blockX) / 4;
    int biomeY = mod16(blockY) / 4;
    int biomeZ = mod16(blockZ) / 4;

    // 计算编码索引(16y + 4z + x)
    int index = 16 * biomeY + 4 * biomeZ + biomeX;

    // 获取并返回群系ID
    return (index < biomeData.size()) ? biomeData[index] : 0;
}

nlohmann::json Biome::GetBiomeJson(const std::string& namespaceName, const std::string& biomeId) {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    // 按照 JAR 文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + biomeId;
        auto it = GlobalCache::biomes.find(cacheKey);
        if (it != GlobalCache::biomes.end()) {
            return it->second;
        }
    }

    std::cerr << "Biome JSON not found: " << namespaceName << ":" << biomeId << std::endl;
    return nlohmann::json();
}

std::string Biome::GetColormapData(const std::string& namespaceName, const std::string& colormapName) {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    // 按照 JAR 文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + colormapName;
        auto it = GlobalCache::colormaps.find(cacheKey);
        if (it != GlobalCache::colormaps.end()) {
            std::string filePath;
            if (SaveColormapToFile(it->second, namespaceName, colormapName, filePath)) {
                return filePath;
            }
            else {
                std::cerr << "Failed to save colormap: " << cacheKey << std::endl;
                return "";
            }
        }
    }

    std::cerr << "Colormap not found: " << namespaceName << ":" << colormapName << std::endl;
    return "";
}

BiomeColors Biome::ParseBiomeColors(const nlohmann::json& biomeJson) {
    BiomeColors colors;

    auto ParseColorWithFallback = [&](const std::string& key,
        const std::string& colormapType,
        float tempMod = 1.0f,
        float downfallMod = 1.0f) {
            int directColor = biomeJson["effects"].value(key, -1);
            if (directColor != -1) return directColor;

            auto colormap = GetColormapData("minecraft", colormapType);
            return CalculateColorFromColormap(colormap,colors.adjTemperature * tempMod,colors.adjDownfall * downfallMod);
        };


    // 检查 "temperature" 和 "downfall" 是否存在
    if (biomeJson.contains("temperature") && biomeJson.contains("downfall")) {
        const float temp = biomeJson["temperature"].get<float>();
        const float downfall = biomeJson["downfall"].get<float>();

        // 需要色图计算的参数准备
        colors.adjTemperature = BiomeUtils::clamp(temp, 0.0f, 1.0f);
        colors.adjDownfall = BiomeUtils::clamp(downfall, 0.0f, 1.0f);
    }
    else {
        // 如果没有提供温度和降水量,则设置默认值
        colors.adjTemperature = 0.5f;
        colors.adjDownfall = 0.5f;
    }

    // 检查 "effects" 是否存在
    if (biomeJson.contains("effects")) {
        const nlohmann::json& effects = biomeJson["effects"];

        // 解析直接颜色值
        colors.fog = effects.value("fog_color", 0xFFFFFF);
        colors.sky = effects.value("sky_color", 0x84ECFF);
        colors.water = effects.value("water_color", 0x3F76E4);
        colors.waterFog = effects.value("water_fog_color", 0x050533);
        
        // 统一处理植物颜色
        colors.foliage = ParseColorWithFallback("foliage_color", "foliage");
        
        // 干旱植物颜色处理 - 优先检查是否有直接颜色值
        int directDryFoliageColor = effects.value("dry_foliage_color", -1);
        if (directDryFoliageColor != -1) {
            // 如果有直接颜色值,直接使用
            colors.dryFoliage = directDryFoliageColor;
        } else {
            // 尝试使用dry_foliage.png文件
            auto dryFoliageColormap = GetColormapData("minecraft", "dry_foliage");
            if (!dryFoliageColormap.empty()) {
                // 如果找到dry_foliage.png,使用它来计算颜色
                colors.dryFoliage = CalculateColorFromColormap(dryFoliageColormap,
                    colors.adjTemperature,
                    colors.adjDownfall);
            } else {
                // 如果没有找到dry_foliage.png,回退到使用foliage.png和调整参数
                colors.dryFoliage = ParseColorWithFallback("dry_foliage_color", "foliage", 1.2f, 0.8f);
            }
        }
        
        colors.grass = ParseColorWithFallback("grass_color", "grass");
    }

    return colors;
}

int Biome::GetId(const std::string& fullName) {
    std::unique_lock<std::shared_mutex> lock(registryMutex);

    // 验证命名格式
    const size_t colonPos = fullName.find(':');
    if (colonPos == std::string::npos) {
        throw std::invalid_argument("Invalid biome format: " + fullName);
    }
    auto it = biomeRegistry.find(fullName);
    // 已存在直接返回
    if (it != biomeRegistry.end()) {
        return it->second.id;
    }

    // 获取生物群系配置数据
    const auto& biomeJson = GetBiomeJson(fullName.substr(0, colonPos), fullName.substr(colonPos + 1));

    // 提前解析颜色数据
    BiomeColors colors = ParseBiomeColors(biomeJson);

    // 原子化注册操作
    auto& newBiome = biomeRegistry.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(fullName),
        std::forward_as_tuple(
            static_cast<int>(biomeRegistry.size()),
            std::move(colors)
        )
    ).first->second;

    // 设置基础信息
    newBiome.namespaceName = fullName.substr(0, colonPos);
    newBiome.biomeName = fullName.substr(colonPos + 1);

    return newBiome.id;
}

int Biome::GetColor(int biomeId, BiomeColorType colorType) {
    // 共享读锁
    std::shared_lock<std::shared_mutex> lock(registryMutex);

    auto it = std::find_if(biomeRegistry.begin(), biomeRegistry.end(),
        [biomeId](const auto& entry) { return entry.second.id == biomeId; });

    if (it == biomeRegistry.end()) return 0xFFFFFF;

    // 获取独立颜色锁
    std::lock_guard<std::mutex> colorLock(it->second.colorMutex);
    switch (colorType) {
    case BiomeColorType::Foliage: return it->second.colors.foliage;
    case BiomeColorType::DryFoliage: return it->second.colors.dryFoliage;
    case BiomeColorType::Grass: return it->second.colors.grass;
    case BiomeColorType::Fog:    return it->second.colors.fog;
    case BiomeColorType::Sky:    return it->second.colors.sky;
    case BiomeColorType::Water:  return it->second.colors.water;
    case BiomeColorType::WaterFog: return it->second.colors.waterFog;
    default: return 0xFFFFFF; // 白色作为默认错误颜色
    }
}


int Biome::GetBiomeColor(int blockX, int blockY, int blockZ, BiomeColorType colorType) {
    // 生物群系过渡距离,默认值为4,可根据需要调整
    const int biomeTransitionDistance = 4;
    int count = 0;
    unsigned int rSum = 0, gSum = 0, bSum = 0;

    // 遍历以 (blockX, blockZ) 为中心、边长为 (2*biomeTransitionDistance + 1) 的正方形区域
    for (int dx = -biomeTransitionDistance; dx <= biomeTransitionDistance; dx++) {
        for (int dz = -biomeTransitionDistance; dz <= biomeTransitionDistance; dz++) {
            int curX = blockX + dx;
            int curZ = blockZ + dz;

            // 将世界坐标转换为区块坐标
            int chunkX, chunkZ;
            blockToChunk(curX, curZ, chunkX, chunkZ);

            // 将当前块的Y坐标转换为子区块索引(保持与原 Y 坐标一致)
            int sectionY;
            blockYToSectionY(blockY, sectionY);

            // 创建缓存键
            auto blockKey = std::make_tuple(chunkX, chunkZ, sectionY);

            // 检查 SectionCache 中是否存在对应的区块数据,否则加载缓存
            if (sectionCache.find(blockKey) == sectionCache.end()) {
                LoadAndCacheBlockData(chunkX, chunkZ);
            }

            const auto& biomeData = sectionCache[blockKey].biomeData;

            // 计算在子区块内的坐标,注意与生物群系数据排列有关
            int biomeX = mod16(curX) / 4;
            int biomeY = mod16(blockY) / 4;
            int biomeZ = mod16(curZ) / 4;
            int index = 16 * biomeY + 4 * biomeZ + biomeX;

            // 获取生物群系ID(若超出范围,则默认为0)
            int biomeId = (index < biomeData.size()) ? biomeData[index] : 0;

            // 共享读锁确保 biomeRegistry 的线程安全
            std::shared_lock<std::shared_mutex> lock(registryMutex);
            auto it = std::find_if(biomeRegistry.begin(), biomeRegistry.end(),
                [biomeId](const auto& entry) { return entry.second.id == biomeId; });

            // 默认颜色设置为白色
            int color = 0xFFFFFF;
            if (it != biomeRegistry.end()) {
                // 获取独立颜色锁,确保颜色数据读取安全
                std::lock_guard<std::mutex> colorLock(it->second.colorMutex);
                switch (colorType) {
                case BiomeColorType::Foliage:
                    color = it->second.colors.foliage;
                    break;
                case BiomeColorType::DryFoliage:
                    color = it->second.colors.dryFoliage;
                    break;
                case BiomeColorType::Grass:
                    color = it->second.colors.grass;
                    break;
                case BiomeColorType::Fog:
                    color = it->second.colors.fog;
                    break;
                case BiomeColorType::Sky:
                    color = it->second.colors.sky;
                    break;
                case BiomeColorType::Water:
                    color = it->second.colors.water;
                    break;
                case BiomeColorType::WaterFog:
                    color = it->second.colors.waterFog;
                    break;
                default:
                    color = 0xFFFFFF;
                    break;
                }
            }

            // 将颜色分解为RGB分量并累加
            int r = (color >> 16) & 0xFF;
            int g = (color >> 8) & 0xFF;
            int b = color & 0xFF;
            rSum += r;
            gSum += g;
            bSum += b;
            count++;
        }
    }

    // 计算各分量的平均值
    int avgR = rSum / count;
    int avgG = gSum / count;
    int avgB = bSum / count;
    // 组合回最终颜色值
    int finalColor = (avgR << 16) | (avgG << 8) | avgB;
    return finalColor;
}


int Biome::CalculateColorFromColormap(const std::string& filePath,float adjTemperature,float adjDownfall) {
    if (filePath.empty()) {
        return 0x00FF00; // 错误颜色
    }

    // 加载图片数据
    int width, height, channels;
    unsigned char* data = stbi_load(filePath.c_str(),
        &width,
        &height,
        &channels,
        0); // 0表示保留原始通道数

    if (!data) {
        std::cerr << "Failed to load colormap: " << filePath
            << ", error: " << stbi_failure_reason() << std::endl;
        return 0x00FF00;
    }

    // 验证尺寸
    if (width != 256 || height != 256) {
        std::cerr << "Invalid colormap size: " << filePath
            << " (expected 256x256, got "
            << width << "x" << height << ")" << std::endl;
        stbi_image_free(data);
        return 0x00FF00;
    }

    // 确保有足够的颜色通道
    const bool hasColorChannels = (channels >= 3);
    if (!hasColorChannels) {
        std::cerr << "Unsupported channel format: " << filePath
            << " (channels: " << channels << ")" << std::endl;
        stbi_image_free(data);
        return 0x00FF00;
    }

    // 温度和降水值钳位到0.0~1.0范围
    adjTemperature = BiomeUtils::clamp(adjTemperature, 0.0f, 1.0f);
    adjDownfall = BiomeUtils::clamp(adjDownfall, 0.0f, 1.0f);
    
    // 将降水值乘以温度值,确保在下三角形区域内
    adjDownfall = adjDownfall * adjTemperature;
    
    // 计算在颜色图中的坐标
    // 颜色图以右下角为原点:温度从右往左递增(1->0),降水从下往上递增(1->0)
    // 但图片坐标系以左上角为原点:x从左往右递增(0->255),y从上往下递增(0->255)
    
    // 温度映射:温度1.0对应x=0,温度0.0对应x=255
    int tempCoord = static_cast<int>((1.0f - adjTemperature) * 255.0f);
    
    // 降水映射:降水1.0对应y=0,降水0.0对应y=255
    int downfallCoord = static_cast<int>((1.0f - adjDownfall) * 255.0f);
    
    // 确保坐标在有效范围内
    tempCoord = BiomeUtils::clamp(tempCoord, 0, 255);
    downfallCoord = BiomeUtils::clamp(downfallCoord, 0, 255);
    
    // 在图片坐标系中,我们需要转换坐标
    // x:直接使用温度映射(已经是从左往右的递增)
    int x = tempCoord;
    
    // y:直接使用降水映射(已经是从上往下的递增)
    int y = downfallCoord;

    // 计算像素偏移 - 使用图片坐标系
    const size_t pixelOffset = (y * width + x) * channels;

    // 根据图片通道数提取颜色
    uint8_t r = data[pixelOffset];
    uint8_t g = data[pixelOffset + (channels >= 2 ? 1 : 0)]; // 单通道时复用R
    uint8_t b = data[pixelOffset + (channels >= 3 ? 2 : 0)]; // 双通道时复用R

    stbi_image_free(data);

    return (r << 16) | (g << 8) | b;
}


std::vector<std::vector<int>> Biome::GenerateBiomeMap(int minX, int minZ, int maxX, int maxZ) {
    std::vector<std::vector<int>> biomeMap;
    int width = maxX - minX + 1;
    int height = maxZ - minZ + 1;

    biomeMap.resize(height, std::vector<int>(width));

    for (int x = minX; x <= maxX; ++x) {
        for (int z = minZ; z <= maxZ; ++z) {
            int currentY = GetHeightMapY(x, z, "MOTION_BLOCKING");
            int biomeId = GetBiomeId(x, currentY, z);
            biomeMap[z - minZ][x - minX] = biomeId; // 修正坐标映射
        }
    }
    return biomeMap;
}


void Biome::ExportAllToPNG(int minX, int minZ, int maxX, int maxZ) {
    auto biomeMap = GenerateBiomeMap(minX, minZ, maxX, maxZ);
    // 导出图片
    Biome::ExportToPNG(biomeMap, "foliage.png", BiomeColorType::Foliage);
    Biome::ExportToPNG(biomeMap, "dry_foliage.png", BiomeColorType::DryFoliage);
    Biome::ExportToPNG(biomeMap, "water.png", BiomeColorType::Water);
    Biome::ExportToPNG(biomeMap, "grass.png", BiomeColorType::Grass);
    Biome::ExportToPNG(biomeMap, "waterFog.png", BiomeColorType::WaterFog);
    Biome::ExportToPNG(biomeMap, "fog.png", BiomeColorType::Fog);
    Biome::ExportToPNG(biomeMap, "sky.png", BiomeColorType::Sky);
}

bool Biome::ExportToPNG(const std::vector<std::vector<int>>& biomeMap,const std::string& filename,BiomeColorType colorType)
{
    // 生成颜色映射
    std::map<int, std::tuple<uint8_t, uint8_t, uint8_t>> colorMap;

    for (const auto& entry : biomeRegistry) {
        int colorValue = GetColor(entry.second.id, colorType);
        uint8_t r = (colorValue >> 16) & 0xFF;
        uint8_t g = (colorValue >> 8) & 0xFF;
        uint8_t b = colorValue & 0xFF;
        colorMap[entry.second.id] = std::make_tuple(r, g, b);
    }

    if (biomeMap.empty()) return false;

    const int height = biomeMap.size();
    const int width = biomeMap[0].size();

    // 尺寸校验
    for (const auto& row : biomeMap) {
        if (row.size() != static_cast<size_t>(width)) {
            std::cerr << "Error: Invalid biome map dimensions" << std::endl;
            return false;
        }
    }

    std::vector<uint8_t> imageData(width * height * 3);

    // 构建颜色映射(包含默认随机颜色生成)
    std::map<int, std::tuple<uint8_t, uint8_t, uint8_t>> finalColorMap = colorMap;

    // 解决方案:添加维度校验
    for (const auto& row : biomeMap) {
        if (row.size() != static_cast<size_t>(width)) {
            std::cerr << "Error: Biome map is not rectangular\n";
            return false;
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int biomeId = biomeMap[y][x];

            auto& color = finalColorMap[biomeId];
            int index = (y * width + x) * 3;
            imageData[index] = std::get<0>(color);  // R
            imageData[index + 1] = std::get<1>(color);  // G
            imageData[index + 2] = std::get<2>(color);  // B
        }
    }
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exePath(buffer);
    size_t pos = exePath.find_last_of("\\/");
    std::string exeDir = exePath.substr(0, pos);
    // 定义导出文件夹名称
    const std::string exportFolderName = "biomeTex";

    // 创建完整的文件夹路径
    std::string folderPath = exeDir + "\\" + exportFolderName;

    // 创建文件夹(如果不存在)
    BOOL folderCreated = CreateDirectoryA(folderPath.c_str(), NULL);
    if (!folderCreated && GetLastError() != ERROR_ALREADY_EXISTS) {
        std::cerr << "Error: Failed to create directory " << folderPath << std::endl;
        return false;
    }

    // 创建完整的文件路径
    std::string filePath = folderPath + "\\" + filename;    // 写入PNG文件
    return stbi_write_png(filePath.c_str(), width, height, 3, imageData.data(), width * 3);
}