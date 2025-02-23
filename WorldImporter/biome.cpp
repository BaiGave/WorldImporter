#include "biome.h"
#include "block.h"
#include "coord_conversion.h"
#include <iostream>
#include <stdexcept>
#include <fstream>
#include <string>
// 初始化静态成员
std::unordered_map<std::string, int> Biome::biomeRegistry;
std::mutex Biome::registryMutex;
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

int Biome::GetId(const std::string& name) {
    std::lock_guard<std::mutex> lock(registryMutex);

    auto it = biomeRegistry.find(name);
    if (it != biomeRegistry.end()) {
        return it->second;
    }

    // 动态注册新群系
    int newId = static_cast<int>(biomeRegistry.size());
    biomeRegistry.emplace(name, newId);
    return newId;
}

void Biome::PrintAllRegisteredBiomes() {
    std::lock_guard<std::mutex> lock(registryMutex);

    std::cout << "已注册生物群系 (" << biomeRegistry.size() << " 个):\n";
    for (const auto& entry : biomeRegistry) {  // C++14兼容遍历方式
        std::cout << "  ID: " << entry.second
            << "\t名称: " << entry.first << "\n";
    }
}

// 实现生成群系图的方法
std::vector<std::vector<int>> Biome::GenerateBiomeMap(int minX, int minZ, int maxX, int maxZ, int y) {
    std::vector<std::vector<int>> biomeMap;
    biomeMap.reserve((maxX - minX + 1) * (maxZ - minZ + 1));

    // 遍历区域内的所有x和z坐标
    for (int x = minX; x <= maxX; ++x) {
        std::vector<int> row;
        row.reserve(maxZ - minZ + 1);
        for (int z = minZ; z <= maxZ; ++z) {
            int currentY = y;
            if (currentY == -1) {
                // 获取地面高度，若失败则使用默认值
                currentY = GetHeightMapY(x, z, "MOTION_BLOCKING_NO_LEAVES");
                if (currentY < -64) currentY = -64; // 限制y在合法范围内
                else if (currentY > 255) currentY = 255;
            }

            // 获取群系ID，自动处理区块加载
            int biomeId = GetBiomeId(x, currentY, z);
            row.push_back(biomeId);
        }
        biomeMap.push_back(row);
    }

    return biomeMap;
}

int GetBiomeId(int blockX, int blockY, int blockZ) {
    // 将世界坐标转换为区块坐标
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    // 计算当前区块在其区域中的相对位置
    int relativeChunkX = mod32(chunkX);
    int relativeChunkZ = mod32(chunkZ);

    // 将方块的Y坐标转换为子区块索引
    int sectionY;
    blockYToSectionY(blockY, sectionY);
    int adjustedSectionY = AdjustSectionY(sectionY);

    // 创建缓存键
    auto blockKey = std::make_tuple(relativeChunkX, relativeChunkZ, adjustedSectionY);

    // 检查缓存是否存在
    auto biomeCacheIter = biomeDataCache.find(blockKey);
    if (biomeCacheIter == biomeDataCache.end()) {
        // 若缓存不存在，自动加载区块数据
        LoadCacheBlockDataAutomatically(chunkX, chunkZ, sectionY);
        biomeCacheIter = biomeDataCache.find(blockKey);
        if (biomeCacheIter == biomeDataCache.end()) {
            return 0; // 返回默认群系ID
        }
    }

    // 计算区块内相对坐标
    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);

    // 计算YZX编码索引（16x16x16）
    int encodedIndex = toYZX(relativeX, relativeY, relativeZ);

    // 获取并返回群系ID
    const std::vector<int>& biomeData = biomeCacheIter->second;
    return (encodedIndex < biomeData.size()) ? biomeData[encodedIndex] : 0;
}