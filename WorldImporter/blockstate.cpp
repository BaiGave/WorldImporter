#include "blockstate.h"
#include "fileutils.h"
#include <regex>
#include <random>
#include <numeric>
#include <Windows.h>   
#include <iostream>


bool matchConditions(const std::unordered_map<std::string, std::string>& blockConditions, const nlohmann::json& when) {
    // 判断 when 条件是否满足
    if (when.is_object()) {
        // 如果有 "OR" 条件，处理 OR 条件
        if (when.contains("OR")) {
            // 遍历 OR 中的每个条件，只要有一个条件满足就返回 true
            for (const auto& condition : when["OR"]) {
                if (matchConditions(blockConditions, condition)) {
                    return true;  // 如果任一条件满足，就返回 true
                }
            }
            return false;  // 如果所有 OR 条件都不满足，返回 false
        }

        // 如果有 "AND" 条件，处理 AND 条件
        if (when.contains("AND")) {
            // 遍历 AND 中的每个条件，所有条件都必须满足
            for (const auto& condition : when["AND"]) {
                if (!matchConditions(blockConditions, condition)) {
                    return false;  // 如果有一个 AND 条件不满足，返回 false
                }
            }
            return true;  // 如果所有 AND 条件都满足，返回 true
        }

        // 遍历 when 中的每个属性条件
        for (auto& condition : when.items()) {
            std::string key = condition.key();
            std::string value = condition.value();

            if (blockConditions.find(key) != blockConditions.end()) {
                // 如果值包含 "|"，表示“或”条件
                if (value.find('|') != std::string::npos) {
                    // 拆分为多个选项
                    std::vector<std::string> options;
                    size_t pos = 0;
                    while ((pos = value.find('|')) != std::string::npos) {
                        options.push_back(value.substr(0, pos));
                        value.erase(0, pos + 1);
                    }
                    options.push_back(value);  // 加入最后一个选项

                    // 如果 blockConditions[key] 的值在 options 中，则条件匹配
                    if (std::find(options.begin(), options.end(), blockConditions.at(key)) != options.end()) {
                        continue;  // 条件满足，继续处理其他条件
                    }
                    else {
                        return false; // 如果不匹配，则条件不成立
                    }
                }
                // 否则，必须严格匹配
                else if (blockConditions.at(key) != value) {
                    return false;
                }
            }
            else {
                // 如果条件不在 blockConditions 中，则跳过
                return false;
            }
        }
    }
    return true;
}


// 获取目标路径对应的 .json 文件的内容
nlohmann::json GetBlockstateJson(const std::string& namespaceName, const std::string& blockId) {
    // 获取当前整合包的版本
    std::string currentVersion = currentSelectedGameVersion;

    // 在 modListCache 中查找对应的模组列表
    if (modListCache.find(currentVersion) == modListCache.end()) {
        std::cerr << "Mod list for version " << currentVersion << " not found!" << std::endl;
        return nlohmann::json();  // 返回空的 JSON
    }

    // 获取该版本的游戏文件夹路径
    std::string gameFolderPath = config.versionConfigs[currentVersion].gameFolderPath;

    // 遍历 modListCache 中该版本的模组列表，查找对应的 .json 文件
    for (const auto& folderData : modListCache[currentVersion]) {
        // 获取当前模组的命名空间
        std::string modNamespace = folderData.namespaceName;

        // 如果是 Vanilla (minecraft)
        if (modNamespace == "vanilla") {
            // 从 VersionCache 中获取 minecraft 的路径
            if (VersionCache.find(currentVersion) != VersionCache.end()) {
                for (const auto& folderData : VersionCache[currentVersion]) {
                    // 获取 minecraft 文件夹路径
                    std::wstring minecraftPath = string_to_wstring(folderData.path);
                    JarReader minecraftReader(minecraftPath);

                    // 构造 blockstate 文件的路径
                    std::string blockstateFilePath = "assets/" + namespaceName + "/blockstates/" + blockId + ".json";
                    std::string fileContent = minecraftReader.getFileContent(blockstateFilePath);

                    // 如果找到了文件，解析 JSON 内容并返回
                    if (!fileContent.empty()) {
                        try {
                            nlohmann::json blockstateJson = nlohmann::json::parse(fileContent);
                            return blockstateJson;
                        }
                        catch (const std::exception& e) {
                            std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                            return nlohmann::json();  // 返回空的 JSON
                        }
                    }
                }
            }
            else {
                std::cerr << "Minecraft version not found in VersionCache!" << std::endl;
                return nlohmann::json();  // 返回空的 JSON
            }
        }
        // 如果是 ResourcePack
        else if (modNamespace == "resourcePack") {
            // 遍历 resourcePacksCache 查找对应的 resourcePack 文件
            for (const auto& folderData : resourcePacksCache[currentVersion]) {
                std::wstring resourcePackPath = string_to_wstring(folderData.path);
                JarReader resourcePackReader(resourcePackPath);

                // 构造 blockstate 文件的路径
                std::string blockstateFilePath = "assets/" + namespaceName + "/blockstates/" + blockId + ".json";
                std::string fileContent = resourcePackReader.getFileContent(blockstateFilePath);

                // 如果找到了文件，解析 JSON 内容并返回
                if (!fileContent.empty()) {
                    try {
                        nlohmann::json blockstateJson = nlohmann::json::parse(fileContent);
                        return blockstateJson;
                    }
                    catch (const std::exception& e) {
                        std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                        return nlohmann::json();  // 返回空的 JSON
                    }
                }
            }
        }
        // 如果是普通 Mod 模组，继续按原逻辑处理
        else {
            // 构造 blockstate 文件的路径
            std::string blockstateFilePath = "assets/" + namespaceName + "/blockstates/" + blockId + ".json";
            std::wstring jarFilePath = string_to_wstring(folderData.path);
            JarReader jarReader(jarFilePath);

            // 如果找到了文件，解析 JSON 内容并返回
            std::string fileContent = jarReader.getFileContent(blockstateFilePath);
            if (!fileContent.empty()) {
                try {
                    nlohmann::json blockstateJson = nlohmann::json::parse(fileContent);
                    return blockstateJson;
                }
                catch (const std::exception& e) {
                    std::cerr << "Error parsing JSON: " << e.what() << std::endl;
                    return nlohmann::json();  // 返回空的 JSON
                }
            }
        }
    }

    // 如果没有找到文件，返回空 JSON
    std::cerr << "blockstate file not found for blockId: " << blockId << std::endl;
    return nlohmann::json();  // 返回空的 JSON
}


nlohmann::json ProcessBlockstateJson(const std::string& namespaceName, const std::string& blockId) {
    // 使用正则表达式分离 blockId 和方括号中的条件
    std::regex blockIdRegex("^(.*?)\\[(.*)\\]$"); // 匹配 blockId 和方括号中的条件部分
    std::smatch match;

    std::string baseBlockId = blockId;  // 默认 baseBlockId 为原始的 blockId
    std::string condition = "";         // 默认条件为空

    // 如果 blockId 包含方括号，则提取基础部分和条件部分
    std::unordered_map<std::string, std::string> blockConditions;  // 存储 blockId 中的条件
    if (std::regex_match(blockId, match, blockIdRegex)) {
        baseBlockId = match.str(1);  // 提取方括号前的部分作为基础 blockId
        condition = match.str(2);    // 提取方括号中的内容作为条件

        // 解析 blockId 中的条件并存储为键值对
        std::regex conditionRegex("(\\w+)=(true|false|side\\|up|none|side|up|\\d+)");  // 更新条件格式以支持 side|up
        std::sregex_iterator iter(condition.begin(), condition.end(), conditionRegex);
        std::sregex_iterator end;
        while (iter != end) {
            blockConditions[(*iter)[1].str()] = (*iter)[2].str();
            ++iter;
        }
    }

    // 先通过 GetBlockstateJson 读取文件，使用基础的 blockId
    nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);

    // 如果读取失败，返回空的 JSON
    if (blockstateJson.is_null()) {
        return nlohmann::json();
    }

    // 处理读取到的 JSON 数据，根据实际需求进行处理
    if (blockstateJson.contains("variants")) {
        // 获取 "variants" 部分
        auto variants = blockstateJson["variants"];

        // 遍历每个变种项
        for (auto& variant : variants.items()) {
            std::string variantKey = variant.key();
            // 判断变种名是否符合条件
            if (condition.empty() || variantKey == condition) {
                // 如果变种值是数组类型，处理加权选择
                if (variant.value().is_array()) {
                    // 获取所有模型及其权重
                    std::vector<std::pair<nlohmann::json, int>> modelsWithWeights;
                    int totalWeight = 0;

                    for (const auto& item : variant.value()) {
                        // 获取每个模型的 weight 属性，如果没有指定，则默认值为 1
                        int weight = item.contains("weight") ? item["weight"].get<int>() : 1;
                        modelsWithWeights.push_back({ item, weight });
                        totalWeight += weight;
                    }

                    // 随机选择一个模型，根据权重
                    if (totalWeight > 0) {
                        // 创建一个随机数生成器
                        std::random_device rd;
                        std::mt19937 gen(rd());
                        std::uniform_int_distribution<> dis(1, totalWeight);

                        // 随机选择一个权重
                        int randomWeight = dis(gen);
                        int cumulativeWeight = 0;
                        for (const auto& model : modelsWithWeights) {
                            cumulativeWeight += model.second;
                            if (randomWeight <= cumulativeWeight) {
                                // 获取 modelId
                                std::string modelId = model.first.contains("model") ? model.first["model"].get<std::string>() : "";

                                if (!modelId.empty()) {
                                    // 判断 modelId 是否包含冒号，如果包含冒号，使用冒号前的部分作为新的 namespaceName
                                    size_t colonPos = modelId.find(':');
                                    std::string modelNamespace = namespaceName;  // 默认为原 namespaceName

                                    if (colonPos != std::string::npos) {
                                        modelNamespace = modelId.substr(0, colonPos);  // 提取冒号前的部分
                                        modelId = modelId.substr(colonPos + 1);      // 提取冒号后的部分
                                    }

                                    // 使用 ProcessModelJson 获取模型数据
                                    nlohmann::json selectedModelJson = ProcessModelJson(modelNamespace, modelId);

                                    // 输出最终选择的模型数据
                                    std::cout << "Processed model JSON: " << selectedModelJson << std::endl;

                                    // 在此可以进行其他处理，比如返回最终的 JSON 或做进一步的合并
                                }

                                break;
                            }
                        }
                    }
                }
                else {
                    // 处理模型ID
                    std::string modelId = variant.value().contains("model") ? variant.value()["model"].get<std::string>() : "";

                    if (!modelId.empty()) {
                        // 判断 modelId 是否包含冒号，如果包含冒号，使用冒号前的部分作为新的 namespaceName
                        size_t colonPos = modelId.find(':');
                        std::string modelNamespace = namespaceName;  // 默认为原 namespaceName

                        if (colonPos != std::string::npos) {
                            modelNamespace = modelId.substr(0, colonPos);  // 提取冒号前的部分
                            modelId = modelId.substr(colonPos + 1);      // 提取冒号后的部分
                        }

                        // 使用 ProcessModelJson 获取模型数据
                        nlohmann::json selectedModelJson = ProcessModelJson(modelNamespace, modelId);

                        // 输出最终选择的模型数据
                        std::cout << "Processed model JSON: " << selectedModelJson << std::endl;

                        // 在此可以进行其他处理，比如返回最终的 JSON 或做进一步的合并
                    }
                }
            }
        }
    }

    // 处理 multipart 部分
    if (blockstateJson.contains("multipart")) {
        // 获取 "multipart" 部分
        auto multipart = blockstateJson["multipart"];

        // 遍历每个 multipart 条目
        for (const auto& item : multipart) {
            // 每个 multipart 条目包含条件和模型
            if (item.contains("when") && item.contains("apply")) {
                auto when = item["when"];
                auto apply = item["apply"];

                // 判断条件是否符合
                bool conditionMatched = matchConditions(blockConditions, when);

                // 如果条件匹配，则输出 apply 部分的模型
                if (conditionMatched) {

                    // 处理模型ID
                    if (apply.is_array()) {
                        // 如果 apply 部分是一个数组，处理加权选择
                        int totalWeight = 0;
                        std::vector<std::pair<nlohmann::json, int>> modelsWithWeights;

                        for (const auto& modelItem : apply) {
                            int weight = modelItem.contains("weight") ? modelItem["weight"].get<int>() : 1;
                            modelsWithWeights.push_back({ modelItem, weight });
                            totalWeight += weight;
                        }

                        // 随机选择一个模型
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
                                        nlohmann::json selectedModelJson = ProcessModelJson(modelNamespace, modelId);
                                        std::cout << "Processed model JSON: " << selectedModelJson << std::endl;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    else {
                        // 如果 apply 部分不是数组，直接处理单个模型
                        std::string modelId = apply.contains("model") ? apply["model"].get<std::string>() : "";
                        if (!modelId.empty()) {
                            size_t colonPos = modelId.find(':');
                            std::string modelNamespace = namespaceName;
                            if (colonPos != std::string::npos) {
                                modelNamespace = modelId.substr(0, colonPos);
                                modelId = modelId.substr(colonPos + 1);
                            }
                            nlohmann::json selectedModelJson = ProcessModelJson(modelNamespace, modelId);
                            std::cout << "Processed model JSON: " << selectedModelJson << std::endl;
                        }
                    }
                }
            }
        }
    }

    return blockstateJson;
}

