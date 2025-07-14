#pragma once

// C++ 标准库头文件
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <shared_mutex>
#include <unordered_set>
#include <vector>

// 项目头文件
#include "config.h"
#include "model.h"
#include "chunk.h"
#include "nbtutils.h"
#include "hashutils.h"
#include "Fluid.h"
#include "GlobalCache.h"
extern Config config;

// 内存监控相关的 extern 声明
extern std::shared_mutex entityBlockCacheMutex; // 新增 EntityBlockCache 的互斥锁
extern std::shared_mutex heightMapCacheMutex;   // 新增 heightMapCache 的互斥锁

// 缓存的 extern 声明，以便 MemoryMonitor 可以访问
// 类型定义直接使用 MemoryMonitor.h 中的别名以确保一致性，
// 或者确保这里的类型与 block.cpp 中的定义以及 MemoryMonitor.h 中的别名完全匹配。
// 为了简单起见，这里我们假设 MemoryMonitor.h 中的别名是准确的。
// 注意：实际项目中，更推荐的方式是在一个统一的头文件中定义这些缓存的类型和 extern 声明，
// 或者 MemoryMonitor.h 直接包含 block.cpp 中定义的缓存（但这会导致更紧密的耦合）。
// 这里我们选择在 block.h 中添加 extern 声明，并期望类型定义正确。

// #include "MemoryMonitor.h" // 可以考虑包含这个，但可能导致循环依赖，取决于 MemoryMonitor.h 是否也需要 block.h
// 如果不包含 MemoryMonitor.h，需要确保以下类型与 MemoryMonitor.h 中的别名以及 block.cpp 中的实际类型一致

// 假设 EntityBlock 和 SectionCacheEntry 等已在此文件或其包含的头文件中定义
class EntityBlock; // 前向声明或确保 EntityBlock.h 已被包含
struct SectionCacheEntry; // 确保 SectionCacheEntry 已定义

extern std::unordered_map<std::pair<int, int>, std::vector<std::shared_ptr<EntityBlock>>, pair_hash> EntityBlockCache;
extern std::unordered_map<std::pair<int, int>, std::unordered_map<std::string, std::vector<int>>, pair_hash> heightMapCache;
extern std::unordered_map<std::tuple<int, int, int>, SectionCacheEntry, triple_hash> sectionCache;

struct Block {
    std::string name;
    int8_t level;
    bool air;

    Block(const std::string& name) : name(name), level(-1), air(true) {
        // 用 string_view 减少 substr 拷贝,仅对 baseName 做一次 std::string 构造
        std::string_view fullName(name);
        size_t bracketPos = fullName.find('[');
        std::string_view baseNameView = (bracketPos != std::string_view::npos) ?
            fullName.substr(0, bracketPos) : fullName;
        std::string baseName(baseNameView);

        // 解析方块状态属性 (用 string_view 减少拷贝)
        std::unordered_map<std::string, std::string> states;
        if (bracketPos != std::string_view::npos) {
            size_t closePos = fullName.find(']', bracketPos + 1);
            std::string_view stateStrView = fullName.substr(bracketPos + 1, closePos - bracketPos - 1);
            size_t start = 0;
            while (start < stateStrView.size()) {
                size_t end = stateStrView.find(',', start);
                std::string_view pairView = (end == std::string_view::npos) ?
                    stateStrView.substr(start) :
                    stateStrView.substr(start, end - start);
                size_t eqPos = pairView.find(':');
                if (eqPos != std::string_view::npos) {
                    states.emplace(
                        std::string(pairView.substr(0, eqPos)),
                        std::string(pairView.substr(eqPos + 1))
                    );
                }
                if (end == std::string_view::npos) break;
                start = end + 1;
            }
        }

        /* 流体处理逻辑 */
        bool fluidProcessed = false;

        // 阶段1:检查强制含水方块
        if (!fluidProcessed) {
            for (const auto& fluidEntry : fluidDefinitions) {
                const FluidInfo& info = fluidEntry.second;
                if (info.liquid_blocks.count(baseName)) {
                    level = 0;
                    fluidProcessed = true;
                    break;
                }
            }
        }
        // 阶段2:检查流体属性(如waterlogged)
        for (const auto& fluidEntry : fluidDefinitions) {
            const std::string& fluidName = fluidEntry.first;
            const FluidInfo& info = fluidEntry.second;

            if (!info.property.empty() &&
                states.count(info.property) &&
                states[info.property] == "true") {
                level = 0;
                fluidProcessed = true;
                break;
            }
        }

        

        // 阶段3:处理流体自身level属性
        if (!fluidProcessed) {
            const auto& it = fluidDefinitions.find(baseName);
            if (it != fluidDefinitions.end()) {
                const FluidInfo& info = it->second;
                std::string levelProp = info.level_property.empty() ?
                    "level" : info.level_property;

                try {
                    level = states.count(levelProp) ?
                        std::stoi(states[levelProp]) : 0;
                }
                catch (...) {
                    level = 0;
                }
                fluidProcessed = true;
            }
        }

        // 原有空气判断逻辑
        air = (solidBlocks.find(baseName) == solidBlocks.end());


    }
    Block(const std::string& name, bool air) : name(name), level(-1), air(air) {}

    // 方法:获取命名空间部分
    std::string GetNamespace() const {
        size_t colonPos = name.find(':'); // 查找第一个冒号的位置
        if (colonPos != std::string::npos) { // 如果找到冒号
            return name.substr(0, colonPos);
        }
        else { // 如果没有找到冒号
            return "minecraft"; // 默认命名空间
        }
    }

    std::string GetNameAndNameSpaceWithoutState() const {
        size_t bracketPos = name.find('[');  // 查找第一个方括号的位置

        // 如果没有方括号,返回完整的字符串
        if (bracketPos == std::string::npos) {
            return name;
        }

        // 如果有方括号,返回方括号之前的部分
        return name.substr(0, bracketPos);
    }
    // 保留命名空间和基础名字,只处理状态键值对
    std::string GetModifiedNameWithNamespace() const {
        // 提取命名空间部分
        size_t colonPos = name.find(':');
        std::string namespacePart;
        std::string baseNamePart;

        if (colonPos != std::string::npos) {
            namespacePart = name.substr(0, colonPos + 1); // 包括冒号本身
            baseNamePart = name.substr(colonPos + 1);
        }
        else {
            namespacePart = ""; // 无命名空间
            baseNamePart = name;
        }

        // 提取基础名字和状态键值对部分
        size_t bracketPos = baseNamePart.find('[');
        std::string blockName;
        std::string stateStr;

        if (bracketPos != std::string::npos) {
            blockName = baseNamePart.substr(0, bracketPos);
            stateStr = baseNamePart.substr(bracketPos + 1, baseNamePart.size() - bracketPos - 2); // 去除最后的 ]
        }
        else {
            blockName = baseNamePart;
            stateStr = "";
        }

        // 解析状态键值对,并过滤掉指定的键
        std::vector<std::string> statePairs;
        std::string pair;

        for (const char c : stateStr) {
            if (c == ',') {
                statePairs.push_back(pair);
                pair.clear();
            }
            else {
                pair.push_back(c);
            }
        }

        if (!pair.empty()) {
            statePairs.push_back(pair);
        }

        std::vector<std::string> filteredPairs;

        for (const auto& pairStr : statePairs) {
            size_t equalPos = pairStr.find(':');
            std::string key = pairStr.substr(0, equalPos);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower); // 转换为小写

            // 修改这里：保留waterlogged属性，只过滤distance和persistent
            if (key != "distance" && key != "persistent") {
                filteredPairs.push_back(pairStr);
            }
        }

        // 重新组合状态键值对,并替换冒号为等号
        std::string filteredState;
        for (const auto& pair : filteredPairs) {
            if (!filteredState.empty()) {
                filteredState += ",";
            }
            // 替换冒号为等号
            std::string transformedPair = pair;
            std::replace(transformedPair.begin(), transformedPair.end(), ':', '=');
            filteredState += transformedPair;
        }

        // 组合最终结果
        std::string result;
        if (!namespacePart.empty()) {
            result += namespacePart;
        }
        result += blockName;

        if (!filteredPairs.empty()) {
            result += "[" + filteredState + "]";
        }

        return result;
    }

    // 获取不带 waterlogged, distance, persistent 键值对的方块完整名字
    std::string GetBlockNameWithoutProperties() const {
        // 从完整名字中提取 base 名字和状态部分
        size_t bracketPos = name.find('[');
        std::string baseName = (bracketPos != std::string::npos) ? name.substr(0, bracketPos) : name;

        // 如果没有状态部分,直接返回原名
        if (bracketPos == std::string::npos) {
            return name;
        }

        // 提取状态字符串
        std::string stateStr = name.substr(bracketPos + 1, name.size() - bracketPos - 2); // 去掉最后的 ]

        // 解析状态字符串为键值对
        std::vector<std::string> statePairs;
        std::string pair;
        for (const char c : stateStr) {
            if (c == ',') {
                statePairs.push_back(pair);
                pair.clear();
            }
            else {
                pair.push_back(c);
            }
        }
        if (!pair.empty()) {
            statePairs.push_back(pair);
            pair.clear();
        }

        // 过滤掉需要移除的键值对
        std::vector<std::string> filteredPairs;
        for (const auto& pairStr : statePairs) {
            if (!pairStr.empty()) {
                // 分离键和值
                size_t equalPos = pairStr.find(':');
                std::string key = pairStr.substr(0, equalPos);
                std::string value = (equalPos != std::string::npos) ? pairStr.substr(equalPos + 1) : "";

                // 判断是否需要保留该键值对
                bool keep = true;
                // 修改这里：保留waterlogged属性，只过滤distance和persistent
                if (key == "distance" || key == "persistent") {
                    keep = false; // 移除这些键
                }

                if (keep) {
                    filteredPairs.push_back(pairStr);
                }
            }
        }

        // 重新组合状态部分
        std::string filteredState;
        for (const auto& pair : filteredPairs) {
            if (!filteredState.empty()) {
                filteredState += ",";
            }
            filteredState += pair;
        }

        // 返回没有指定键值对的完整名字
        if (filteredState.empty()) {
            return baseName;
        }
        else {
            return baseName + "[" + filteredState + "]";
        }
    }

    // 新函数:同时获取 GetName 和 GetBlockNameWithoutProperties 的效果
    std::string GetModifiedName() const {
        size_t colonPos = name.find(':');
        std::string baseNamePart;

        if (colonPos != std::string::npos) {
            baseNamePart = name.substr(colonPos + 1); // 获取冒号后的内容
        }
        else {
            baseNamePart = name; // 没有命名空间时,整个字符串作为基础名
        }

        size_t bracketPos = baseNamePart.find('[');
        std::string blockName;

        if (bracketPos != std::string::npos) {
            blockName = baseNamePart.substr(0, bracketPos);
        }
        else {
            blockName = baseNamePart;
        }

        std::string stateStr;

        if (bracketPos != std::string::npos) {
            stateStr = baseNamePart.substr(bracketPos + 1, baseNamePart.size() - bracketPos - 2);
        }

        // 解析状态字符串并过滤特定键
        std::vector<std::string> statePairs;
        std::string pair;

        for (const char c : stateStr) {
            if (c == ',') {
                statePairs.push_back(pair);
                pair.clear();
            }
            else {
                pair.push_back(c);
            }
        }

        if (!pair.empty()) {
            statePairs.push_back(pair);
        }

        std::vector<std::string> filteredPairs;

        for (const auto& pairStr : statePairs) {
            size_t equalPos = pairStr.find(':');
            std::string key = pairStr.substr(0, equalPos);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower); // 转换为小写

            // 修改这里：保留waterlogged属性，只过滤distance和persistent
            if (key != "distance" && key != "persistent") {
                filteredPairs.push_back(pairStr);
            }
        }

        // 重新组合过滤后的状态键值对
        std::string filteredState;

        for (const auto& pair : filteredPairs) {
            if (!filteredState.empty()) {
                filteredState += ",";
            }
            filteredState += pair;
        }

        if (filteredPairs.empty()) {
            // 如果没有状态键值对,直接返回 blockName
            return blockName;
        }
        else {
            // 替换冒号为等号
            std::string result = blockName + "[";
            std::replace(filteredState.begin(), filteredState.end(), ':', '=');
            result += filteredState + "]";
            return result;
        }
    }
};

struct SectionCacheEntry {
    // 把频繁访问的大数组放在前面,减少访存跨缓存行
    std::vector<int> skyLight;      // 天空光照数据
    std::vector<int> blockLight;    // 方块光照数据
    std::vector<int> blockData;     // 方块数据
    std::vector<int> biomeData;     // 生物群系数据
    std::vector<std::string> blockPalette; // 方块调色板 (相对较小)
};

extern std::vector<Block> globalBlockPalette;
extern std::unordered_map<std::tuple<int, int, int>, SectionCacheEntry, triple_hash> sectionCache;
extern std::unordered_map<std::pair<int, int>, std::unordered_map<std::string, std::vector<int>>, pair_hash> heightMapCache;

// 全局读写锁:保护 sectionCache 线程安全
extern std::shared_mutex sectionCacheMutex;

// 保护 EntityBlockCache 与 heightMapCache 的读写
extern std::shared_mutex chunkAuxCacheMutex;

// 高度图类型
static const std::vector<std::string> mapTypes = {"MOTION_BLOCKING", "MOTION_BLOCKING_NO_LEAVES",   "OCEAN_FLOOR", "WORLD_SURFACE"};

void LoadAndCacheBlockData(int chunkX, int chunkZ);

void UpdateSkyLightNeighborFlags();

int GetBlockId(int blockX, int blockY, int blockZ);

// 获取方块ID时同时获取相邻方块的air状态,返回当前方块ID
int GetBlockIdWithNeighbors(int blockX, int blockY, int blockZ,bool* neighborIsAir = nullptr,int* fluidLevels = nullptr);

int GetSkyLight(int blockX, int blockY, int blockZ);

int GetBlockLight(int blockX, int blockY, int blockZ);

int GetLevel(int blockX, int blockY, int blockZ);

int GetHeightMapY(int blockX, int blockZ, const std::string& heightMapType);

void ClearSectionCacheForChunk(int chunkX, int chunkZ);

// 获取方块名称转换为Block对象
Block GetBlockById(int blockId);

// 返回全局的block对照表(Block对象)
std::vector<Block> GetGlobalBlockPalette();


// 初始化,注册"minecraft:air"为ID0
void InitializeGlobalBlockPalette();


