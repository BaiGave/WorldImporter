#include "blockstate.h"
#include "fileutils.h"
#include "objExporter.h"
#include <regex>
#include <random>
#include <numeric>
#include <Windows.h>
#include <iostream>
#include <sstream>
std::unordered_map<std::string, std::unordered_map<std::string, ModelData>> BlockModelCache;
// --------------------------------------------------------------------------------
// 条件匹配函数
// --------------------------------------------------------------------------------
bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when) {
    // 判断 when 条件是否满足
    if (when.is_object()) {
        // 处理 OR 条件
        if (when.contains("OR")) {
            for (const auto& condition : when["OR"]) {
                if (matchConditions(blockConditions, condition)) {
                    return true;
                }
            }
            return false;
        }

        // 处理 AND 条件
        if (when.contains("AND")) {
            for (const auto& condition : when["AND"]) {
                if (!matchConditions(blockConditions, condition)) {
                    return false;
                }
            }
            return true;
        }

        // 处理普通属性条件
        for (auto& condition : when.items()) {
            std::string key = condition.key();
            std::string value = condition.value();

            if (blockConditions.find(key) != blockConditions.end()) {
                // 值包含 "|" 时，视为 "或" 条件
                if (value.find('|') != std::string::npos) {
                    std::vector<std::string> options;
                    size_t pos = 0;
                    while ((pos = value.find('|')) != std::string::npos) {
                        options.push_back(value.substr(0, pos));
                        value.erase(0, pos + 1);
                    }
                    options.push_back(value);

                    if (std::find(options.begin(), options.end(), blockConditions.at(key)) != options.end()) {
                        continue;
                    }
                    else {
                        return false;
                    }
                }
                else if (blockConditions.at(key) != value) { // 严格匹配
                    return false;
                }
            }
            else {
                return false;
            }
        }
    }
    return true;
}

// 辅助函数：将键值对字符串解析为map
std::unordered_map<std::string, std::string> ParseKeyValuePairs(const std::string& input) {
    std::unordered_map<std::string, std::string> result;
    if (input.empty()) return result;

    std::stringstream ss(input);
    std::string pair;
    while (std::getline(ss, pair, ',')) {
        size_t eqPos = pair.find('=');
        if (eqPos != std::string::npos) {
            std::string key = pair.substr(0, eqPos);
            std::string value = pair.substr(eqPos + 1);
            result[key] = value;
        }
    }
    return result;
}

// 检查 subset 的所有键值对是否存在于 superset 中且值相同
bool IsSubset(
    const std::unordered_map<std::string, std::string>& subset,
    const std::unordered_map<std::string, std::string>& superset
) {
    for (const auto& kv : subset) {
        auto it = superset.find(kv.first);
        if (it == superset.end() || it->second != kv.second) {
            return false;
        }
    }
    return true;
}
// --------------------------------------------------------------------------------
// 字符串分割函数
// --------------------------------------------------------------------------------
std::vector<std::string> SplitString(const std::string& input, char delimiter) {
    std::vector<std::string> result;
    std::istringstream stream(input);
    std::string token;
    while (std::getline(stream, token, delimiter)) {
        result.push_back(token);
    }
    return result;
}

// --------------------------------------------------------------------------------
// 编码和排序函数
// --------------------------------------------------------------------------------
std::string SortedVariantKey(const std::string& key) {
    static const std::regex keyRegex(R"(([^,=]+)=([^,]+))");
    std::smatch keyMatch;

    std::map<std::string, std::string> keyMap;
    std::vector<std::string> parts = SplitString(key, ',');

    for (const auto& part : parts) {
        std::smatch match;
        if (std::regex_match(part, match, keyRegex)) {
            std::string name = match[1].str();
            std::string value = match[2].str();
            keyMap[name] = value;
        }
    }

    std::stringstream ss;
    bool first = true;
    for (const auto& entry : keyMap) {
        if (!first) {
            ss << ",";
        }
        ss << entry.first << "=" << entry.second;
        first = false;
    }

    return ss.str();
}

// 计算矩阵尺寸
int CalculateMatrixSize(int variantCount) {
    return static_cast<int>(std::ceil(std::sqrt(variantCount)));
}

// --------------------------------------------------------------------------------
// JSON 文件读取函数
// --------------------------------------------------------------------------------
nlohmann::json GetBlockstateJson(const std::string& namespaceName, const std::string& blockId) {
    std::string cacheKey = namespaceName + ":" + blockId;
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
    auto it = GlobalCache::blockstates.find(cacheKey);
    if (it != GlobalCache::blockstates.end()) {
        return it->second;
    }
    std::cerr << "Blockstate not found: " << cacheKey << std::endl;
    return nlohmann::json();
}

// --------------------------------------------------------------------------------
// 方块状态 JSON 处理
// --------------------------------------------------------------------------------
std::unordered_map<std::string, ModelData> ProcessBlockstateJson(const std::string& namespaceName, const std::vector<std::string>& blockIds) {
    std::unordered_map<std::string, ModelData> result;
    auto& namespaceCache = BlockModelCache[namespaceName];

    for (const auto& blockId : blockIds) {
        if (namespaceCache.find(blockId) != namespaceCache.end()) {
            continue;
        }

        // 解析 blockId 和条件
        static const std::regex blockIdRegex(R"(^(.*?)\[(.*)\]$)");
        std::smatch match;
        std::string baseBlockId = blockId;
        std::string condition;
        std::unordered_map<std::string, std::string> blockConditions;

        if (std::regex_match(blockId, match, blockIdRegex)) {
            baseBlockId = match.str(1);
            condition = match.str(2);

            static const std::regex conditionRegex(R"((\w+)=([^,]+))");
            auto conditionsBegin = std::sregex_iterator(condition.begin(), condition.end(), conditionRegex);
            auto conditionsEnd = std::sregex_iterator();

            for (auto i = conditionsBegin; i != conditionsEnd; ++i) {
                std::smatch submatch = *i;
                blockConditions[submatch[1].str()] = submatch[2].str();
            }
        }

        // 读取 blockstate JSON
        nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);
        if (blockstateJson.is_null()) {
            continue;
        }

        ModelData mergedModel;
        std::vector<ModelData> selectedModels;

        // 处理 variants
        if (blockstateJson.contains("variants")) {
            for (auto& variant : blockstateJson["variants"].items()) {
                std::string variantKey = variant.key();
                std::string normalizedCondition = SortedVariantKey(condition);
                std::string normalizedVariantKey = SortedVariantKey(variantKey);

                // 解析为键值对
                auto conditionMap = ParseKeyValuePairs(normalizedCondition);
                auto variantMap = ParseKeyValuePairs(normalizedVariantKey);

                // 判断条件：variantMap 的所有键值对需存在于 conditionMap 中
                if (condition.empty() || IsSubset(variantMap, conditionMap)) {
                    int rotationX = 0, rotationY = 0;
                    bool uvlock = false;

                    if (variant.value().contains("x")) {
                        rotationX = variant.value()["x"].get<int>();
                    }
                    if (variant.value().contains("y")) {
                        rotationY = variant.value()["y"].get<int>();
                    }
                    if (variant.value().contains("uvlock")) {
                        uvlock = variant.value()["uvlock"].get<bool>();
                    }
                    // 处理模型加权数组
                    if (variant.value().is_array()) {
                        int totalWeight = 0;
                        std::vector<std::pair<nlohmann::json, int>> modelsWithWeights;

                        for (const auto& item : variant.value()) {
                            int weight = item.contains("weight") ? item["weight"].get<int>() : 1;
                            modelsWithWeights.push_back({ item, weight });
                            totalWeight += weight;
                        }

                        // 随机选择模型
                        if (totalWeight > 0) {
                            std::random_device rd;
                            std::mt19937 gen(rd());
                            std::uniform_int_distribution<> dis(1, totalWeight);
                            int randomWeight = dis(gen);
                            int cumulativeWeight = 0;

                            for (const auto& model : modelsWithWeights) {
                                cumulativeWeight += model.second;
                                if (randomWeight <= cumulativeWeight) {
                                    std::string modelId = model.first.contains("model") ? model.first["model"].get<std::string>() : "";
                                    if (!modelId.empty()) {
                                        size_t colonPos = modelId.find(':');
                                        std::string modelNamespace = namespaceName;

                                        if (colonPos != std::string::npos) {
                                            modelNamespace = modelId.substr(0, colonPos);
                                            modelId = modelId.substr(colonPos + 1);
                                        }

                                        ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock);
                                        selectedModels.push_back(selectedModel);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    else {
                        std::string modelId = variant.value().contains("model") ? variant.value()["model"].get<std::string>() : "";
                        if (!modelId.empty()) {
                            size_t colonPos = modelId.find(':');
                            std::string modelNamespace = namespaceName;

                            if (colonPos != std::string::npos) {
                                modelNamespace = modelId.substr(0, colonPos);
                                modelId = modelId.substr(colonPos + 1);
                            }

                            ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock);
                            selectedModels.push_back(selectedModel);
                        }
                    }
                }
            }
        }

        // 处理 multipart
        if (blockstateJson.contains("multipart")) {
            auto multipart = blockstateJson["multipart"];
            for (const auto& item : multipart) {
                if (item.contains("apply")) {
                    auto apply = item["apply"];
                    bool conditionMatched = true;

                    if (item.contains("when")) {
                        conditionMatched = matchConditions(blockConditions, item["when"]);
                    }

                    if (conditionMatched) {
                        int rotationX = 0, rotationY = 0;
                        bool uvlock = false; // 新增 uvlock 捕获

                        if (apply.contains("x")) {
                            rotationX = apply["x"].get<int>();
                        }
                        if (apply.contains("y")) {
                            rotationY = apply["y"].get<int>();
                        }
                        // 新增 uvlock 处理
                        if (apply.contains("uvlock")) {
                            uvlock = apply["uvlock"].get<bool>();
                        }


                        // 处理 apply 数组和单个模型
                        if (apply.is_array()) {
                            int totalWeight = 0;
                            std::vector<std::pair<nlohmann::json, int>> modelsWithWeights;

                            for (const auto& modelItem : apply) {
                                int weight = modelItem.contains("weight") ? modelItem["weight"].get<int>() : 1;
                                modelsWithWeights.push_back({ modelItem, weight });
                                totalWeight += weight;
                            }

                            if (totalWeight > 0) {
                                std::random_device rd;
                                std::mt19937 gen(rd());
                                std::uniform_int_distribution<> dis(1, totalWeight);
                                int randomWeight = dis(gen);
                                int cumulativeWeight = 0;

                                for (const auto& model : modelsWithWeights) {
                                    cumulativeWeight += model.second;
                                    if (randomWeight <= cumulativeWeight) {
                                        std::string modelId = model.first.contains("model") ? model.first["model"].get<std::string>() : "";
                                        if (!modelId.empty()) {
                                            size_t colonPos = modelId.find(':');
                                            std::string modelNamespace = namespaceName;

                                            if (colonPos != std::string::npos) {
                                                modelNamespace = modelId.substr(0, colonPos);
                                                modelId = modelId.substr(colonPos + 1);
                                            }

                                            ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock);
                                            selectedModels.push_back(selectedModel);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            std::string modelId = apply.contains("model") ? apply["model"].get<std::string>() : "";
                            if (!modelId.empty()) {
                                size_t colonPos = modelId.find(':');
                                std::string modelNamespace = namespaceName;

                                if (colonPos != std::string::npos) {
                                    modelNamespace = modelId.substr(0, colonPos);
                                    modelId = modelId.substr(colonPos + 1);
                                }

                                ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY, uvlock);
                                selectedModels.push_back(selectedModel);
                            }
                        }
                    }
                }
            }
        }

        // 合并模型
        if (!selectedModels.empty()) {
            mergedModel = selectedModels[0];
            for (size_t i = 1; i < selectedModels.size(); ++i) {
                mergedModel = MergeModelData(mergedModel, selectedModels[i]);
            }

            std::string fileName = blockId;
            size_t slashPos = fileName.find_last_of("/\\");
            if (slashPos != std::string::npos) {
                fileName = fileName.substr(slashPos + 1);
            }
        }
        /*if (blockId.find("note") != std::string::npos) {
            std::cout << "C" << blockId << std::endl;
        }*/
        // 缓存模型数据
        namespaceCache[blockId] = mergedModel;
        result[blockId] = mergedModel;
    }

    return result;
}

void ProcessBlockstateForBlocks(const std::vector<Block>& blocks) {
    std::unordered_map<std::string, std::vector<std::string>> namespaceToBlockIdsMap;

    // 将 Block 列表按命名空间分组
    for (const auto& block : blocks) {
        std::string namespaceName = block.GetNamespace(); // 使用 Block 结构体的 GetNamespace 方法
        std::string blockId = block.GetModifiedName();

        namespaceToBlockIdsMap[namespaceName].push_back(blockId);
    }

    // 处理每个命名空间下的方块 ID
    for (const auto& entry : namespaceToBlockIdsMap) {
        const std::string& namespaceName = entry.first;
        const std::vector<std::string>& blockIds = entry.second;

        auto blockstateData = ProcessBlockstateJson(namespaceName, blockIds);
        // 合并结果到全局缓存
        for (const auto& pair : blockstateData) {
            BlockModelCache[namespaceName][pair.first] = pair.second;
        }
    }

}
// --------------------------------------------------------------------------------
// 全局方块状态处理函数
// --------------------------------------------------------------------------------
void ProcessAllBlockstateVariants() {
    std::unordered_map<std::string, nlohmann::json> allBlockstates;
    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        allBlockstates = GlobalCache::blockstates;
    }

    ModelData mergedModel;
    int totalModelCount = 0;

    for (const auto& blockstatePair : allBlockstates) {
        const nlohmann::json& blockstateJson = blockstatePair.second;
        if (blockstateJson.contains("multipart")) continue;

        if (blockstateJson.contains("variants")) {
            totalModelCount += blockstateJson["variants"].size();
        }
    }

    const int matrixSize = CalculateMatrixSize(totalModelCount);
    const float spacing = 2.0f;

    int modelCount = 0;
    for (const auto& blockstatePair : allBlockstates) {
        const std::string& cacheKey = blockstatePair.first;
        const nlohmann::json& blockstateJson = blockstatePair.second;

        size_t colonPos = cacheKey.find(':');
        if (colonPos == std::string::npos) continue;
        std::string namespaceName = cacheKey.substr(0, colonPos);
        std::string baseBlockId = cacheKey.substr(colonPos + 1);

        if (blockstateJson.contains("multipart")) continue;

        if (blockstateJson.contains("variants")) {
            std::vector<std::string> variantBlockIds;
            const auto& variants = blockstateJson["variants"];

            for (const auto& variantEntry : variants.items()) {
                std::string variantKey = variantEntry.key();
                std::string fullBlockId = baseBlockId;

                if (!variantKey.empty()) {
                    std::string stateCondition;
                    std::istringstream iss(variantKey);
                    std::string statePair;

                    while (std::getline(iss, statePair, ',')) {
                        size_t eqPos = statePair.find('=');
                        if (eqPos != std::string::npos) {
                            std::string state = statePair.substr(0, eqPos);
                            std::string value = statePair.substr(eqPos + 1);

                            if (!value.empty() && value.front() == '\'' && value.back() == '\'') {
                                value = value.substr(1, value.size() - 2);
                            }

                            if (!stateCondition.empty()) {
                                stateCondition += ",";
                            }
                            stateCondition += state + "=" + value;
                        }
                    }

                    if (!stateCondition.empty()) {
                        fullBlockId += "[" + stateCondition + "]";
                    }
                }

                variantBlockIds.push_back(fullBlockId);
            }

            if (!variantBlockIds.empty()) {
                const auto& modelCache = ProcessBlockstateJson(namespaceName, variantBlockIds);

                for (const auto& entry : modelCache) {
                    modelCount++;

                    const int row = modelCount / matrixSize;
                    const int col = modelCount % matrixSize;
                    const float xOffset = col * spacing;
                    const float zOffset = row * spacing;

                    ModelData mutableModel = entry.second;

                    for (size_t i = 0; i < mutableModel.vertices.size(); i += 3) {
                        mutableModel.vertices[i] += xOffset;
                        mutableModel.vertices[i + 2] += zOffset;
                    }

                    MergeModelsDirectly(mergedModel, mutableModel);
                }
            }
        }
    }

    if (!mergedModel.vertices.empty()) {
        CreateModelFiles(mergedModel, "test");
    }
}