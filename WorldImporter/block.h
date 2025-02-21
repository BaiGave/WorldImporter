#ifndef BLOCK_H
#define BLOCK_H

#include "config.h"
extern Config config;

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <iostream>
#include <tuple>
#include <utility>
#include <cctype>  // for tolower
#include <regex>   // for regex matching

// 自定义哈希函数声明
struct pair_hash;
struct triple_hash;

extern std::unordered_set<std::string> solidBlocks; // 改为哈希表

struct Block {
    std::string name;
    bool air;

    Block(const std::string& name) : name(name), air(true) {
        size_t bracketPos = name.find('[');
        std::string a = (bracketPos != std::string::npos) ?
            name.substr(0, bracketPos) : name;

        // 检查哈希表
        air = (solidBlocks.find(a) == solidBlocks.end());
    }
    Block(const std::string& name, bool air) : name(name), air(air) {}

    // 方法：获取命名空间部分
    std::string GetNamespace() const {
        size_t colonPos = name.find(':'); // 查找第一个冒号的位置
        if (colonPos != std::string::npos) { // 如果找到冒号
            return name.substr(0, colonPos);
        }
        else { // 如果没有找到冒号
            return "minecraft"; // 默认命名空间
        }
    }

    // 方法：获取方块名称部分（冒号之后的部分）
    std::string GetName() const {
        size_t colonPos = name.find(':'); // 查找第一个冒号的位置
        if (colonPos != std::string::npos) { // 如果找到冒号
            return name.substr(colonPos + 1); // 返回冒号之后的部分
        }
        else { // 如果没有找到冒号
            return name; // 返回完整的字符串
        }
    }

    std::string GetNameWithoutState() const {
        size_t colonPos = name.find(':'); // 查找第一个冒号的位置
        size_t bracketPos = name.find('[');  // 查找第一个方括号的位置

        // 如果没有冒号，返回完整字符串
        if (colonPos == std::string::npos) {
            return name.substr(0, bracketPos == std::string::npos ? name.size() : bracketPos).c_str();
        }

        // 如果有冒号，返回冒号之后到方括号之间的部分
        if (bracketPos == std::string::npos) {
            return name.substr(colonPos + 1);
        }

        // 如果冒号后在方括号之前，返回这部分
        if (colonPos + 1 < bracketPos) {
            return name.substr(colonPos + 1, bracketPos - colonPos - 1).c_str();
        }

        // 否则返回空
        return "";
    }
    // 保留命名空间和基础名字，只处理状态键值对
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

        // 解析状态键值对，并过滤掉指定的键
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

            if (key != "waterlogged" && key != "distance" && key != "persistent") {
                filteredPairs.push_back(pairStr);
            }
        }

        // 重新组合状态键值对，并替换冒号为等号
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

        // 如果没有状态部分，直接返回原名
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
                if (key == "waterlogged" || key == "distance" || key == "persistent") {
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

    // 新函数：同时获取 GetName 和 GetBlockNameWithoutProperties 的效果
    std::string GetModifiedName() const {
        size_t colonPos = name.find(':');
        std::string baseNamePart;

        if (colonPos != std::string::npos) {
            baseNamePart = name.substr(colonPos + 1); // 获取冒号后的内容
        }
        else {
            baseNamePart = name; // 没有命名空间时，整个字符串作为基础名
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

            if (key != "waterlogged" && key != "distance" && key != "persistent") {
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
            // 如果没有状态键值对，直接返回 blockName
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
extern std::vector<Block> globalBlockPalette;


// 获取区块NBT数据的函数
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z);

void LoadCacheBlockDataAutomatically(int chunkX, int chunkZ, int sectionY);

void LoadAndCacheBlockData(int chunkX, int chunkZ, int sectionY, const std::tuple<int, int, int>& blockKey);
// 通过x, y, z坐标获取方块ID
int GetBlockId(int blockX, int blockY, int blockZ);

// 获取方块名称转换为Block对象
Block GetBlockById(int blockId);

// 通过ID获取方块名称
std::string GetBlockNameById(int blockId);

std::string GetBlockNamespaceById(int blockId);

// 获取方块ID时同时获取相邻方块的air状态，返回当前方块ID
int GetBlockIdWithNeighbors(int blockX, int blockY, int blockZ, bool* neighborIsAir);

// 返回全局的block对照表(Block对象)
std::vector<Block> GetGlobalBlockPalette();

// 初始化，注册"minecraft:air"为ID0
void InitializeGlobalBlockPalette();

void loadSolidBlocks(const std::string& filepath);


#endif  // BLOCK_H