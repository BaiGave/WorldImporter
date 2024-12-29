#include <iostream>
#include <unordered_map>
#include <string>
#include <memory>
#include "block.h"
#include "nbtutils.h" 
#include "fileutils.h"
#include "decompressor.h"
#include "coord_conversion.h"
#include "config.h"
#include <chrono>
#include <fstream> 
#include <locale> 
#include <codecvt>



using namespace std;

// 自定义哈希函数，用于std::pair<int, int>
struct pair_hash {
    template <class T1, class T2>
    std::size_t operator ()(const std::pair<T1, T2>& p) const {
        auto h1 = std::hash<T1>{}(p.first);
        auto h2 = std::hash<T2>{}(p.second);
        // 将两个哈希值合并
        return h1 ^ (h2 << 1);  // 左移并进行异或
    }
};

// 自定义哈希函数，用于 std::pair<int, int, int>，支持Y值
struct triple_hash {
    template <class T1, class T2, class T3>
    std::size_t operator ()(const std::tuple<T1, T2, T3>& t) const {
        auto h1 = std::hash<T1>{}(std::get<0>(t));
        auto h2 = std::hash<T2>{}(std::get<1>(t));
        auto h3 = std::hash<T3>{}(std::get<2>(t));
        return h1 ^ (h2 << 1) ^ (h3 << 2);  // 结合3个哈希值
    }
};

// 获取区块 NBT 数据
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z) {
    unsigned offset = CalculateOffset(fileData, x, z);
    if (offset == 0) {
        cerr << "错误: 偏移计算失败。" << endl;
        return {};  // 返回空的 vector，表示出错
    }

    // 提取区块数据长度
    unsigned length = ExtractChunkLength(fileData, offset);
    // 读取并解压区块数据
    if (offset + 5 <= fileData.size()) {
        int startOffset = offset + 5;
        int endIndex = startOffset + length - 1;

        if (endIndex < fileData.size()) {
            vector<char> chunkData(fileData.begin() + startOffset, fileData.begin() + endIndex + 1);
            vector<char> decompressedData;

            if (DecompressData(chunkData, decompressedData)) {
                return decompressedData;  // 返回解压后的数据
            }
            else {
                cerr << "错误: 解压失败。" << endl;
                return {};  // 解压失败，返回空数据
            }
        }
        else {
            cerr << "错误: 区块数据超出了文件边界。" << endl;
            return {};  // 返回空的 vector，表示出错
        }
    }
    else {
        cerr << "错误: 从偏移位置读取5个字节的数据不够。" << endl;
        return {};  // 返回空的 vector，表示出错
    }
}

// 缓存读取的数据
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache;
std::unordered_map<std::pair<int, int>, std::shared_ptr<NbtTag>, pair_hash> chunkCache;
std::unordered_map<std::tuple<int, int, int>, std::vector<std::string>, triple_hash> blockPaletteCache;
std::unordered_map<std::tuple<int, int, int>, std::vector<int>, triple_hash> blockDataCache; // 缓存 blockData

// 全局的 blockPalette 和 blockNameToId 映射
std::unordered_map<std::string, int> globalBlockPalette;
std::unordered_map<int, std::string> idToBlockName;

// 初始化，注册 "minecraft:air" 为 ID 0
void InitializeGlobalBlockPalette() {
    globalBlockPalette["minecraft:air"] = 0;
    idToBlockName[0] = "minecraft:air";
}

// 将子区块的 blockPalette 加入全局表并注册
void RegisterBlockPalette(const std::vector<std::string>& blockPalette) {
    for (const auto& blockName : blockPalette) {
        if (globalBlockPalette.find(blockName) == globalBlockPalette.end()) {
            // 如果该方块没有注册过，注册它
            int newId = globalBlockPalette.size();  // ID 从当前大小开始
            globalBlockPalette[blockName] = newId;
            idToBlockName[newId] = blockName;
        }
    }
}

// 处理 SectionY 为负数的情况，确保它是一个非负的索引
int AdjustSectionY(int SectionY) {
    const int OFFSET = 64;  // 偏移量，确保 SectionY 总是非负
    return SectionY + OFFSET;  // 通过加偏移量来确保 SectionY 非负
}

int GetBlockId(int blockX, int blockY, int blockZ) {
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);

    int relativeChunkX = mod32(chunkX);
    int relativeChunkZ = mod32(chunkZ);

    int SectionY;
    blockYToSectionY(blockY, SectionY);

    // 调整 SectionY 为非负值
    int adjustedSectionY = AdjustSectionY(SectionY);

    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);

    // 使用调整后的 SectionY 作为缓存的键
    auto blockKey = std::make_tuple(relativeChunkX, relativeChunkZ, adjustedSectionY);

    // 查询缓存
    auto blockCacheIter = blockDataCache.find(blockKey);
    if (blockCacheIter != blockDataCache.end() && blockPaletteCache.find(blockKey) != blockPaletteCache.end()) {
        // 如果缓存存在，直接使用缓存的数据
        const auto& blockData = blockCacheIter->second;
        const auto& blockPalette = blockPaletteCache[blockKey];

        if (blockData.empty()) {
            return globalBlockPalette["minecraft:air"];  // 如果没有数据，返回 air 的全局 ID
        }

        int encodedValue = toYZX(relativeX, relativeY, relativeZ);
        int relativeId = blockData[encodedValue];  // 子区块的相对 ID

        // 查找全局 ID
        if (relativeId >= 0 && relativeId < blockPalette.size()) {
            const std::string& blockName = blockPalette[relativeId];
            return globalBlockPalette[blockName];  // 返回全局的 ID
        }
        else {
            return globalBlockPalette["minecraft:air"];  // 如果无法找到，返回 air 的全局 ID
        }
    }

    // 如果缓存没有数据，读取文件并解析
    std::vector<char> fileData;
    auto regionKey = std::make_pair(regionX, regionZ);

    // 读取 region 数据
    if (regionCache.find(regionKey) == regionCache.end()) {
        fileData = ReadFileToMemory(config.worldPath, regionX, regionZ);
        regionCache[regionKey] = fileData;
    }
    else {
        fileData = regionCache[regionKey];
    }

    std::shared_ptr<NbtTag> tag;
    auto chunkKey = std::make_pair(relativeChunkX, relativeChunkZ);
    if (chunkCache.find(chunkKey) == chunkCache.end()) {
        std::vector<char> chunkData = GetChunkNBTData(fileData, relativeChunkX, relativeChunkZ);
        size_t index = 0;
        tag = readTag(chunkData, index);
        chunkCache[chunkKey] = tag;
    }
    else {
        tag = chunkCache[chunkKey];
    }

    auto sec = getSectionByIndex(tag, SectionY);
    auto blo = getBlockStates(sec);

    std::vector<std::string> blockPalette = getBlockPalette(blo);
    std::vector<int> blockData = getBlockStatesData(blo, blockPalette);

    // 注册新的 blockPalette 到全局表
    RegisterBlockPalette(blockPalette);

    // 缓存解析后的 blockPalette 和 blockData
    blockPaletteCache[blockKey] = blockPalette;
    blockDataCache[blockKey] = blockData;

    if (blockData.empty()) {
        return globalBlockPalette["minecraft:air"];  // 如果没有数据，返回 air 的全局 ID
    }

    int encodedValue = toYZX(relativeX, relativeY, relativeZ);
    int relativeId = blockData[encodedValue];  // 子区块的相对 ID

    // 查找全局 ID
    if (relativeId >= 0 && relativeId < blockPalette.size()) {
        const std::string& blockName = blockPalette[relativeId];
        return globalBlockPalette[blockName];  // 返回全局的 ID
    }
    else {
        return globalBlockPalette["minecraft:air"];  // 如果无法找到，返回 air 的全局 ID
    }
}

// 通过 ID 获取方块名称
std::string GetBlockNameById(int blockId) {
    auto it = idToBlockName.find(blockId);
    if (it != idToBlockName.end()) {
        return it->second;  // 如果找到了对应的 ID，返回名称
    }
    else {
        return "minecraft:air";  // 如果没有找到对应的名称，返回默认的 air
    }
}

// 返回全局的 block 对照表 (block 名称 <-> ID)
std::unordered_map<int, std::string> GetGlobalBlockPalette() {
    return idToBlockName;
}
