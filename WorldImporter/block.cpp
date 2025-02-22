#include <iostream>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <string>
#include <sstream>
#include <memory>
#include "block.h"
#include "model.h"
#include "blockstate.h"
#include "nbtutils.h"
#include "biome.h"
#include "fileutils.h"
#include "decompressor.h"
#include "coord_conversion.h"
#include "config.h"
#include <chrono>
#include <fstream>
#include <locale>
#include <codecvt>
#include <random>
#include <algorithm>  // added for find_if

using namespace std;

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

// --------------------------------------------------------------------------------
// 文件缓存相关对象
// --------------------------------------------------------------------------------
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache;
std::unordered_map<std::pair<int, int>, std::shared_ptr<NbtTag>, pair_hash> chunkCache;
std::unordered_map<std::tuple<int, int, int>, std::vector<std::string>, triple_hash> blockPaletteCache;
std::unordered_map<std::tuple<int, int, int>, std::vector<int>, triple_hash> blockDataCache;
std::unordered_map<std::tuple<int, int, int>, std::vector<int>, triple_hash> biomeDataCache;
std::unordered_map<std::pair<int, int>, std::unordered_map<std::string, std::vector<int>>, pair_hash> heightMapCache;
std::vector<Block> globalBlockPalette;
std::unordered_set<std::string> solidBlocks;

// --------------------------------------------------------------------------------
// 文件操作相关函数
// --------------------------------------------------------------------------------
std::vector<int> decodeHeightMap(const std::vector<int64_t>& data) {
    // 根据数据长度自动判断存储格式
    int bitsPerEntry = (data.size() == 37) ? 9 : 8; // 主世界37个long用9bit，其他32个用8bit
    int entriesPerLong = 64 / bitsPerEntry;
    int mask = (1 << bitsPerEntry) - 1;
    std::vector<int> heights;

    for (const auto& longVal : data) {
        int64_t value = reverseEndian(longVal);
        for (int i = 0; i < entriesPerLong; ++i) {
            int height = static_cast<int>((value >> (i * bitsPerEntry)) & mask);
            heights.push_back(height);
            if (heights.size() >= 256) break;
        }
        if (heights.size() >= 256) break;
    }

    heights.resize(256);
    return heights;
}

std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z) {
    unsigned offset = CalculateOffset(fileData, x, z);
    if (offset == 0) {
        cerr << "错误: 偏移计算失败。" << endl;
        return {};
    }

    unsigned length = ExtractChunkLength(fileData, offset);
    if (offset + 5 <= fileData.size()) {
        int startOffset = offset + 5;
        int endIndex = startOffset + length - 1;

        if (endIndex < fileData.size()) {
            vector<char> chunkData(fileData.begin() + startOffset, fileData.begin() + endIndex + 1);
            vector<char> decompressedData;

            if (DecompressData(chunkData, decompressedData)) {
                return decompressedData;
            } else {
                cerr << "错误: 解压失败。" << endl;
                return {};
            }
        } else {
            cerr << "错误: 区块数据超出了文件边界。" << endl;
            return {};
        }
    } else {
        cerr << "错误: 从偏移位置读取5个字节的数据不够。" << endl;
        return {};
    }
}

std::vector<char> getRegionFromCache(int regionX, int regionZ) {
    // 创建区域缓存的键值
    auto regionKey = std::make_pair(regionX, regionZ);

    // 检查区域是否已缓存
    if (regionCache.find(regionKey) == regionCache.end()) {
        // 若未缓存，从磁盘读取区域文件
        std::vector<char> fileData = ReadFileToMemory(config.worldPath, regionX, regionZ);
        // 将区域文件数据存入缓存
        regionCache[regionKey] = fileData;
    }

    // 返回缓存中的区域文件数据
    return regionCache[regionKey];
}

std::shared_ptr<NbtTag> getChunkFromCache(int chunkX, int chunkZ, std::vector<char>& regionData) {
    // 创建区块缓存的键值
    auto chunkKey = std::make_pair(mod32(chunkX), mod32(chunkZ));

    // 检查区块是否已缓存
    if (chunkCache.find(chunkKey) == chunkCache.end()) {
        // 若未缓存，获取区块 NBT 数据
        std::vector<char> chunkData = GetChunkNBTData(regionData, chunkKey.first, chunkKey.second);
        size_t index = 0;
        // 解析 NBT 标签
        auto tag = readTag(chunkData, index);
        // 将解析后的标签存入缓存
        chunkCache[chunkKey] = tag;
    }

    // 返回缓存中的 NBT 标签
    return chunkCache[chunkKey];
}

std::string PrintHeightMapCache(bool verbose, bool showAllChunks) {
    std::ostringstream ss;

    if (heightMapCache.empty()) {
        return "HeightMapCache is empty\n";
    }

    ss << "====== HeightMap Cache Report ======\n";
    ss << "Total cached chunks: " << heightMapCache.size() << "\n\n";

    // C++14兼容遍历方式
    for (const auto& chunkEntry : heightMapCache) {
        const auto& chunkCoord = chunkEntry.first;
        const auto& typeMap = chunkEntry.second;
        int chunkX = chunkCoord.first;
        int chunkZ = chunkCoord.second;

        if (!showAllChunks && typeMap.empty()) continue;

        ss << "[Chunk (" << chunkX << "," << chunkZ << ")]\n";

        // 类型遍历
        for (const auto& typeEntry : typeMap) {
            const std::string& mapType = typeEntry.first;
            const auto& heightArray = typeEntry.second;

            ss << "  Type: " << mapType << "\n";

            if (verbose) {
                for (int z = 0; z < 16; ++z) {
                    ss << "    ";
                    for (int x = 0; x < 16; ++x) {
                        int index = x + z * 16;
                        ss << std::setw(4) << heightArray[index] << " ";
                    }
                    ss << "\n";
                }
            }
        }
        ss << "\n";
    }

    return ss.str();
}

// --------------------------------------------------------------------------------
// 方块相关核心函数
// --------------------------------------------------------------------------------
void RegisterBlockPalette(const std::vector<std::string>& blockPalette) {
    for (const auto& blockName : blockPalette) {
        if (find_if(globalBlockPalette.begin(), globalBlockPalette.end(), [&blockName](const Block& b) {
            return b.name == blockName;
        }) == globalBlockPalette.end()) {
            globalBlockPalette.emplace_back(Block(blockName));
        }
    }
}

void loadSolidBlocks(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open air blocks file: " + filepath);
    }

    nlohmann::json j;
    file >> j;

    if (j.contains("solid_blocks")) {
        for (auto& block : j["solid_blocks"]) {
            solidBlocks.insert(block.get<std::string>());
        }
    }
    else {
        throw std::runtime_error("Air blocks file missing 'solid_blocks' array");
    }
}

void LoadAndCacheBlockData(int chunkX, int chunkZ, int sectionY, const std::tuple<int, int, int>& blockKey) {
    // 内部计算区域坐标
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);

    std::vector<char> regionData = getRegionFromCache(regionX, regionZ);
    std::shared_ptr<NbtTag> tag = getChunkFromCache(chunkX, chunkZ, regionData);
    auto sec = getSectionByIndex(tag, sectionY);
    auto blo = getBlockStates(sec);


    // 提取调色板和方块数据
    std::vector<std::string> blockPalette = getBlockPalette(blo);
    std::vector<int> blockData = getBlockStatesData(blo, blockPalette);

    // 转换为全局ID并注册调色板
    std::vector<int> globalBlockData;
    if (!blockPalette.empty()) {
        for (int relativeId : blockData) {
            if (relativeId < 0 || relativeId >= blockPalette.size()) {
                globalBlockData.push_back(0);
                continue;
            }
            const std::string& blockName = blockPalette[relativeId];
            auto globalIter = std::find_if(globalBlockPalette.begin(), globalBlockPalette.end(),
                [&blockName](const Block& b) { return b.name == blockName; });
            if (globalIter != globalBlockPalette.end()) {
                globalBlockData.push_back(static_cast<int>(globalIter - globalBlockPalette.begin()));
            }
            else {
                globalBlockPalette.emplace_back(Block(blockName));
                globalBlockData.push_back(globalBlockPalette.size() - 1);
            }
        }

    }
    // ================ 新增高度图处理 ================
    auto heightMapsTag = getChildByName(tag, "Heightmaps");
    if (heightMapsTag && heightMapsTag->type == TagType::COMPOUND) {
        std::vector<std::string> mapTypes = {
            "MOTION_BLOCKING",
            "MOTION_BLOCKING_NO_LEAVES",
            "OCEAN_FLOOR",
            "WORLD_SURFACE"
        };


        for (const auto& mapType : mapTypes) {
            auto mapDataTag = getChildByName(heightMapsTag, mapType);
            if (mapDataTag && mapDataTag->type == TagType::LONG_ARRAY) {
                size_t numLongs = mapDataTag->payload.size() / sizeof(int64_t);
                const int64_t* rawData = reinterpret_cast<const int64_t*>(mapDataTag->payload.data());
                std::vector<int64_t> longData(rawData, rawData + numLongs);

                // 直接传入long数组即可
                std::vector<int> heights = decodeHeightMap(longData);
                heightMapCache[std::make_pair(chunkX, chunkZ)][mapType] = heights;
            }
        }
    }
    // ================ 新增群系数据处理 ================
    // 获取群系数据
    auto bio = getBiomes(sec);
    std::vector<int> biomeData;

    if (bio) {
        // 获取群系调色板
        std::vector<std::string> biomePalette = getBiomePalette(bio);

        // 获取 data 标签
        auto dataTag = getChildByName(bio, "data");
        if (dataTag && dataTag->type == TagType::LONG_ARRAY) {
            // 解析 long 数组
            size_t numLongs = dataTag->payload.size() / sizeof(int64_t);
            const int64_t* data = reinterpret_cast<const int64_t*>(dataTag->payload.data());

            // 计算编码参数
            int bitsPerIndex = biomePalette.size() > 1 ?
                static_cast<int>(std::ceil(std::log2(biomePalette.size()))) : 1;
            int entriesPerLong = 64 / bitsPerIndex;
            int mask = (1 << bitsPerIndex) - 1;

            // 解析每个 long 值
            for (size_t i = 0; i < numLongs; ++i) {
                int64_t value = reverseEndian(data[i]);
                for (int j = 0; j < entriesPerLong; ++j) {
                    int index = (value >> (j * bitsPerIndex)) & mask;
                    if (index < biomePalette.size()) {
                        int bid = Biome::GetId(biomePalette[index]);
                        biomeData.push_back(bid == -1 ? 0 : bid); // 无效 ID 使用默认
                    }
                    else {
                        biomeData.push_back(0); // 无效索引使用默认
                    }
                }
            }

            // 确保数据长度为 4096（16x16x16）
            if (biomeData.size() > 4096) biomeData.resize(4096);
            else if (biomeData.size() < 4096) biomeData.resize(4096, 0);
        }
    }

    // 缓存群系数据
    if (!biomeData.empty()) {
        biomeDataCache[blockKey] = biomeData;
    }
    // 更新缓存
    blockDataCache[blockKey] = globalBlockData;
}

// 新增方法：无需传入 blockKey，由 chunkX、chunkZ 和 sectionY 计算得出
void LoadCacheBlockDataAutomatically(int chunkX, int chunkZ, int sectionY) {
    // 计算区域坐标
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);

    // 获取区域数据
    std::vector<char> regionData = getRegionFromCache(regionX, regionZ);

    // 获取区块数据
    std::shared_ptr<NbtTag> tag = getChunkFromCache(chunkX, chunkZ, regionData);

    // 获取子区块
    auto sec = getSectionByIndex(tag, sectionY);
    auto blo = getBlockStates(sec);

    // 提取调色板和方块数据
    std::vector<std::string> blockPalette = getBlockPalette(blo);
    std::vector<int> blockData = getBlockStatesData(blo, blockPalette);

    // 转换为全局ID并注册调色板
    std::vector<int> globalBlockData;
    if (!blockPalette.empty()) {
        for (int relativeId : blockData) {
            if (relativeId < 0 || relativeId >= blockPalette.size()) {
                globalBlockData.push_back(0);
                continue;
            }
            const std::string& blockName = blockPalette[relativeId];
            auto globalIter = std::find_if(globalBlockPalette.begin(), globalBlockPalette.end(),
                [&blockName](const Block& b) { return b.name == blockName; });
            if (globalIter != globalBlockPalette.end()) {
                globalBlockData.push_back(static_cast<int>(globalIter - globalBlockPalette.begin()));
            }
            else {
                globalBlockPalette.emplace_back(Block(blockName));
                globalBlockData.push_back(globalBlockPalette.size() - 1);
            }
        }
    }

    // 计算 blockKey
    int relativeChunkX = mod32(chunkX);
    int relativeChunkZ = mod32(chunkZ);
    int adjustedSectionY = AdjustSectionY(sectionY);
    auto blockKey = std::make_tuple(relativeChunkX, relativeChunkZ, adjustedSectionY);
    // ================ 新增高度图处理 ================
    auto heightMapsTag = getChildByName(tag, "Heightmaps");
    if (heightMapsTag && heightMapsTag->type == TagType::COMPOUND) {
        std::vector<std::string> mapTypes = {
            "MOTION_BLOCKING",
            "MOTION_BLOCKING_NO_LEAVES",
            "OCEAN_FLOOR",
            "WORLD_SURFACE"
        };


        for (const auto& mapType : mapTypes) {
            auto mapDataTag = getChildByName(heightMapsTag, mapType);
            if (mapDataTag && mapDataTag->type == TagType::LONG_ARRAY) {
                size_t numLongs = mapDataTag->payload.size() / sizeof(int64_t);
                const int64_t* rawData = reinterpret_cast<const int64_t*>(mapDataTag->payload.data());
                std::vector<int64_t> longData(rawData, rawData + numLongs);

                // 直接传入long数组即可
                std::vector<int> heights = decodeHeightMap(longData);
                heightMapCache[std::make_pair(chunkX, chunkZ)][mapType] = heights;
            }
        }
    }

    // ================ 新增群系数据处理 ================
    // 获取群系数据
    auto bio = getBiomes(sec);
    std::vector<int> biomeData;

    if (bio) {
        // 获取群系调色板
        std::vector<std::string> biomePalette = getBiomePalette(bio);

        // 获取 data 标签
        auto dataTag = getChildByName(bio, "data");
        if (dataTag && dataTag->type == TagType::LONG_ARRAY) {
            // 解析 long 数组
            size_t numLongs = dataTag->payload.size() / sizeof(int64_t);
            const int64_t* data = reinterpret_cast<const int64_t*>(dataTag->payload.data());

            // 计算编码参数
            int bitsPerIndex = biomePalette.size() > 1 ?
                static_cast<int>(std::ceil(std::log2(biomePalette.size()))) : 1;
            int entriesPerLong = 64 / bitsPerIndex;
            int mask = (1 << bitsPerIndex) - 1;

            // 解析每个 long 值
            for (size_t i = 0; i < numLongs; ++i) {
                int64_t value = reverseEndian(data[i]);
                for (int j = 0; j < entriesPerLong; ++j) {
                    int index = (value >> (j * bitsPerIndex)) & mask;
                    if (index < biomePalette.size()) {
                        int bid = Biome::GetId(biomePalette[index]);
                        biomeData.push_back(bid == -1 ? 0 : bid); // 无效 ID 使用默认
                    }
                    else {
                        biomeData.push_back(0); // 无效索引使用默认
                    }
                }
            }

            // 确保数据长度为 4096（16x16x16）
            if (biomeData.size() > 4096) biomeData.resize(4096);
            else if (biomeData.size() < 4096) biomeData.resize(4096, 0);
        }
    }

    // 缓存群系数据
    if (!biomeData.empty()) {
        biomeDataCache[blockKey] = biomeData;
    }
    // 更新缓存
    blockDataCache[blockKey] = globalBlockData;
}
// --------------------------------------------------------------------------------
// 方块ID查询相关函数
// --------------------------------------------------------------------------------
int GetBlockId(int blockX, int blockY, int blockZ) {
    // 将世界坐标(blockX, blockZ)转换为所在的区块坐标(chunkX, chunkZ)
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    // 将区块坐标(chunkX, chunkZ)转换为所在的区域坐标(regionX, regionZ)
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);

    // 计算当前区块在其区域中的相对位置（同理 Z 轴）
    int relativeChunkX = mod32(chunkX);
    int relativeChunkZ = mod32(chunkZ);

    // 将方块的 Y 坐标转换为子区块（Section）索引
    int SectionY;
    blockYToSectionY(blockY, SectionY);

    // 调整子区块 Y 坐标到外部使用的偏移量（通常 Minecraft 的世界 Y 轴从 -64 开始）
    int adjustedSectionY = AdjustSectionY(SectionY);

    // 计算方块在当前子区块中的相对坐标（XYZ）
    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);

    // 创建键用于查找缓存
    auto blockKey = std::make_tuple(relativeChunkX, relativeChunkZ, adjustedSectionY);

    // 检查缓存是否存在
    auto blockCacheIter = blockDataCache.find(blockKey);
    if (blockCacheIter != blockDataCache.end()) {
        std::vector<int>& blockData = blockCacheIter->second;
        int encodedValue = toYZX(relativeX, relativeY, relativeZ);
        return (encodedValue < blockData.size()) ? blockData[encodedValue] : 0;
    }

    // 若缓存不存在，调用新方法加载并缓存数据
    LoadAndCacheBlockData(chunkX, chunkZ, SectionY, blockKey);

    // 返回当前方块的全局ID
    std::vector<int>& cachedData = blockDataCache[blockKey];
    int encodedValue = toYZX(relativeX, relativeY, relativeZ);
    return (encodedValue < cachedData.size()) ? cachedData[encodedValue] : 0;
}
// --------------------------------------------------------------------------------
// 方块扩展信息查询函数
// --------------------------------------------------------------------------------
Block GetBlockById(int blockId) {
    if (blockId >= 0 && blockId < globalBlockPalette.size()) {
        return globalBlockPalette[blockId];
    } else {
        return Block("minecraft:air", true);
    }
}

std::string GetBlockNameById(int blockId) {
    if (blockId >= 0 && blockId < globalBlockPalette.size()) {
        return globalBlockPalette[blockId].GetModifiedNameWithNamespace();
    } else {
        return "minecraft:air";
    }
}

std::string GetBlockNamespaceById(int blockId) {
    if (blockId >= 0 && blockId < globalBlockPalette.size()) {
        return globalBlockPalette[blockId].GetNamespace();
    }
    else {
        return "minecraft";
    }
}

// 获取方块ID时同时获取相邻方块的air状态，返回当前方块ID
int GetBlockIdWithNeighbors(int blockX, int blockY, int blockZ, bool* neighborIsAir) {
    int currentId = GetBlockId(blockX, blockY, blockZ);
    Block currentBlock = GetBlockById(currentId);

    const std::vector<std::tuple<int, int, int>> directions = {
        {0, 1, 0},  // 上（Y+）
        {0, -1, 0}, // 下（Y-）
        {-1, 0, 0}, // 西（X-）
        {1, 0, 0},  // 东（X+）
        {0, 0, -1}, // 北（Z-）
        {0, 0, 1}   // 南（Z+）
    };

    for (int i = 0; i < 6; ++i) {
        int dx, dy, dz;
        tie(dx, dy, dz) = directions[i];

        int nx = blockX + dx;
        int ny = blockY + dy;
        int nz = blockZ + dz;

        //if (ny < -64 || ny > 255) {
        //    neighborIsAir[i] = (ny < -64); // 地下视为非空气（可选）
        //    continue;
        //}

        int neighborId = GetBlockId(nx, ny, nz);
        Block neighborBlock = GetBlockById(neighborId);
        neighborIsAir[i] = neighborBlock.air;
    }

    return currentId;
}

int GetHeightMapY(int blockX, int blockZ, const std::string& heightMapType) {
    // 将世界坐标转换为区块坐标
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    // 触发区块加载（确保高度图数据存在）
    GetBlockId(blockX, 0, blockZ); // Y坐标任意，只为触发加载

    // 查找缓存
    auto chunkKey = std::make_pair(chunkX, chunkZ);
    auto chunkIter = heightMapCache.find(chunkKey);
    if (chunkIter == heightMapCache.end()) {
        return -1; // 区块未加载
    }

    // 获取指定类型的高度图
    auto& typeMap = chunkIter->second;
    auto typeIter = typeMap.find(heightMapType);
    if (typeIter == typeMap.end()) {
        return -2; // 类型不存在
    }

    // 计算局部坐标
    int localX = mod16(blockX);
    int localZ = mod16(blockZ);
    int index = localX + localZ * 16;

    // 返回高度值
    return (index < 256) ? typeIter->second[index] : -1;
}
// --------------------------------------------------------------------------------
// 全局方块配置相关函数
// --------------------------------------------------------------------------------
void InitializeGlobalBlockPalette() {
    globalBlockPalette.emplace_back(Block("minecraft:air", true));
}

std::vector<Block> GetGlobalBlockPalette() {
    return globalBlockPalette;
}