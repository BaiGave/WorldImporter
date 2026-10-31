#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "include/stb_image_write.h"
#include "include/stb_image.h"
#include "biome.h"
#include "legacybiomes.h"
#include "fileutils.h"
#include "block.h"
#include "GlobalCache.h"
#include "locutil.h"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <string>
#include <cmath>
#include "hashutils.h"
#include "config.h"
#include "nbtutils.h"
#include "decompressor.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// 跨平台获取可执行文件目录的实现
static std::string getExecutableDir() {
    namespace fs = std::filesystem;
    fs::path exePath;

    try {
        // 获取可执行文件路径
#ifdef _WIN32
        // Windows平台（用 W 版 + 手动转换，避 fs::path 的编码问题）
        wchar_t buffer[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        }
        std::wstring ws(buffer);
        size_t p = ws.find_last_of(L"\\/");
        if (p != std::wstring::npos) ws.resize(p + 1); else ws.clear();
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string ret(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &ret[0], len, nullptr, nullptr);
            ret.resize(len - 1);
            return ret;
        }
        return ".";
#elif defined(__APPLE__)
        // macOS平台
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        exePath = fs::canonical(fs::path(buffer));
#else
        // Linux平台
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        buffer[len] = '\0';
        exePath = fs::path(buffer);
#endif
        // 获取父目录
        return exePath.parent_path().string() + PathSepStr();
    }
    catch (const std::exception& e) {
        std::cerr << "获取可执行文件目录失败: " << e.what() << std::endl;
        return fs::current_path().string() + PathSepStr(); // 失败时返回当前目录
    }
}

bool SaveColormapToFile(const std::vector<unsigned char>& pixelData,const std::string& namespaceName,const std::string& colormapName,std::string& savePath) {
    // 检查是否找到了纹理数据
    if (!pixelData.empty()) {

        // 获取当前工作目录(即 exe 所在的目录)
        std::string exePath = getExecutableDir();

        // 获取 exe 所在目录
        size_t pos = exePath.find_last_of("\\/");
        std::string exeDir = exePath.substr(0, pos);

        // 如果传入了 savePath, 则使用 savePath 作为保存目录,否则默认使用当前 exe 目录
        if (savePath.empty()) {
            savePath = JoinPath(exeDir, "colormap");  // 默认保存到 exe 目录下的 textures 文件夹
        }
        else {
            savePath = JoinPath(exeDir, savePath);  // 使用提供的 savePath 路径
        }

        // 创建保存目录(如果不存在)
        if (!std::filesystem::exists(savePath)) {
            // 文件夹不存在,创建它
            if (!std::filesystem::create_directories(savePath)) {
                std::cerr << "Failed to create directory: " << savePath << std::endl;
                return false;
            }
        }

        // 处理 blockId,去掉路径部分,保留最后的文件名
        size_t lastSlashPos = colormapName.find_last_of("/\\");
        std::string fileName = (lastSlashPos == std::string::npos) ? colormapName : colormapName.substr(lastSlashPos + 1);

        // 构建保存路径,使用处理后的 blockId 作为文件名
        std::string filePath = JoinPath(savePath, fileName + ".png");
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
    return true;
}

int GetBiomeId(int blockX, int blockY, int blockZ) {
    // 将世界坐标转换为区块坐标
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    // 将方块的Y坐标转换为子区块索引
    int sectionY;
    blockYToSectionY(blockY, sectionY);
    int adjustedSectionY = AdjustSectionY(sectionY); // 与 ProcessSection 存储 key 一致

    // 创建缓存键
    auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);

    // 检查 SectionCache 中是否存在对应的区块数据,如果没有则加载
    // 注意: 必须加锁访问 sectionCache, 且不能用 operator[] 插入(多线程并发写 unordered_map 会崩溃)
    {
        std::shared_lock<std::shared_mutex> sc_lock(sectionCacheMutex);
        if (!globalPaletteFrozen.load(std::memory_order_acquire) &&
            sectionCache.find(blockKey) == sectionCache.end()) {
            sc_lock.unlock();
            LoadAndCacheBlockData(chunkX, chunkZ);
        }
    }

    std::shared_lock<std::shared_mutex> sc_lock(sectionCacheMutex);
    auto secIt = sectionCache.find(blockKey);
    if (secIt == sectionCache.end()) return 0;
    const auto& biomeData = secIt->second.biomeData;

    int biomeX = mod16(blockX) / 4;
    int biomeY = mod16(blockY) / 4;
    int biomeZ = mod16(blockZ) / 4;

    // 计算编码索引(16y + 4z + x)
    int index = 16 * biomeY + 4 * biomeZ + biomeX;

    // 获取并返回群系ID
    return (index < biomeData.size()) ? biomeData[index] : 0;
}

// 初始化静态成员
std::unordered_map<std::string, BiomeInfo> Biome::biomeRegistry;
std::shared_mutex Biome::registryMutex;

nlohmann::json Biome::GetBiomeJson(const std::string& namespaceName, const std::string& biomeId) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);

    // 使用快速查找索引(O(1))
    std::string indexKey = std::string("biomes:") + namespaceName + ":" + biomeId;
    auto it = GlobalCache::biomeIndex.find(indexKey);
    if (it != GlobalCache::biomeIndex.end()) {
        auto cacheIt = GlobalCache::biomes.find(it->second);
        if (cacheIt != GlobalCache::biomes.end()) {
            return cacheIt->second;
        }
    }

    // 回退: 线性扫描
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + biomeId;
        auto fallbackIt = GlobalCache::biomes.find(cacheKey);
        if (fallbackIt != GlobalCache::biomes.end()) {
            return fallbackIt->second;
        }
    }

    // 1.18.x–1.19.2 的客户端 jar 没有群系 JSON。只在 jar/模组都未命中时回退。
    if (namespaceName == "minecraft") {
        nlohmann::json legacy = LegacyVanillaBiomeJson(biomeId);
        if (!legacy.is_null()) return legacy;
    }

    std::cerr << "Biome JSON not found: " << namespaceName << ":" << biomeId << std::endl;
    return nlohmann::json();
}

std::string Biome::GetColormapData(const std::string& namespaceName, const std::string& colormapName) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);

    // 使用快速查找索引(O(1))
    std::string indexKey = std::string("colormaps:") + namespaceName + ":" + colormapName;
    auto idxIt = GlobalCache::colormapIndex.find(indexKey);
    if (idxIt != GlobalCache::colormapIndex.end()) {
        auto it = GlobalCache::colormaps.find(idxIt->second);
        if (it != GlobalCache::colormaps.end()) {
            std::string filePath;
            if (SaveColormapToFile(it->second, namespaceName, colormapName, filePath)) {
                return filePath;
            }
            else {
                std::cerr << "Failed to save colormap: " << idxIt->second << std::endl;
                return "";
            }
        }
    }

    // 回退: 线性扫描
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

    auto safeColorVal = [](const nlohmann::json& j, const std::string& k, int def) -> int {
        if (!j.is_object() || !j.contains(k)) return def;
        const auto& v = j[k];
        if (v.is_number()) return v.get<int>();
        // 1.21.11 RGB codec also accepts three normalized numeric channels.
        if (v.is_array() && v.size() == 3) {
            int color = 0;
            for (const auto& channel : v) {
                if (!channel.is_number()) return def;
                const double value = channel.get<double>();
                if (!std::isfinite(value) || value < 0.0 || value > 1.0) return def;
                color = (color << 8) | static_cast<int>(value * 255.0);
            }
            return color;
        }
        if (v.is_string()) {
            const auto& str = v.get_ref<const std::string&>();
            try {
                if (!str.empty() && str[0] == '#') {
                    if (str.size() != 7 || str.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
                        return def;
                    return static_cast<int>(std::stoul(str.substr(1), nullptr, 16));
                }
                // Preserve legacy decimal / 0x-prefixed string parsing.
                return static_cast<int>(std::stoul(str, nullptr, 0));
            } catch (const std::exception&) {}
        }
        return def;
    };

    const nlohmann::json emptyObject = nlohmann::json::object();
    const bool hasEffects = biomeJson.contains("effects");
    const auto& effects = hasEffects ? biomeJson["effects"] : emptyObject;
    const auto& attributes = biomeJson.contains("attributes") ? biomeJson["attributes"] : emptyObject;
    auto visualColor = [&](const std::string& key, int def) -> int {
        const int legacy = safeColorVal(effects, key, -1);
        if (legacy != -1) return legacy;

        const std::string attributeKey = "minecraft:visual/" + key;
        if (!attributes.is_object() || !attributes.contains(attributeKey)) return def;
        const auto& attribute = attributes[attributeKey];
        if (attribute.is_object()) {
            // An explicit override (or omitted modifier) needs no inherited value.
            // Other modifiers require dimension context, which this parser lacks.
            if (attribute.contains("modifier") && attribute["modifier"] != "override") return def;
            return safeColorVal(attribute, "argument", def);
        }
        return safeColorVal(attributes, attributeKey, def);
    };

    // Resolve before the no-effects return: attributes-only biomes are valid too.
    // Keep WI's existing no-effects fog default instead of the old missing-key white.
    colors.fog = visualColor("fog_color", 0xC0D8FF);
    colors.sky = visualColor("sky_color", hasEffects ? 0x84ECFF : 0x78A7FF);
    colors.waterFog = visualColor("water_fog_color", hasEffects ? 0x050533 : 0x3F76E4);

    // 空/无效 json 保护: 避免访问不存在的 effects 键抛异常, 导致生物群系注册失败
    if (biomeJson.is_null() || biomeJson.empty() || !biomeJson.contains("effects")) {
        // 使用默认生物群系颜色(中性绿/蓝)
        colors.grass = 0x7CBD6B;
        colors.foliage = 0x7CBD6B;
        colors.dryFoliage = 0x7CBD6B;
        colors.water = 0x3F76E4;
        colors.adjTemperature = 0.5f;
        colors.adjDownfall = 0.5f;
        return colors;
    }

    auto ParseColorWithFallback = [&](const std::string& key,
        const std::string& colormapType,
        float tempMod = 1.0f,
        float downfallMod = 1.0f) {
            int directColor = safeColorVal(effects, key, -1);
            if (directColor != -1) return directColor;

            auto colormap = GetColormapData("minecraft", colormapType);
            return CalculateColorFromColormap(colormap,colors.adjTemperature * tempMod,colors.adjDownfall * downfallMod);
        };


    // 检查 "temperature" 和 "downfall" 是否存在
    if (biomeJson.contains("temperature") && biomeJson.contains("downfall")) {
        auto safeFloat = [](const nlohmann::json& j, float def) {
            if (j.is_number()) return j.get<float>();
            if (j.is_string()) { try { return std::stof(j.get<std::string>()); } catch(...) {} }
            return def;
        };
        const float temp = safeFloat(biomeJson["temperature"], 0.5f);
        const float downfall = safeFloat(biomeJson["downfall"], 0.5f);

        // 需要色图计算的参数准备
        colors.adjTemperature = BiomeUtils::clamp(temp, 0.0f, 1.0f);
        colors.adjDownfall = BiomeUtils::clamp(downfall, 0.0f, 1.0f);
    }
    else {
        // 如果没有提供温度和降水量,则设置默认值
        colors.adjTemperature = 0.5f;
        colors.adjDownfall = 0.5f;
    }

    // 当 JSON 没有 effects 时，用 colormap + 默认温降计算基础颜色
    if (!biomeJson.contains("effects") || biomeJson["effects"].empty()) {
        auto calcDefault = [&](const std::string& cmap, int defColor) -> int {
            auto data = GetColormapData("minecraft", cmap);
            if (!data.empty()) {
                return CalculateColorFromColormap(data, colors.adjTemperature, colors.adjDownfall);
            }
            return defColor;
        };
        colors.foliage = calcDefault("foliage", -1);
        colors.grass = calcDefault("grass", -1);
        colors.dryFoliage = calcDefault("foliage", -1);
    }
    // 检查 "effects" 是否存在
    if (biomeJson.contains("effects")) {
        // 解析直接颜色值
        colors.water = safeColorVal(effects, "water_color", 0x3F76E4);

        // 统一处理植物颜色
        colors.foliage = ParseColorWithFallback("foliage_color", "foliage");

        // 干旱植物颜色处理 - 优先检查是否有直接颜色值
        int directDryFoliageColor = safeColorVal(effects, "dry_foliage_color", -1);
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

        // grass_color_modifier (vanilla GrassColorModifier). dark_forest is a
        // position-independent average with 0x28340A, baked here. swamp depends on
        // BIOME_INFO_NOISE at the block column: keep 0x6A7039 for LOD/GetColor and
        // set swampGrass so the tint export substitutes BiomeSwampGrassColor.
        if (effects.contains("grass_color_modifier") && effects["grass_color_modifier"].is_string()) {
            const std::string mod = effects["grass_color_modifier"].get<std::string>();
            if (mod == "swamp") {
                colors.grass = 0x6A7039;
                colors.swampGrass = true;
            } else if (mod == "dark_forest") {
                colors.grass = ((colors.grass & 0xFEFEFE) + 0x28340A) >> 1;
            }
        }
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

            // sectionCache 使用 AdjustSectionY(sectionY) 作为键；必须与
            // ProcessSection/ GetBlockId 保持一致，否则这里会误判区块未加载，
            // 在模型线程中触发 LoadAndCacheBlockData，造成并发写缓存和堆损坏。
            int adjustedSectionY = AdjustSectionY(sectionY);
            auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);

            // 模型阶段批次及一圈边界已完整预加载。此处只读，缺失时使用默认色；
            // 绝不能在模型线程里惰性 LoadAndCacheBlockData，否则会与 GetBlockId/
            // GetSkyLight 的高频无锁读取并发写 sectionCache，触发堆损坏。
            std::shared_lock<std::shared_mutex> sc_lock(sectionCacheMutex);
            auto secIt = sectionCache.find(blockKey);
            if (secIt == sectionCache.end()) continue;
            const auto& biomeData = secIt->second.biomeData;

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
    if (count == 0) return 0xFFFFFF; // No loaded samples: neutral multiplicative tint.
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

// 添加全局地图的最小X和最小Z坐标，用于计算偏移量
int g_biomeMapMinX = 0;
int g_biomeMapMinZ = 0;

namespace {

constexpr int kBlendChannels = 7;

int BlendChannel(BiomeColorType type) {
    switch (type) {
    case BiomeColorType::Foliage: return 0;
    case BiomeColorType::DryFoliage: return 1;
    case BiomeColorType::Grass: return 2;
    case BiomeColorType::Fog: return 3;
    case BiomeColorType::Sky: return 4;
    case BiomeColorType::Water: return 5;
    case BiomeColorType::WaterFog: return 6;
    }
    return -1;
}

// Per-pixel blended colours filled by GenerateBiomeMap while sectionCache still
// holds the batch (plus its one-chunk ring). ExportToPNG runs after that cache
// is dropped, so it only writes these buffers. ready == 0 keeps the id-to-colour
// path used by tests that never call GenerateBiomeMap. 0xFFFFFF is a legal tint,
// so a colour sentinel cannot mark "not computed".
std::vector<uint8_t> g_blendReady;
std::vector<uint32_t> g_blendColor[kBlendChannels];
bool g_seedReady = false;
bool g_hasZoom = false;
int64_t g_zoomSeed = 0;

struct TintLut {
    std::vector<int> color[kBlendChannels];
    std::vector<uint8_t> swamp;
};

bool IsBlockTintChannel(int channel) {
    return channel == 0 || channel == 1 || channel == 2 || channel == 5;
}

int LutColor(const TintLut& lut, int channel, int biomeId, int sampleX, int sampleZ) {
    if (channel == 2 && biomeId >= 0 && biomeId < static_cast<int>(lut.swamp.size()) && lut.swamp[biomeId]) {
        return BiomeSwampGrassColor(sampleX, sampleZ);
    }
    if (biomeId < 0 || biomeId >= static_cast<int>(lut.color[channel].size())) return 0xFFFFFF;
    return lut.color[channel][biomeId];
}

struct SecKey {
    int cx, cz, sy;
    bool operator==(const SecKey& other) const {
        return cx == other.cx && cz == other.cz && sy == other.sy;
    }
};

struct SecKeyHash {
    size_t operator()(const SecKey& key) const {
        size_t h = std::hash<int>{}(key.cx);
        h ^= std::hash<int>{}(key.cz) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.sy) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct SecSnap {
    bool present = false;
    std::vector<int> biomes;
};

struct SampleKey {
    int x, y, z;
    bool operator==(const SampleKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct SampleKeyHash {
    size_t operator()(const SampleKey& key) const {
        size_t h = std::hash<int>{}(key.x);
        h ^= std::hash<int>{}(key.z) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(key.y) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

struct Resolved {
    bool ok = false;
    int id = 0;
    int x = 0;
    int z = 0;
};

// Copies section biome palettes out of sectionCache. Does not insert into the
// shared cache and does not call LoadAndCacheBlockData: the palette is frozen
// while the tint map is built, and a missing section is a real gap (Y range or
// a column past the one-chunk preload), not a request to parse another chunk.
class BiomeSampler {
public:
    bool TryAt(int blockX, int blockY, int blockZ, int& biomeId) {
        int chunkX = 0, chunkZ = 0;
        blockToChunk(blockX, blockZ, chunkX, chunkZ);
        int sectionY = 0;
        blockYToSectionY(blockY, sectionY);
        const SecKey key{chunkX, chunkZ, AdjustSectionY(sectionY)};
        auto found = sections.find(key);
        if (found == sections.end()) {
            SecSnap snap;
            {
                std::shared_lock<std::shared_mutex> lock(sectionCacheMutex);
                auto cached = sectionCache.find(std::make_tuple(chunkX, chunkZ, key.sy));
                if (cached != sectionCache.end()) {
                    snap.present = true;
                    snap.biomes = cached->second.biomeData;
                }
            }
            found = sections.emplace(key, std::move(snap)).first;
        }
        const SecSnap& snap = found->second;
        if (!snap.present) return false;
        const int biomeX = mod16(blockX) / 4;
        const int biomeY = mod16(blockY) / 4;
        const int biomeZ = mod16(blockZ) / 4;
        const int index = 16 * biomeY + 4 * biomeZ + biomeX;
        biomeId = (index >= 0 && static_cast<size_t>(index) < snap.biomes.size()) ? snap.biomes[index] : 0;
        return true;
    }

    // Winner-first quart lookup. If that section is loaded the other seven
    // corners are not consulted (the distance winner is unique in the vanilla
    // strict-> rule). If it is missing, the nearest loaded corner is used.
    // Missing corners are treated as infinite distance. Ties keep the lower index.
    Resolved Resolve(int x, int y, int z) {
        const SampleKey key{x, y, z};
        auto cached = samples.find(key);
        if (cached != samples.end()) return cached->second;

        Resolved resolved;
        resolved.x = x;
        resolved.z = z;
        if (!g_hasZoom) {
            int biomeId = 0;
            if (TryAt(x, y, z, biomeId)) {
                resolved.ok = true;
                resolved.id = biomeId;
            }
            samples.emplace(key, resolved);
            return resolved;
        }

        const int shiftedX = x - 2;
        const int shiftedY = y - 2;
        const int shiftedZ = z - 2;
        const int parentX = shiftedX >> 2;
        const int parentY = shiftedY >> 2;
        const int parentZ = shiftedZ >> 2;
        const double fractX = static_cast<double>(shiftedX & 3) / 4.0;
        const double fractY = static_cast<double>(shiftedY & 3) / 4.0;
        const double fractZ = static_cast<double>(shiftedZ & 3) / 4.0;

        int cornerX[8], cornerY[8], cornerZ[8];
        double cornerDist[8];
        int winner = 0;
        double bestDist = std::numeric_limits<double>::infinity();
        for (int i = 0; i < 8; ++i) {
            const bool xEven = (i & 4) == 0;
            const bool yEven = (i & 2) == 0;
            const bool zEven = (i & 1) == 0;
            cornerX[i] = xEven ? parentX : parentX + 1;
            cornerY[i] = yEven ? parentY : parentY + 1;
            cornerZ[i] = zEven ? parentZ : parentZ + 1;
            cornerDist[i] = BiomeFiddledDistance(
                g_zoomSeed, cornerX[i], cornerY[i], cornerZ[i],
                xEven ? fractX : fractX - 1.0,
                yEven ? fractY : fractY - 1.0,
                zEven ? fractZ : fractZ - 1.0);
            if (bestDist > cornerDist[i]) {
                bestDist = cornerDist[i];
                winner = i;
            }
        }

        int biomeId = 0;
        if (TryAt(cornerX[winner] << 2, cornerY[winner] << 2, cornerZ[winner] << 2, biomeId)) {
            resolved.ok = true;
            resolved.id = biomeId;
            samples.emplace(key, resolved);
            return resolved;
        }

        int fallback = -1;
        double fallbackDist = std::numeric_limits<double>::infinity();
        for (int i = 0; i < 8; ++i) {
            if (i == winner) continue;
            if (!(fallbackDist > cornerDist[i])) continue;
            int candidate = 0;
            if (!TryAt(cornerX[i] << 2, cornerY[i] << 2, cornerZ[i] << 2, candidate)) continue;
            fallback = i;
            fallbackDist = cornerDist[i];
            biomeId = candidate;
        }
        if (fallback >= 0) {
            resolved.ok = true;
            resolved.id = biomeId;
        }
        samples.emplace(key, resolved);
        return resolved;
    }

private:
    std::unordered_map<SecKey, SecSnap, SecKeyHash> sections;
    std::unordered_map<SampleKey, Resolved, SampleKeyHash> samples;
};

// Neighbours outside the loaded set: clamp X/Z into the export rectangle and
// sample that column at the same Y. The batch loader preloads one chunk (16
// blocks) around the batch, which covers the default radius 2 and the fuzz
// quart in XZ. A quart can still sit one section above or below the surface
// block; that section is used only when the export Y range already loaded it.
// If the clamped column is also missing, the caller reuses the center column.
// Swamp noise uses the coordinates of the sample that actually supplied the id.
Resolved SampleColumn(BiomeSampler& sampler, int x, int y, int z, int minX, int maxX, int minZ, int maxZ) {
    Resolved resolved = sampler.Resolve(x, y, z);
    if (resolved.ok) return resolved;
    const int clampedX = std::clamp(x, minX, maxX);
    const int clampedZ = std::clamp(z, minZ, maxZ);
    if (clampedX != x || clampedZ != z) {
        resolved = sampler.Resolve(clampedX, y, clampedZ);
    }
    return resolved;
}

bool ReadFileBytes(const std::string& path, std::vector<char>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > 8 * 1024 * 1024) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    in.read(out.data(), size);
    return static_cast<size_t>(in.gcount()) == out.size();
}

bool ParseNbtRoot(const std::vector<char>& file, NbtTagPtr& root) {
    const std::vector<char>* source = &file;
    std::vector<char> inflated;
    if (file.size() >= 2 && static_cast<unsigned char>(file[0]) == 0x1f &&
        static_cast<unsigned char>(file[1]) == 0x8b) {
        if (!DecompressGzip(file, inflated)) return false;
        source = &inflated;
    }
    try {
        size_t index = 0;
        root = readTag(*source, index);
        return static_cast<bool>(root);
    } catch (...) {
        return false;
    }
}

bool ChildLong(const NbtTagPtr& parent, const char* name, int64_t& out) {
    if (!parent) return false;
    const NbtTagPtr child = getChildByName(parent, name);
    if (!child || child->type != TagType::LONG || child->payload.size() < 8) return false;
    out = bytesToLong(child->payload);
    return true;
}

bool SeedFromNbtFile(const std::string& path, int64_t& out) {
    std::vector<char> bytes;
    if (!ReadFileBytes(path, bytes)) return false;
    NbtTagPtr root;
    if (!ParseNbtRoot(bytes, root)) return false;
    // 1.18-1.21 level.dat: Data.WorldGenSettings.seed
    const NbtTagPtr data = getChildByName(root, "Data");
    const NbtTagPtr settings = data ? getChildByName(data, "WorldGenSettings") : nullptr;
    if (ChildLong(settings, "seed", out)) return true;
    // 26.x: world/data/minecraft/world_gen_settings.dat compound "data".seed
    const NbtTagPtr data26 = getChildByName(root, "data");
    if (ChildLong(data26, "seed", out)) return true;
    return ChildLong(root, "seed", out);
}

bool ReadWorldSeed(int64_t& out) {
    if (config.worldPath.empty()) return false;
    if (SeedFromNbtFile(JoinPath(config.worldPath, "level.dat"), out)) return true;
    return SeedFromNbtFile(JoinPath(config.worldPath, "data", "minecraft", "world_gen_settings.dat"), out);
}

void EnsureZoomSeed() {
    if (g_seedReady) return;
    g_seedReady = true;
    g_hasZoom = false;
    int64_t worldSeed = 0;
    if (ReadWorldSeed(worldSeed)) {
        g_zoomSeed = BiomeObfuscateSeed(worldSeed);
        g_hasZoom = true;
        std::cout << "Biome blend: world seed " << worldSeed << ", zoom seed " << g_zoomSeed
                  << ", radius " << std::clamp(config.biomeBlendRadius, 0, 32) << std::endl;
    } else {
        std::cout << "Biome blend: world seed unavailable, raw quart biomes, radius "
                  << std::clamp(config.biomeBlendRadius, 0, 32) << std::endl;
    }
}

}  // namespace

// 初始化生物群系地图尺寸和偏移量
void Biome::InitializeBiomeMap(int minX, int minZ, int maxX, int maxZ) {
    int width = maxX - minX + 1;
    int height = maxZ - minZ + 1;
    g_biomeMap.assign(height, std::vector<int>(width));
    g_biomeMapMinX = minX;
    g_biomeMapMinZ = minZ;
    const size_t pixels = (width > 0 && height > 0)
        ? static_cast<size_t>(width) * static_cast<size_t>(height)
        : 0;
    g_blendReady.assign(pixels, 0);
    for (int channel = 0; channel < kBlendChannels; ++channel) g_blendColor[channel].assign(pixels, 0);
    g_seedReady = false;
    g_hasZoom = false;
    g_zoomSeed = 0;
}

void Biome::FillBiomeExportLut(std::vector<int> (&colors)[7], std::vector<uint8_t>& swamp) {
    std::shared_lock<std::shared_mutex> lock(registryMutex);
    int maxId = 0;
    for (const auto& entry : biomeRegistry) {
        if (entry.second.id > maxId) maxId = entry.second.id;
    }
    for (int channel = 0; channel < 7; ++channel) {
        colors[channel].assign(static_cast<size_t>(maxId) + 1, 0xFFFFFF);
    }
    swamp.assign(static_cast<size_t>(maxId) + 1, 0);
    for (const auto& entry : biomeRegistry) {
        const int id = entry.second.id;
        if (id < 0) continue;
        std::lock_guard<std::mutex> colorLock(entry.second.colorMutex);
        const BiomeColors& biomeColors = entry.second.colors;
        colors[0][id] = biomeColors.foliage;
        colors[1][id] = biomeColors.dryFoliage;
        colors[2][id] = biomeColors.grass;
        colors[3][id] = biomeColors.fog;
        colors[4][id] = biomeColors.sky;
        colors[5][id] = biomeColors.water;
        colors[6][id] = biomeColors.waterFog;
        swamp[id] = biomeColors.swampGrass ? 1 : 0;
    }
}

void Biome::GenerateBiomeMap(int minX, int minZ, int maxX, int maxZ) {
    // 确保全局地图已初始化
    if (g_biomeMap.empty() || g_biomeMap[0].empty()) {
        std::cerr << "Error: g_biomeMap not initialized in GenerateBiomeMap!\n";
        return;
    }

    EnsureZoomSeed();
    TintLut lut;
    FillBiomeExportLut(lut.color, lut.swamp);
    BiomeSampler sampler;

    int radius = config.biomeBlendRadius;
    if (radius < 0) radius = 0;
    if (radius > 32) radius = 32;
    const int sampleCount = (radius * 2 + 1) * (radius * 2 + 1);

    const int globalMinX = g_biomeMapMinX;
    const int globalMinZ = g_biomeMapMinZ;
    const int globalMaxX = globalMinX + static_cast<int>(g_biomeMap[0].size()) - 1;
    const int globalMaxZ = globalMinZ + static_cast<int>(g_biomeMap.size()) - 1;
    const size_t mapWidth = g_biomeMap[0].size();
    const size_t mapHeight = g_biomeMap.size();

    // Every neighbour is sampled at this column's surface Y, matching
    // ClientLevel.calculateBlockTint (Cursor3D keeps Y fixed). The blended
    // colour is the toward-zero mean of all (2r+1)^2 samples, including ones
    // substituted by the clamp / center fallback, so the divisor stays full.
    for (int x = minX; x <= maxX; ++x) {
        for (int z = minZ; z <= maxZ; ++z) {
            const int mapX = x - globalMinX;
            const int mapZ = z - globalMinZ;
            if (mapX < 0 || mapZ < 0 || static_cast<size_t>(mapX) >= mapWidth ||
                static_cast<size_t>(mapZ) >= mapHeight) {
                continue;
            }

            // 高度图存的是 (最高方块Y + 1) - 世界最低Y, 需换算成世界Y再取表面方块所在群系
            const int hm = GetHeightMapY(x, z, "MOTION_BLOCKING");
            const int currentY = hm > 0 ? hm - 1 + minSectionY * 16 : hm;

            Resolved center = SampleColumn(sampler, x, currentY, z, globalMinX, globalMaxX, globalMinZ, globalMaxZ);
            if (!center.ok) {
                center.ok = true;
                center.id = 0;
                center.x = x;
                center.z = z;
            }
            g_biomeMap[mapZ][mapX] = center.id;

            int red[kBlendChannels] = {};
            int green[kBlendChannels] = {};
            int blue[kBlendChannels] = {};
            for (int dz = -radius; dz <= radius; ++dz) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    Resolved sample = SampleColumn(
                        sampler, x + dx, currentY, z + dz,
                        globalMinX, globalMaxX, globalMinZ, globalMaxZ);
                    if (!sample.ok) sample = center;
                    for (int channel = 0; channel < kBlendChannels; ++channel) {
                        // 原版只有方块着色（草/树叶/干树叶/水）走 calculateBlockTint 的方形平均；
                        // 天空/雾/水雾由 CubicSampler 另行处理，这里取本格群系，不做方形平均。
                        const Resolved& src = IsBlockTintChannel(channel) ? sample : center;
                        const int color = LutColor(lut, channel, src.id, src.x, src.z);
                        red[channel] += (color >> 16) & 0xFF;
                        green[channel] += (color >> 8) & 0xFF;
                        blue[channel] += color & 0xFF;
                    }
                }
            }

            const size_t pixel = static_cast<size_t>(mapZ) * mapWidth + static_cast<size_t>(mapX);
            if (pixel < g_blendReady.size()) {
                g_blendReady[pixel] = 1;
                for (int channel = 0; channel < kBlendChannels; ++channel) {
                    const int blended =
                        ((red[channel] / sampleCount) & 0xFF) << 16 |
                        ((green[channel] / sampleCount) & 0xFF) << 8 |
                        ((blue[channel] / sampleCount) & 0xFF);
                    g_blendColor[channel][pixel] = static_cast<uint32_t>(blended);
                }
            }
        }
    }
}

bool Biome::ExportToPNG(const std::string& filename, BiomeColorType colorType)
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

    if (g_biomeMap.empty()) return false;

    const int height = g_biomeMap.size();
    const int width = g_biomeMap[0].size();

    // 尺寸校验
    for (const auto& row : g_biomeMap) {
        if (row.size() != static_cast<size_t>(width)) {
            std::cerr << "Error: Invalid biome map dimensions" << std::endl;
            return false;
        }
    }

    std::vector<uint8_t> imageData(width * height * 3);

    // 构建颜色映射(包含默认随机颜色生成)
    std::map<int, std::tuple<uint8_t, uint8_t, uint8_t>> finalColorMap = colorMap;

    // 解决方案:添加维度校验
    for (const auto& row : g_biomeMap) {
        if (row.size() != static_cast<size_t>(width)) {
            std::cerr << "Error: Biome map is not rectangular\n";
            return false;
        }
    }
    const int blendChannel = BlendChannel(colorType);
    const bool haveBlend = blendChannel >= 0 &&
        g_blendReady.size() == static_cast<size_t>(width) * static_cast<size_t>(height) &&
        g_blendColor[blendChannel].size() == g_blendReady.size();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t pixel = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const int index = (y * width + x) * 3;
            if (haveBlend && g_blendReady[pixel]) {
                const uint32_t blended = g_blendColor[blendChannel][pixel];
                imageData[index] = static_cast<uint8_t>((blended >> 16) & 0xFF);
                imageData[index + 1] = static_cast<uint8_t>((blended >> 8) & 0xFF);
                imageData[index + 2] = static_cast<uint8_t>(blended & 0xFF);
                continue;
            }
            int biomeId = g_biomeMap[y][x];
            auto& color = finalColorMap[biomeId];
            imageData[index] = std::get<0>(color);  // R
            imageData[index + 1] = std::get<1>(color);  // G
            imageData[index + 2] = std::get<2>(color);  // B
        }
    }
    std::string exePath = getExecutableDir();
    size_t pos = exePath.find_last_of("\\/");
    std::string exeDir = exePath.substr(0, pos);
    // 定义导出文件夹名称
    const std::string exportFolderName = "biomeTex";

    // 创建完整的文件夹路径
    std::string folderPath = JoinPath(exeDir, exportFolderName);

    // 创建文件夹(如果不存在)
    if (!std::filesystem::exists(folderPath)) {
        if (!std::filesystem::create_directory(folderPath)) {
            std::cerr << "Error: Failed to create directory " << folderPath << std::endl;
            return false;
        }
    }

    // 创建完整的文件路径
    std::string filePath = JoinPath(folderPath, filename);    // 写入PNG文件
    return stbi_write_png(filePath.c_str(), width, height, 3, imageData.data(), width * 3);
}
