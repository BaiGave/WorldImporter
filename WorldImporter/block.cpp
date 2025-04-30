// --- C++ 标准库头文件 ---
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <locale>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// --- 第三方库头文件 ---
#include "include/json.hpp"

// --- 项目头文件 ---
#include "config.h"
#include "block.h"
#include "model.h"
#include "EntityBlock.h"
#include "blockstate.h"
#include "nbtutils.h"
#include "biome.h"
#include "fileutils.h"
#include "decompressor.h"
#include "locutil.h"
#include "hashutils.h"

using namespace std;

// --------------------------------------------------------------------------------
// 文件缓存相关对象
// --------------------------------------------------------------------------------
// 统一的缓存表（预留桶数量，减少 rehash）
std::unordered_map<std::tuple<int, int, int>, SectionCacheEntry, triple_hash> sectionCache(4096);
// 添加静态邻居偏移数组，避免重复构造
static const std::array<std::tuple<int,int,int>,6> kSectionNeighborOffsets = {{
    {1, 0, 0}, {-1, 0, 0},
    {0, 1, 0}, {0, -1, 0},
    {0, 0, 1}, {0, 0, -1}
}};
// 实体方块缓存
std::unordered_map<std::pair<int, int>, std::vector<std::shared_ptr<EntityBlock>>, pair_hash> entityBlockCache(1024);
// 区域和高度图缓存（预留桶减少 rehash）
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache(1024);
std::unordered_map<std::pair<int, int>, std::unordered_map<std::string, std::vector<int>>, pair_hash> heightMapCache(1024);
std::vector<Block> globalBlockPalette;
std::unordered_set<std::string> solidBlocks;
std::unordered_set<std::string> fluidBlocks;
std::unordered_map<std::string, FluidInfo> fluidDefinitions;
// --------------------------------------------------------------------------------
// 文件操作相关函数
// --------------------------------------------------------------------------------
std::vector<int> decodeHeightMap(const std::vector<int64_t>& data) {
    // 预分配256个高度值，避免多次重分配
    std::vector<int> heights;
    heights.reserve(256);
    // 根据数据长度动态判断存储格式
    int bitsPerEntry = (data.size() == 37) ? 9 : 8;
    int entriesPerLong = 64 / bitsPerEntry;
    int mask = (1 << bitsPerEntry) - 1;
    for (const auto& longVal : data) {
        int64_t value = reverseEndian(longVal);
        for (int i = 0; i < entriesPerLong; ++i) {
            heights.push_back(static_cast<int>((value >> (i * bitsPerEntry)) & mask));
            if (heights.size() >= 256) break;
        }
        if (heights.size() >= 256) break;
    }
    heights.resize(256);
    return heights;
}

std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z) {
    unsigned offset = CalculateOffset(fileData, mod32(x), mod32(z));

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

const std::vector<char>& getRegionFromCache(int regionX, int regionZ) {
    // 创建区域缓存的键值
    auto regionKey = std::make_pair(regionX, regionZ);
    // 使用查找避免重复哈希
    auto it = regionCache.find(regionKey);
    if (it == regionCache.end()) {
        // 若未缓存，从磁盘读取区域文件并插入缓存
        std::vector<char> fileData = ReadFileToMemory(config.worldPath, regionX, regionZ);
        auto result = regionCache.emplace(regionKey, std::move(fileData));
        it = result.first;
    }
    // 返回缓存中的区域文件数据
    return it->second;
}

void UpdateSkyLightNeighborFlags() {
    std::unordered_map<std::tuple<int, int, int>, bool, triple_hash> needsUpdate;

    // 收集需要更新的区块
    for (const auto& entry : sectionCache) {
        const auto& key = entry.first;
        const auto& skyLightData = entry.second.skyLight;

        if (skyLightData.size() == 1 && skyLightData[0] == -1) {
            needsUpdate[key] = true;
        }
    }

    // 检查邻居，并更新skyLightData为单元素-2，使用assign避免重新分配
    for (auto& entry : needsUpdate) {
        int chunkX = std::get<0>(entry.first);
        int chunkZ = std::get<1>(entry.first);
        int sectionY = std::get<2>(entry.first);
        bool hasLightNeighbor = false;
        for (const auto& offset : kSectionNeighborOffsets) {
            auto dir = std::make_tuple(chunkX + std::get<0>(offset), chunkZ + std::get<1>(offset), sectionY + std::get<2>(offset));
            auto it = sectionCache.find(dir);
            if (it != sectionCache.end() && it->second.skyLight.size() == 4096) {
                hasLightNeighbor = true;
                break;
            }
        }
        if (hasLightNeighbor) {
            auto& skyLightData = sectionCache[entry.first].skyLight;
            skyLightData.assign(1, -2);
        }
    }
}



// --------------------------------------------------------------------------------
// 方块相关核心函数
// --------------------------------------------------------------------------------
// 新增函数：处理单个子区块
void ProcessSection(int chunkX, int chunkZ, int sectionY, const NbtTagPtr& sectionTag) {
    // 获取方块数据
    auto blo = getBlockStates(sectionTag);
    std::vector<std::string> blockPalette = getBlockPalette(blo);
    std::vector<int> blockData = getBlockStatesData(blo, blockPalette);

    // 转换为全局ID并注册调色板
    std::vector<int> globalBlockData;
    globalBlockData.reserve(blockData.size()); // 预分配空间

    static std::unordered_map<std::string, int> globalBlockMap; // 预处理全局调色板映射

    // 预处理全局调色板，建立快速查找的映射
    if (globalBlockMap.empty()) {
        for (size_t i = 0; i < globalBlockPalette.size(); ++i) {
            const Block& block = globalBlockPalette[i];
            if (globalBlockMap.find(block.name) == globalBlockMap.end()) {
                globalBlockMap[block.name] = static_cast<int>(i);
            }
        }
    }

    for (int relativeId : blockData) {
        if (relativeId < 0 || relativeId >= static_cast<int>(blockPalette.size())) {
            globalBlockData.push_back(0);
            continue;
        }

        const std::string& blockName = blockPalette[relativeId];
        auto it = globalBlockMap.find(blockName);
        if (it != globalBlockMap.end()) {
            globalBlockData.push_back(it->second);
        }
        else {
            int idx = static_cast<int>(globalBlockPalette.size());
            globalBlockPalette.emplace_back(blockName);
            globalBlockMap[blockName] = idx;
            globalBlockData.push_back(idx);
        }
    }

    // 获取生物群系数据
    auto bio = getBiomes(sectionTag);
    std::vector<int> biomeData;
    if (bio) {
        std::vector<std::string> biomePalette = getBiomePalette(bio);
        auto dataTag = getChildByName(bio, "data");

        if (dataTag && dataTag->type == TagType::LONG_ARRAY) {
            int paletteSize = biomePalette.size();
            int bitsPerEntry = (paletteSize > 1) ? static_cast<int>(std::ceil(std::log2(paletteSize))) : 1;
            int entriesPerLong = 64 / bitsPerEntry; // 每个long存储多少个条目
            int mask = (1 << bitsPerEntry) - 1;

            biomeData.resize(64, 0); // 固定64个生物群系单元
            int totalProcessed = 0;

            const int64_t* data = reinterpret_cast<const int64_t*>(dataTag->payload.data());
            size_t dataSize = dataTag->payload.size() / sizeof(int64_t);

            for (size_t i = 0; i < dataSize && totalProcessed < 64; ++i) {
                int64_t value = reverseEndian(data[i]);
                for (int pos = 0; pos < entriesPerLong && totalProcessed < 64; ++pos) {
                    int index = (value >> (pos * bitsPerEntry)) & mask;
                    if (index < paletteSize) {
                        biomeData[totalProcessed] = Biome::GetId(biomePalette[index]);
                    }
                    totalProcessed++;
                }
            }
        }
        else if (!biomePalette.empty()) {
            int defaultBid = Biome::GetId(biomePalette[0]);
            biomeData.assign(64, defaultBid);
        }
    }

    // 获取光照数据
    auto processLightData = [&](const std::string& lightType, std::vector<int>& lightData) {
        auto lightTag = getChildByName(sectionTag, lightType);
        if (lightTag && lightTag->type == TagType::BYTE_ARRAY) {
            // 批量解析，每个原始字节产生2个光照值
            const auto& rawData = lightTag->payload;
            size_t rawSize = rawData.size();
            lightData.resize(4096);
            size_t pairs = std::fmin(rawSize, size_t(2048));
            for (size_t i = 0; i < pairs; ++i) {
                uint8_t byteVal = static_cast<uint8_t>(rawData[i]);
                lightData[2*i]   = byteVal & 0xF;
                lightData[2*i+1] = (byteVal >> 4) & 0xF;
            }
            if (pairs * 2 < 4096) {
                std::memset(lightData.data() + pairs * 2, 0, (4096 - pairs * 2) * sizeof(int));
            }
        }
        else {
            lightData = { -1 };
        }
    };

    std::vector<int> skyLightData;
    processLightData("SkyLight", skyLightData);
    std::vector<int> blockLightData;
    processLightData("BlockLight", blockLightData);

    // 存储到统一的缓存
    int adjustedSectionY = AdjustSectionY(sectionY);
    auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);
    sectionCache[blockKey] = {
        std::move(skyLightData),      // skyLight
        std::move(blockLightData),    // blockLight
        std::move(globalBlockData),   // blockData
        std::move(biomeData),         // biomeData
        std::move(blockPalette)       // blockPalette
    };
}

void ProcessEntityBlocks(int chunkX, int chunkZ, const NbtTagPtr& blockEntitiesTag) {
    std::vector<std::shared_ptr<EntityBlock>> entityBlocks;

    for (const auto& entityTag : blockEntitiesTag->children) {
        // 提取基础信息
        auto idTag = getChildByName(entityTag, "id");
        auto xTag = getChildByName(entityTag, "x");
        auto yTag = getChildByName(entityTag, "y");
        auto zTag = getChildByName(entityTag, "z");

        std::string id;
        int x = 0, y = 0, z = 0;
        if (idTag && idTag->type == TagType::STRING) {
            id = std::string(idTag->payload.begin(), idTag->payload.end());
        }
        if (xTag && xTag->type == TagType::INT) {
            x = bytesToInt(xTag->payload);
        }
        if (yTag && yTag->type == TagType::INT) {
            y = bytesToInt(yTag->payload);
        }
        if (zTag && zTag->type == TagType::INT) {
            z = bytesToInt(zTag->payload);
        }

        // 创建实体
        std::shared_ptr<EntityBlock> entityBlock;

        // 在解析每个 blockTag 时创建 YuushyaBlockEntry 并填充数据
        if (id == "yuushya:showblockentity") {
            auto yuushyaEntity = std::make_shared<YuushyaShowBlockEntity>();
            yuushyaEntity->id = id;
            yuushyaEntity->x = x;
            yuushyaEntity->y = y;
            yuushyaEntity->z = z;

            auto blocksTag = getChildByName(entityTag, "Blocks");
            if (blocksTag && blocksTag->type == TagType::LIST) {
                for (const auto& blockTag : blocksTag->children) {
                    if (blockTag && blockTag->type == TagType::COMPOUND) {
                        YuushyaBlockEntry entry;

                        // 解析 BlockState
                        auto blockStateTag = getChildByName(blockTag, "BlockState");
                        if (blockStateTag && blockStateTag->type == TagType::COMPOUND) {
                            std::string blockName;
                            auto nameTag = getChildByName(blockStateTag, "Name");
                            if (nameTag && nameTag->type == TagType::STRING) {
                                blockName = std::string(nameTag->payload.begin(), nameTag->payload.end());
                            }

                            // 解析 Properties
                            auto propertiesTag = getChildByName(blockStateTag, "Properties");
                            if (propertiesTag && propertiesTag->type == TagType::COMPOUND) {
                                std::string propertiesStr;
                                for (const auto& prop : propertiesTag->children) {
                                    if (!propertiesStr.empty()) propertiesStr += ",";
                                    propertiesStr += prop->name + ":" + std::string(prop->payload.begin(), prop->payload.end());
                                }
                                if (!propertiesStr.empty()) {
                                    blockName += "[" + propertiesStr + "]";
                                }
                            }

                            // 转换为全局 ID
                            static std::unordered_map<std::string, int> globalBlockMap;
                            if (!blockName.empty()) {
                                auto it = globalBlockMap.find(blockName);
                                if (it != globalBlockMap.end()) {
                                    entry.blockid = it->second;
                                }
                                else {
                                    entry.blockid = static_cast<int>(globalBlockPalette.size());
                                    globalBlockPalette.emplace_back(blockName);
                                    globalBlockMap[blockName] = entry.blockid;
                                }
                            }
                        }

                        // 解析其他属性
                        auto showPosTag = getChildByName(blockTag, "ShowPos");
                        if (showPosTag && showPosTag->type == TagType::LIST) {
                            for (const auto& pos : showPosTag->children) {
                                entry.showPos.push_back(bytesToDouble(pos->payload));
                            }
                        }

                        auto showRotationTag = getChildByName(blockTag, "ShowRotation");
                        if (showRotationTag && showRotationTag->type == TagType::LIST) {
                            for (const auto& rot : showRotationTag->children) {
                                entry.showRotation.push_back(bytesToFloat(rot->payload));
                            }
                        }

                        auto showScalesTag = getChildByName(blockTag, "ShowScales");
                        if (showScalesTag && showScalesTag->type == TagType::LIST) {
                            for (const auto& scale : showScalesTag->children) {
                                entry.showScales.push_back(bytesToFloat(scale->payload));
                            }
                        }

                        auto isShownTag = getChildByName(blockTag, "isShown");
                        if (isShownTag && isShownTag->type == TagType::BYTE) {
                            entry.isShown = bytesToByte(isShownTag->payload);
                        }

                        auto slotTag = getChildByName(blockTag, "Slot");
                        if (slotTag && slotTag->type == TagType::BYTE) {
                            entry.slot = bytesToByte(slotTag->payload);
                        }

                        yuushyaEntity->blocks.push_back(entry);
                    }
                }
            }

            // 解析 ControlSlot 和 keepPacked
            auto controlSlotTag = getChildByName(entityTag, "ControlSlot");
            if (controlSlotTag) yuushyaEntity->controlSlot = bytesToByte(controlSlotTag->payload);

            auto keepPackedTag = getChildByName(entityTag, "keepPacked");
            if (keepPackedTag) yuushyaEntity->keepPacked = bytesToByte(keepPackedTag->payload);

            entityBlocks.push_back(yuushyaEntity);
        }
        else if (id == "littletiles:tiles") {
            auto littleTilesEntity = std::make_shared<LittleTilesTilesEntity>();
            littleTilesEntity->id = id;
            littleTilesEntity->x = x;
            littleTilesEntity->y = y;
            littleTilesEntity->z = z;

            // 解析 content 标签
            auto contentTag = getChildByName(entityTag, "content");
            if (contentTag && contentTag->type == TagType::COMPOUND) {
                // 获取 tiles 标签，tiles 是一个 CompoundTag
                auto tilesTag = getChildByName(contentTag, "tiles");
                if (tilesTag && tilesTag->type == TagType::COMPOUND) {
                    // 遍历 tiles 复合标签中的每个键，例如 "minecraft:granite"、"minecraft:stone"
                    for (const auto& tileGroupTag : tilesTag->children) {
                        // 确保子标签类型为 ListTag
                        if (tileGroupTag->type != TagType::LIST)
                            continue;

                        // 使用子标签的 name 作为默认的 blockName
                        std::string blockName = tileGroupTag->name;
                        // 新建一个 tile 条目
                        LittleTilesTileEntry tileEntry;
                        tileEntry.blockName = blockName;

                        // 标记：第一个 IntArrayTag 作为颜色，其余均作为 box
                        bool isFirstArray = true;
                        // 遍历 ListTag 下的每个子节点，均为 IntArrayTag
                        for (const auto& intArrayTag : tileGroupTag->children) {
                            if (intArrayTag->type != TagType::INT_ARRAY)
                                continue;

                            // 解析 payload 为 int 数组
                            std::vector<int> values = readIntArray(intArrayTag->payload);
                            if (isFirstArray) {
                                // 第一个数组作为颜色数据
                                tileEntry.color = values;
                                isFirstArray = false;
                            }
                            else {
                               
                                // 其他数组作为 box 数据
                                // 预期每个 box 应为 7 个 int
                                if (values.size() == 7) {
                                    // 辅助：把一个字节拆成 [高半字节, 低半字节]
                                    auto splitNibble = [](unsigned char b) {
                                        return std::vector<int>{ (b >> 4) & 0x0F, b & 0x0F };
                                        };

                                    // 在 box 处理逻辑里，拿到 intArrayTag->payload
                                    const auto& pl = intArrayTag->payload;

                                    unsigned char b0 = pl[3];
                                    unsigned char b1 = pl[2];
                                    unsigned char b2 = pl[1];

                                    // 拆半字节
                                    auto d0 = splitNibble(b0);   // 对应你要的 0x16 → [1,6]
                                    auto d1 = splitNibble(b1);   // 对应 0x13 → [1,3]
                                    auto d2 = splitNibble(b2);   // 对应 0x63 → [6,3]

                                    // 把它们拼进去
                                    std::vector<int> transformed;
                                    transformed.insert(transformed.end(), d0.begin(), d0.end());
                                    transformed.insert(transformed.end(), d1.begin(), d1.end());
                                    transformed.insert(transformed.end(), d2.begin(), d2.end());


                                    // 接着追加后面 6 个 int（原始顺序不变）
                                    for (size_t i = 1; i < values.size(); ++i) {
                                        transformed.push_back(values[i]);
                                    }

                                    // 现在 transformed 应该包含 12 个数字
                                    tileEntry.boxDataList.push_back(transformed);
                                }
                                else {
                                    // 如果数据不是 7 个 int，则直接保存原始数据，或根据需求做额外处理
                                    tileEntry.boxDataList.push_back(values);
                                }
                            }
                        }
                        // 将解析得到的 tileEntry 添加到实体中
                        littleTilesEntity->tiles.push_back(tileEntry);
                    }
                }
            }
            // 将解析完成后的 littletiles 实体加入实体列表
            entityBlocks.push_back(littleTilesEntity);
        
        }
        else {
            // 其他实体只存储基础信息
            auto basicEntity = std::make_shared<YuushyaShowBlockEntity>();
            basicEntity->id = id;
            basicEntity->x = x;
            basicEntity->y = y;
            basicEntity->z = z;
            // 存入缓存
            entityBlocks.push_back(basicEntity);
        }


    }

    // 存入缓存
    auto chunkKey = std::make_pair(chunkX, chunkZ);
    entityBlockCache[chunkKey] = entityBlocks;
}


// 修改 LoadAndCacheBlockData，使其处理整个 chunk 的所有子区块
void LoadAndCacheBlockData(int chunkX, int chunkZ) {
    // 计算区域坐标
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);

    // 获取区域数据
    const auto& regionData = getRegionFromCache(regionX, regionZ);

    // 获取区块数据
    std::vector<char> chunkData = GetChunkNBTData(regionData, mod32(chunkX), mod32(chunkZ));
    size_t index = 0;
    auto tag = readTag(chunkData, index);

    auto yPosTag = getChildByName(tag, "yPos");
    if (yPosTag && yPosTag->type == TagType::INT) {
        minSectionY = bytesToInt(yPosTag->payload);
    }
    // 处理高度图
    auto heightMapsTag = getChildByName(tag, "Heightmaps");
    if (heightMapsTag && heightMapsTag->type == TagType::COMPOUND) {
        std::vector<std::string> mapTypes = {
            "MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES",
            "OCEAN_FLOOR", "WORLD_SURFACE"
        };

        for (const auto& mapType : mapTypes) {
            auto mapDataTag = getChildByName(heightMapsTag, mapType);
            if (mapDataTag && mapDataTag->type == TagType::LONG_ARRAY) {
                size_t numLongs = mapDataTag->payload.size() / sizeof(int64_t);
                const int64_t* rawData = reinterpret_cast<const int64_t*>(mapDataTag->payload.data());
                std::vector<int64_t> longData(rawData, rawData + numLongs);

                std::vector<int> heights = decodeHeightMap(longData);
                heightMapCache[std::make_pair(chunkX, chunkZ)][mapType] = heights;
            }
        }
    }
    //提取实体方块
    auto blockEntitiesTag = getChildByName(tag, "block_entities");
    if (blockEntitiesTag && blockEntitiesTag->type == TagType::LIST) {
        ProcessEntityBlocks(chunkX, chunkZ, blockEntitiesTag); // 处理实体方块
    }

    // 提取所有子区块
    auto sectionsTag = getChildByName(tag, "sections");
    if (!sectionsTag || sectionsTag->type != TagType::LIST) {
        return; // 没有子区块
    }

    // 遍历所有子区块
    for (const auto& sectionTag : sectionsTag->children) {
        int sectionY = -1;
        auto yTag = getChildByName(sectionTag, "Y");
        
        if (yTag && yTag->type == TagType::BYTE) {
            sectionY = static_cast<int>(yTag->payload[0]);
        }

        // 处理子区块
        ProcessSection(chunkX, chunkZ, sectionY, sectionTag);
    }
}

// --------------------------------------------------------------------------------
// 缓存释放函数
// --------------------------------------------------------------------------------
void ReleaseSectionCache() {
    // 先清空，再 shrink_to_fit（但 unordered_map 没有 shrink_to_fit，所以直接替换更有效）
    sectionCache.clear();
    sectionCache = {};  // 强制释放内存
}
// --------------------------------------------------------------------------------
// 方块ID查询相关函数
// --------------------------------------------------------------------------------
// 获取方块ID
int GetBlockId(int blockX, int blockY, int blockZ) {
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    int sectionY;
    blockYToSectionY(blockY, sectionY);
    int adjustedSectionY = AdjustSectionY(sectionY);
    auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);
    if (sectionCache.find(blockKey) == sectionCache.end()) {
        LoadAndCacheBlockData(chunkX, chunkZ);
    }

    const auto& blockData = sectionCache[blockKey].blockData;
    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);
    int yzx = toYZX(relativeX, relativeY, relativeZ);

    return (yzx < blockData.size()) ? blockData[yzx] : 0;
}

// 获取天空光照
int GetSkyLight(int blockX, int blockY, int blockZ) {
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    int sectionY;
    blockYToSectionY(blockY, sectionY);
    int adjustedSectionY = AdjustSectionY(sectionY);
    auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);

    if (sectionCache.find(blockKey) == sectionCache.end()) {
        LoadAndCacheBlockData(chunkX, chunkZ);
    }

    const auto& skyLightData = sectionCache[blockKey].skyLight;
    if (skyLightData.size() == 1) {
        return skyLightData[0]; // 标记为-1或-2
    }

    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);
    int yzx = toYZX(relativeX, relativeY, relativeZ);

    return (yzx < skyLightData.size()) ? skyLightData[yzx] : 0;
}

int GetBlockLight(int blockX, int blockY, int blockZ) {
    int chunkX, chunkZ;
    blockToChunk(blockX, blockZ, chunkX, chunkZ);

    int sectionY;
    blockYToSectionY(blockY, sectionY);
    int adjustedSectionY = AdjustSectionY(sectionY);
    auto blockKey = std::make_tuple(chunkX, chunkZ, adjustedSectionY);

    if (sectionCache.find(blockKey) == sectionCache.end()) {
        LoadAndCacheBlockData(chunkX, chunkZ);
    }

    const auto& blockLightData = sectionCache[blockKey].blockLight;
    if (blockLightData.size() == 1) {
        return blockLightData[0]; // 标记为-1或-2
    }

    int relativeX = mod16(blockX);
    int relativeY = mod16(blockY);
    int relativeZ = mod16(blockZ);
    int yzx = toYZX(relativeX, relativeY, relativeZ);

    return (yzx < blockLightData.size()) ? blockLightData[yzx] : 0;
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

int GetLevel(int blockX, int blockY, int blockZ) {
    int currentId = GetBlockId(blockX, blockY, blockZ);
    Block currentBlock = GetBlockById(currentId);
    std::string baseName = currentBlock.GetNameAndNameSpaceWithoutState(); // 获取带命名空间的完整名称

    // 判断当前方块是否是注册流体或已有level标记
    bool isFluid = fluidDefinitions.find(baseName) != fluidDefinitions.end();

    if (isFluid || currentBlock.level == 0) {
        // 检查上方方块
        int upperId = GetBlockId(blockX, blockY + 1, blockZ);
        Block upperBlock = GetBlockById(upperId);
        std::string upperBaseName = upperBlock.GetNameAndNameSpaceWithoutState();

        bool upperIsFluid = fluidDefinitions.find(upperBaseName) != fluidDefinitions.end();

        if (upperIsFluid || upperBlock.level == 0) {
            return 8; // 上方是流体
        }
        else {
            return currentBlock.level; // 当前流体level
        }
    }

    return currentBlock.air ? -1 : -2; // 空气返回-1，固体返回-2
}
// 获取方块ID时同时获取相邻方块的air状态，返回当前方块ID
int GetBlockIdWithNeighbors(
    int blockX, int blockY, int blockZ,
    bool* neighborIsAir,
    int* fluidLevels) {
    int currentId = GetBlockId(blockX, blockY, blockZ);
    Block currentBlock = GetBlockById(currentId);
    std::string currentBaseName = currentBlock.GetNameAndNameSpaceWithoutState();

    bool isFluid = fluidDefinitions.find(currentBaseName) != fluidDefinitions.end();
    bool hasFluidData = (currentBlock.level != -1);

    // 统一处理 neighborIsAir 数组（6个方向）
    if (neighborIsAir != nullptr) {
        static const std::array<std::tuple<int, int, int>, 6> directions = { {
            {0, 1, 0},    // 上（Y+）
            {0, -1, 0},   // 下（Y-）
            {-1, 0, 0},   // 西（X-）
            {1, 0, 0},    // 东（X+）
            {0, 0, -1},   // 北（Z-）
            {0, 0, 1}     // 南（Z+）
        } };

        for (size_t i = 0; i < directions.size(); ++i) {
            int dx, dy, dz;
            std::tie(dx, dy, dz) = directions[i];
            int nx = blockX + dx;
            int ny = blockY + dy;
            int nz = blockZ + dz;

            // 如果启用了保留边界面，则直接判断
            if (config.keepBoundary &&
                ((nx == config.maxX + 1) || (nx == config.minX - 1) ||
                    (nz == config.maxZ + 1) || (nz == config.minZ - 1))) {
                neighborIsAir[i] = true;
                continue;
            }

            int neighborId = GetBlockId(nx, ny, nz);
            Block neighborBlock = GetBlockById(neighborId);

            if (hasFluidData) {
                std::string neighborBase = neighborBlock.GetNameAndNameSpaceWithoutState();
                bool isSameFluid = (currentBaseName == neighborBase);
                bool neighborIsFluid = (fluidDefinitions.find(neighborBase) != fluidDefinitions.end());
                neighborIsAir[i] = (isSameFluid && (neighborBlock.level != 0 && neighborBlock.level != -1)) ||
                    (neighborBlock.level != 0 && !neighborIsFluid && neighborBlock.air);
            }
            else {
                neighborIsAir[i] = neighborBlock.air;
            }
        }
    }

    // 处理 fluidLevels 数组，仅在存在流体数据且数组不为空时进行
    if (hasFluidData && fluidLevels != nullptr) {
        // 中心块的流体等级
        fluidLevels[0] = GetLevel(blockX, blockY, blockZ);
        static const std::array<std::tuple<int, int, int>, 9> levelDirections = { {
            {0, 0, -1},   // 北
            {0, 0, 1},    // 南
            {1, 0, 0},    // 东
            {-1, 0, 0},   // 西
            {1, 0, -1},   // 东北
            {-1, 0, -1},  // 西北
            {1, 0, 1},    // 东南
            {-1, 0, 1},   // 西南
            {0, 1, 0}     // 上
        } };
        for (size_t i = 0; i < levelDirections.size(); ++i) {
            int dx, dy, dz;
            std::tie(dx, dy, dz) = levelDirections[i];
            fluidLevels[i + 1] = GetLevel(blockX + dx, blockY + dy, blockZ + dz);
        }
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
    globalBlockPalette.emplace_back(Block("minecraft:air"));
}

std::vector<Block> GetGlobalBlockPalette() {
    return globalBlockPalette;
}

