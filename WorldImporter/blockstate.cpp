#include "blockstate.h"
#include "fileutils.h"
#include <regex>
#include <random>
#include <numeric>
#include <Windows.h>   
#include <iostream>
#include <sstream>
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
nlohmann::json GetBlockstateJson(const std::string& namespaceName,
    const std::string& blockId) {

    // 构造缓存键
    std::string cacheKey = namespaceName + ":" + blockId;

    // 加锁保护缓存访问
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    auto it = GlobalCache::blockstates.find(cacheKey);
    if (it != GlobalCache::blockstates.end()) {
        return it->second;
    }

    // 未找到时的处理（可选）
    std::cerr << "Blockstate not found: " << cacheKey << std::endl;
    return nlohmann::json();
}

std::unordered_map<std::string, ModelData> ProcessBlockstateJson(
    const std::string& namespaceName,
    const std::vector<std::string>& blockIds
) {
    std::unordered_map<std::string, ModelData> result;
    // 确保 namespace 条目存在
    auto& namespaceCache = BlockModelCache[namespaceName];

    for (const auto& blockId : blockIds) {
        auto cacheIt = namespaceCache.find(blockId);
        if (cacheIt != namespaceCache.end()) {
            result[blockId] = cacheIt->second;
            continue;
        }
        // 如果已缓存，跳过处理
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
            auto conditions_begin = std::sregex_iterator(condition.begin(), condition.end(), conditionRegex);
            auto conditions_end = std::sregex_iterator();

            for (auto i = conditions_begin; i != conditions_end; ++i) {
                std::smatch submatch = *i;
                blockConditions[submatch[1].str()] = submatch[2].str();
            }
        }

        // 读取 blockstate JSON
        nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);
        if (blockstateJson.is_null()) {
            continue; // 文件不存在或解析失败，跳过
        }

        // 处理 Variants 和 Multipart
        ModelData mergedModel;
        std::vector<ModelData> selectedModels;

        // 处理读取到的 JSON 数据，根据实际需求进行处理
        if (blockstateJson.contains("variants")) {

            // 获取 "variants" 部分
            auto variants = blockstateJson["variants"];

            // 遍历每个变种项
            for (auto& variant : variants.items()) {
                std::string variantKey = variant.key();
                // 判断变种名是否符合条件
                if (condition.empty() || variantKey == condition) {
                    // 提取旋转参数
                    int rotationX = 0;
                    int rotationY = 0;
                    if (variant.value().contains("x")) {
                        rotationX = variant.value()["x"].get<int>();
                    }
                    if (variant.value().contains("y")) {
                        rotationY = variant.value()["y"].get<int>();
                    }
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


                                        ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY);
                                        selectedModels.push_back(selectedModel); // 收集选中的模型
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

                            ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY);
                            selectedModels.push_back(selectedModel); // 收集选中的模型

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
                // 每个 multipart 条目必须包含 apply，但 when 是可选的
                if (item.contains("apply")) {
                    auto apply = item["apply"];
                    bool conditionMatched = true;  // 默认没有 when 时条件为 true
                    // 如果有 when 条件，则进行判断
                    if (item.contains("when")) {
                        auto when = item["when"];
                        conditionMatched = matchConditions(blockConditions, when);
                    }
               
                
                    // 如果条件匹配，则输出 apply 部分的模型
                    if (conditionMatched) {
                        // 提取旋转参数
                        int rotationX = 0;
                        int rotationY = 0;
                        if (apply.contains("x")) {
                            rotationX = apply["x"].get<int>();
                        }
                        if (apply.contains("y")) {
                            rotationY = apply["y"].get<int>();
                        }

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
                                            ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY);
                                            selectedModels.push_back(selectedModel); // 收集选中的模型
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
                                ModelData selectedModel = ProcessModelJson(modelNamespace, modelId, rotationX, rotationY);

                            
                                selectedModels.push_back(selectedModel); // 收集选中的模型
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

            // 生成文件（仅在首次处理时）
            std::string fileName = blockId;
            size_t slashPos = fileName.find_last_of("/\\");
            if (slashPos != std::string::npos) {
                fileName = fileName.substr(slashPos + 1);
            }
            mergedModel.objName = fileName;

        }

        // 存入缓存
        namespaceCache[blockId] = mergedModel;
        result[blockId] = mergedModel;
        }

    // 返回全局缓存
    return result;
}
// 新增辅助函数：计算排列矩阵维度
int CalculateMatrixSize(int variantCount) {
    return static_cast<int>(std::ceil(std::sqrt(variantCount)));
}


void ProcessAllBlockstateVariants() {
    // 获取全局缓存中的blockstates
    std::unordered_map<std::string, nlohmann::json> allBlockstates;
    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        allBlockstates = GlobalCache::blockstates;
    }

    // 创建全局合并模型容器
    ModelData mergedModel;
    std::string mergedBaseName = "combined_master_model";
    int totalModelCount = 0;
    int ModelCount = 0;
    for (const auto& blockstatePair : allBlockstates) {
        const nlohmann::json& blockstateJson = blockstatePair.second;
        if (blockstateJson.contains("multipart")) continue;

        // 处理variants
        if (blockstateJson.contains("variants")) {
            const auto& variants = blockstateJson["variants"];
            totalModelCount+=variants.size();
        }

        
    }
    // 计算全局网格位置
    const int n = CalculateMatrixSize(totalModelCount);
    const float spacing = 2.0f;

   
    // 第一阶段：流式处理并合并所有模型
    for (const auto& blockstatePair : allBlockstates) {
        const std::string& cacheKey = blockstatePair.first;
        const nlohmann::json& blockstateJson = blockstatePair.second;

        // 分割命名空间和基础方块ID
        size_t colonPos = cacheKey.find(':');
        if (colonPos == std::string::npos) continue;

        std::string namespaceName = cacheKey.substr(0, colonPos);
        std::string baseBlockId = cacheKey.substr(colonPos + 1);

        if (blockstateJson.contains("multipart")) continue;

        // 处理variants
        if (blockstateJson.contains("variants")) {
            std::vector<std::string> variantBlockIds;
            const auto& variants = blockstateJson["variants"];

            // 收集所有变种ID
            for (const auto& variantEntry : variants.items()) {
                std::string variantKey = variantEntry.key();
                std::string fullBlockId = baseBlockId;
                // 构造带状态的方块ID（过滤空variant）
                if (!variantKey.empty()) {
                    // 转换variant语法：variantKey -> state条件
                    std::string stateCondition;
                    std::istringstream iss(variantKey);
                    std::string statePair;

                    // 分割多个状态条件（用逗号分隔）
                    while (std::getline(iss, statePair, ',')) {
                        size_t eqPos = statePair.find('=');
                        if (eqPos != std::string::npos) {
                            std::string state = statePair.substr(0, eqPos);
                            std::string value = statePair.substr(eqPos + 1);

                            // 标准化值（去除引号）
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

                // 构造完整带命名空间的blockId
                std::string namespacedBlockId = fullBlockId;
                variantBlockIds.push_back(namespacedBlockId);
            }

            if (!variantBlockIds.empty()) {
                // 获取变种模型并合并到主模型
                const auto& modelCache = ProcessBlockstateJson(namespaceName, variantBlockIds);

                
                // 记录开始时间
                //auto start = std::chrono::high_resolution_clock::now();
                // 修改后的循环结构
                // 正确代码（根据返回类型调整）
                for (const auto& entry : modelCache) {  // 先获取整个条目
                    const std::string& blockId = entry.first;
                    const ModelData& currentModel = entry.second;

                    
                    ModelCount++;
                    // 计算偏移量
                    const int row = ModelCount / n;
                    const int col = ModelCount % n;
                    const float xOffset = col * spacing;
                    const float zOffset = row * spacing;
                    ModelData mutableModel = entry.second; // 创建副本

                    // 修改后的顶点偏移应用（适配连续存储结构）
                    for (size_t i = 0; i < mutableModel.vertices.size(); i += 3) {
                        // 顶点坐标布局：[x0,y0,z0, x1,y1,z1,...]
                        mutableModel.vertices[i] += xOffset;   // X坐标
                        mutableModel.vertices[i + 2] += zOffset; // Z坐标
                    }

                    
                    
                    MergeModelsDirectly(mergedModel, mutableModel);
                    
                }

                // 记录结束时间
                //auto end = std::chrono::high_resolution_clock::now();
                //std::chrono::duration<double, std::milli> duration = end - start;  // 使用毫秒为单位

                // 输出消耗时间
                //std::cout << "Time taken for processing models: " << duration.count() << " milliseconds." << std::endl;

            }
        }

        
    }

    // 第二阶段：导出最终合并文件
    if (!mergedModel.vertices.empty()) {
        CreateModelFiles(mergedModel, "test");
    }
}






// 输出缓存内容的方法
//void PrintModelCache(const std::unordered_map<std::string, std::unordered_map<std::string, ModelData>>& cache) {
//    for (const auto& namespaceEntry : cache) {
//        const std::string& namespaceName = namespaceEntry.first;
//        const auto& blockIdMap = namespaceEntry.second;
//
//        std::cout << "Namespace: " << namespaceName << "\n";
//        for (const auto& blockEntry : blockIdMap) {
//            const std::string& blockId = blockEntry.first;
//            const ModelData& modelData = blockEntry.second;
//
//            std::cout << "  Block ID: " << blockId << "\n";
//
//            // 输出顶点数据
//            std::cout << "    Vertices:\n";
//            for (const auto& vertex : modelData.vertices) {
//                std::cout << "      [";
//                for (float coord : vertex) {
//                    std::cout << coord << " ";
//                }
//                std::cout << "]\n";
//            }
//
//            // 输出UV坐标数据
//            std::cout << "    UV Coordinates:\n";
//            for (const auto& uv : modelData.uvCoordinates) {
//                std::cout << "      [";
//                for (float coord : uv) {
//                    std::cout << coord << " ";
//                }
//                std::cout << "]\n";
//            }
//
//            // 输出面数据
//            std::cout << "    Faces to Vertices:\n";
//            for (const auto& faceEntry : modelData.facesToVertices) {
//                int faceId = faceEntry.first;
//                const std::vector<int>& vertices = faceEntry.second;
//                std::cout << "      Face " << faceId << ": [";
//                for (int vertexId : vertices) {
//                    std::cout << vertexId << " ";
//                }
//                std::cout << "]\n";
//            }
//
//            // 输出面所对应的uv数据
//            std::cout << "    Faces to UVs:\n";
//            for (const auto& faceEntry : modelData.uvToFaceId) {
//                int faceId = faceEntry.first;
//                const std::vector<int>& uvs = faceEntry.second;
//                std::cout << "      Face " << faceId << ": [";
//                for (int uvId : uvs) {
//                    std::cout << uvId << " ";
//                }
//                std::cout << "]\n";
//            }
//
//            // 输出材质数据
//            std::cout << "    Material to Face IDs:\n";
//            for (const auto& materialEntry : modelData.materialToFaceIds) {
//                const std::string& materialName = materialEntry.first;
//                const std::vector<int>& faceIds = materialEntry.second;
//                std::cout << "      Material " << materialName << ": [";
//                for (int faceId : faceIds) {
//                    std::cout << faceId << " ";
//                }
//                std::cout << "]\n";
//            }
//
//
//            // 输出纹理路径
//            std::cout << "    Texture to Path:\n";
//            for (const auto& textureEntry : modelData.textureToPath) {
//                const std::string& textureName = textureEntry.first;
//                const std::string& texturePath = textureEntry.second;
//                std::cout << "      Texture " << textureName << ": " << texturePath << "\n";
//            }
//
//            std::cout << "\n";
//        }
//    }
//}