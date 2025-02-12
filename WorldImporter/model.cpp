#include "model.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <chrono>

//———————————辅助方法———————————————

// 获取可执行文件所在的目录路径
std::string getExecutableDir() {
    // 获取可执行文件路径
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    // 提取目录路径
    std::string exeDir(exePath);
    size_t lastSlash = exeDir.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        exeDir = exeDir.substr(0, lastSlash + 1);  // 包括最后的斜杠
    }
    return exeDir;
}

// 修改后的旋转函数（适配新数据结构）
void ApplyRotationToVertices(std::vector<float>& vertices, int rotationX, int rotationY) {
    // 参数校验
    if (vertices.size() % 3 != 0) {
        throw std::invalid_argument("Invalid vertex data size");
    }

    // 绕 Y 轴旋转（90度增量）
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        switch (rotationY) {
        case 90:
            std::tie(x, z) = std::make_pair(1.0f - z, x);
            break;
        case 180:
            x = 1.0f - x;
            z = 1.0f - z;
            break;
        case 270:
            std::tie(x, z) = std::make_pair(z, 1.0f - x);
            break;
        default:
            break;
        }
    }

    // 绕 X 轴旋转（90度增量）
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        switch (rotationX) {
        case 90:
            std::tie(y, z) = std::make_pair(1.0f - z, y);
            break;
        case 180:
            y = 1.0f - y;
            z = 1.0f - z;
            break;
        case 270:
            std::tie(y, z) = std::make_pair(z, 1.0f - y);
            break;
        default:
            break;
        }
    }
}

//———————————获取JSON原始数据的方法———————————————
nlohmann::json LoadParentModel(const std::string& namespaceName, const std::string& blockId, nlohmann::json& currentModelJson) {
    // 如果当前模型没有 parent 属性，直接返回
    if (!currentModelJson.contains("parent")) {
        return currentModelJson;
    }

    // 获取当前模型的 parent
    std::string parentModelId = currentModelJson["parent"];

    // 判断 parentModelId 是否包含冒号（即是否包含命名空间）
    size_t colonPos = parentModelId.find(':');
    std::string parentNamespace = "minecraft";  // 默认使用minecraft作为当前的 namespaceName

    if (colonPos != std::string::npos) {
        parentNamespace = parentModelId.substr(0, colonPos);  // 提取冒号前的部分作为父模型的命名空间
        parentModelId = parentModelId.substr(colonPos + 1);  // 提取冒号后的部分作为父模型的 ID
    }

    // 生成唯一缓存键
    std::string cacheKey = parentNamespace + ":" + parentModelId;

    // 检查缓存是否存在
    {
        std::lock_guard<std::recursive_mutex> lock(parentModelCacheMutex);
        auto cacheIt = parentModelCache.find(cacheKey);
        if (cacheIt != parentModelCache.end()) {
            // 从缓存中获取父模型数据
            nlohmann::json parentModelJson = cacheIt->second;

            // 合并父模型的属性到当前模型中
            currentModelJson = MergeModelJson(parentModelJson, currentModelJson);

            // 如果父模型没有 parent 属性，停止递归
            if (!parentModelJson.contains("parent")) {
                return currentModelJson;
            }

            // 递归加载父模型的父模型
            return LoadParentModel(parentNamespace, parentModelId, currentModelJson);
        }
    }

    // 缓存未命中，加载父模型
    nlohmann::json parentModelJson = GetModelJson(parentNamespace, parentModelId);

    // 如果父模型不存在，直接返回当前模型
    if (parentModelJson.is_null()) {
        return currentModelJson;
    }

    // 将父模型数据存入缓存
    {
        std::lock_guard<std::recursive_mutex> lock(parentModelCacheMutex); // 加锁
        parentModelCache[cacheKey] = parentModelJson;
    }

    // 合并父模型的属性到当前模型中
    currentModelJson = MergeModelJson(parentModelJson, currentModelJson);

    // 如果父模型没有 parent 属性，停止递归
    if (!parentModelJson.contains("parent")) {
        return currentModelJson;
    }

    // 递归加载父模型的父模型
    return LoadParentModel(parentNamespace, parentModelId, currentModelJson);
}

nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson, const nlohmann::json& currentModelJson) {
    nlohmann::json mergedModelJson = currentModelJson;
    std::map<std::string, std::string> textureMap;

    // 保存子级的 textures
    if (currentModelJson.contains("textures")) {
        for (const auto& item : currentModelJson["textures"].items()) {
            textureMap[item.key()] = item.value().get<std::string>();
        }
    }

    // 父模型的 parent 属性覆盖子模型的 parent 属性
    if (parentModelJson.contains("parent")) {
        mergedModelJson["parent"] = parentModelJson["parent"];
    }

    // 合并 "textures"
    if (parentModelJson.contains("textures")) {
        if (!mergedModelJson.contains("textures")) {
            mergedModelJson["textures"] = nlohmann::json::object();
        }
        for (const auto& item : parentModelJson["textures"].items()) {
            const std::string& key = item.key();
            // 仅当子级不存在该键时处理父级的键
            if (!mergedModelJson["textures"].contains(key)) {
                std::string textureValue = item.value().get<std::string>();
                // 处理变量引用（如 #texture）
                if (!textureValue.empty() && textureValue[0] == '#') {
                    std::string varName = textureValue.substr(1);
                    if (textureMap.find(varName) != textureMap.end()) {
                        textureValue = textureMap[varName];
                    }
                }
                mergedModelJson["textures"][key] = textureValue;
            }
        }
    }

    // 合并 "elements"
    if (parentModelJson.contains("elements") && !currentModelJson.contains("elements")) {
        mergedModelJson["elements"] = parentModelJson["elements"];
    }

    // 合并 "display"
    if (parentModelJson.contains("display") && !currentModelJson.contains("display")) {
        mergedModelJson["display"] = parentModelJson["display"];
    }

    // 合并其他需要继承的属性
    if (parentModelJson.contains("ambientocclusion") && !currentModelJson.contains("ambientocclusion")) {
        mergedModelJson["ambientocclusion"] = parentModelJson["ambientocclusion"];
    }

    return mergedModelJson;
}

nlohmann::json GetModelJson(const std::string& namespaceName,
    const std::string& modelPath) {

    // 构造缓存键
    std::string cacheKey = namespaceName + ":" + modelPath;

    // 加锁保护缓存访问
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    auto it = GlobalCache::models.find(cacheKey);
    if (it != GlobalCache::models.end()) {
        return it->second;
    }

    // 未找到时的处理（可选）
    std::cerr << "Model not found: " << cacheKey << std::endl;
    return nlohmann::json();
}



//———————————将JSON数据转为结构体的方法———————————————

void processTextures(const nlohmann::json& modelJson, ModelData& data,
    std::unordered_map<std::string, int>& textureKeyToMaterialIndex) {

    std::unordered_map<std::string, int> processedMaterials; // 材质名称到索引的映射

    if (modelJson.contains("textures")) {
        auto textures = modelJson["textures"];
        for (auto& texture : textures.items()) {
            std::string textureKey = texture.key();
            std::string textureValue = texture.value();

            // 解析命名空间和路径
            size_t colonPos = textureValue.find(':');
            std::string namespaceName = "minecraft";
            std::string pathPart = textureValue;
            if (colonPos != std::string::npos) {
                namespaceName = textureValue.substr(0, colonPos);
                pathPart = textureValue.substr(colonPos + 1);
            }

            // 生成唯一材质标识
            std::string fullMaterialName = namespaceName + ":" + pathPart;

            // 检查是否已处理过该材质
            if (processedMaterials.find(fullMaterialName) == processedMaterials.end()) {
                // 生成缓存键
                std::string cacheKey = namespaceName + ":" + pathPart;

                // 保存纹理并获取路径
                std::string textureSavePath;
                {
                    std::lock_guard<std::mutex> lock(texturePathCacheMutex);
                    auto cacheIt = texturePathCache.find(cacheKey);
                    if (cacheIt != texturePathCache.end()) {
                        textureSavePath = cacheIt->second;
                    }
                    else {
                        std::string saveDir = "textures";
                        SaveTextureToFile(namespaceName, pathPart, saveDir);
                        textureSavePath = "textures/" + pathPart.substr(pathPart.find_last_of('/') + 1) + ".png";
                        texturePathCache[cacheKey] = textureSavePath;
                    }
                }

                // 记录材质信息
                int materialIndex = data.materialNames.size();
                data.materialNames.push_back(fullMaterialName);
                data.texturePaths.push_back(textureSavePath);
                processedMaterials[fullMaterialName] = materialIndex;
            }

            // 记录材质键到索引的映射
            textureKeyToMaterialIndex[textureKey] = processedMaterials[fullMaterialName];
        }
    }
}


void processElements(const nlohmann::json& modelJson, ModelData& data,
    const std::unordered_map<std::string, int>& textureKeyToMaterialIndex)
{

    std::unordered_map<std::string, int> vertexCache;
    std::unordered_map<std::string, int> uvCache;
    int faceId = 0;
    std::unordered_map<std::string, int> faceCountMap; // 面计数映射

    if (modelJson.contains("elements")) {
        auto elements = modelJson["elements"];

        for (const auto& element : elements) {
            if (element.contains("from") && element.contains("to") && element.contains("faces")) {
                auto from = element["from"];
                auto to = element["to"];
                auto faces = element["faces"];


                // 转换原始坐标为 OBJ 坐标系（/16）
                float x1 = from[0].get<float>() / 16.0f;
                float y1 = from[1].get<float>() / 16.0f;
                float z1 = from[2].get<float>() / 16.0f;
                float x2 = to[0].get<float>() / 16.0f;
                float y2 = to[1].get<float>() / 16.0f;
                float z2 = to[2].get<float>() / 16.0f;

                // 生成基础顶点数据
                std::unordered_map<std::string, std::vector<std::vector<float>>> elementVertices = {
                    {"down",  {{x2, y1, z2}, {x2, y1, z1}, {x1, y1, z1}, {x1, y1, z2}}},
                    {"up",    {{x1, y2, z1}, {x1, y2, z2}, {x2, y2, z2}, {x2, y2, z1}}},
                    {"west",  {{x1, y1, z2}, {x1, y2, z2}, {x1, y2, z1}, {x1, y1, z1}}},
                    {"east",  {{x2, y1, z1}, {x2, y2, z1}, {x2, y2, z2}, {x2, y1, z2}}},
                    {"north", {{x1, y1, z1}, {x1, y2, z1}, {x2, y2, z1}, {x2, y1, z1}}},
                    {"south", {{x2, y1, z2}, {x2, y2, z2}, {x1, y2, z2}, {x1, y1, z2}}}
                };

                // 处理元素旋转
                if (element.contains("rotation")) {
                    auto rotation = element["rotation"];
                    std::string axis = rotation["axis"].get<std::string>();
                    float angle_deg = rotation["angle"].get<float>();
                    auto origin = rotation["origin"];

                    // 转换旋转中心到 OBJ 坐标系
                    float ox = origin[0].get<float>() / 16.0f;
                    float oy = origin[1].get<float>() / 16.0f;
                    float oz = origin[2].get<float>() / 16.0f;
                    float angle_rad = angle_deg * (M_PI / 180.0f); // 转换为弧度

                    // 对每个面的顶点应用旋转
                    for (auto& faceEntry : elementVertices) {
                        auto& vertices = faceEntry.second;
                        for (auto& vertex : vertices) {
                            float& vx = vertex[0];
                            float& vy = vertex[1];
                            float& vz = vertex[2];

                            // 平移至旋转中心相对坐标
                            float tx = vx - ox;
                            float ty = vy - oy;
                            float tz = vz - oz;

                            // 根据轴类型进行旋转
                            if (axis == "x") {
                                // 绕X轴旋转
                                float new_y = ty * cos(angle_rad) - tz * sin(angle_rad);
                                float new_z = ty * sin(angle_rad) + tz * cos(angle_rad);
                                ty = new_y;
                                tz = new_z;
                            }
                            else if (axis == "y") {
                                // 绕Y轴旋转
                                float new_x = tx * cos(angle_rad) + tz * sin(angle_rad);
                                float new_z = -tx * sin(angle_rad) + tz * cos(angle_rad);
                                tx = new_x;
                                tz = new_z;
                            }
                            else if (axis == "z") {
                                // 绕Z轴旋转
                                float new_x = tx * cos(angle_rad) - ty * sin(angle_rad);
                                float new_y = tx * sin(angle_rad) + ty * cos(angle_rad);
                                tx = new_x;
                                ty = new_y;
                            }

                            // 平移回原坐标系
                            vx = tx + ox;
                            vy = ty + oy;
                            vz = tz + oz;
                        }
                    }
                }

                // 遍历每个面的数据，判断面是否存在，如果存在则处理
                for (auto& face : faces.items()) {
                    std::string faceName = face.key();
                    if (elementVertices.find(faceName) != elementVertices.end()) {
                        auto faceVertices = elementVertices[faceName];

                        // ======== 面重叠处理逻辑 ========
                        if (faceVertices.size() >= 3) {
                            // 计算法线方向
                            const auto& v0 = faceVertices[0];
                            const auto& v1 = faceVertices[1];
                            const auto& v2 = faceVertices[2];

                            // 计算向量差
                            float vec1x = v1[0] - v0[0];
                            float vec1y = v1[1] - v0[1];
                            float vec1z = v1[2] - v0[2];
                            float vec2x = v2[0] - v0[0];
                            float vec2y = v2[1] - v0[1];
                            float vec2z = v2[2] - v0[2];

                            // 计算法线向量
                            float crossX = vec1y * vec2z - vec1z * vec2y;
                            float crossY = vec1z * vec2x - vec1x * vec2z;
                            float crossZ = vec1x * vec2y - vec1y * vec2x;

                            // 归一化处理
                            float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
                            if (length > 0) {
                                crossX /= length;
                                crossY /= length;
                                crossZ /= length;
                            }

                            // 四舍五入法线向量到小数点后两位
                            crossX = std::round(crossX * 100.0f) / 100.0f;
                            crossY = std::round(crossY * 100.0f) / 100.0f;
                            crossZ = std::round(crossZ * 100.0f) / 100.0f;

                            // 计算面中心点
                            float centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
                            for (const auto& v : faceVertices) {
                                centerX += v[0];
                                centerY += v[1];
                                centerZ += v[2];
                            }
                            centerX /= faceVertices.size();
                            centerY /= faceVertices.size();
                            centerZ /= faceVertices.size();

                            // 四舍五入中心点到小数点后四位
                            centerX = std::round(centerX * 10000.0f) / 10000.0f;
                            centerY = std::round(centerY * 10000.0f) / 10000.0f;
                            centerZ = std::round(centerZ * 10000.0f) / 10000.0f;

                            // 生成唯一标识键
                            std::stringstream keyStream;
                            keyStream << std::fixed << std::setprecision(2)
                                << crossX << "," << crossY << "," << crossZ << "_"
                                << std::setprecision(4)
                                << centerX << "," << centerY << "," << centerZ;
                            std::string key = keyStream.str();

                            // 计算偏移量（第二个面偏移0.01，第三个0.02，依此类推）
                            int count = ++faceCountMap[key];
                            float offset = (count - 1) * 0.001f;

                            // 沿法线方向偏移顶点
                            for (auto& v : faceVertices) {
                                v[0] += crossX * offset;
                                v[1] += crossY * offset;
                                v[2] += crossZ * offset;
                            }

                            // 更新当前面的顶点数据
                            elementVertices[faceName] = faceVertices;
                        }

                        // ======== 顶点处理逻辑 ========
                        std::array<int, 4> vertexIndices;
                        for (int i = 0; i < 4; ++i) {
                            const auto& vertex = faceVertices[i];
                            std::string vertexKey =
                                std::to_string(vertex[0]) + "," +
                                std::to_string(vertex[1]) + "," +
                                std::to_string(vertex[2]);

                            // 检查顶点缓存
                            if (vertexCache.find(vertexKey) == vertexCache.end()) {
                                // 插入新顶点并记录索引
                                vertexCache[vertexKey] = data.vertices.size() / 3; // 新索引计算方式
                                data.vertices.insert(data.vertices.end(),
                                    { vertex[0], vertex[1], vertex[2] });
                            }
                            vertexIndices[i] = vertexCache[vertexKey];
                        }

                        // 插入面数据（四个顶点索引）
                        data.faces.insert(data.faces.end(),
                            { vertexIndices[0], vertexIndices[1],
                             vertexIndices[2], vertexIndices[3] });

                        // 初始化materialIndices（假设faceId是连续递增的）
                        data.materialIndices.resize(faceId + 1, -1); // -1表示未分配材质
                        // 在处理每个面的材质时
                        if (face.value().contains("texture")) {
                            std::string texture = face.value()["texture"];
                            if (texture.front() == '#') texture.erase(0, 1);

                            // 通过映射获取材质索引
                            auto it = textureKeyToMaterialIndex.find(texture);
                            if (it != textureKeyToMaterialIndex.end()) {
                                data.materialIndices[faceId] = it->second;
                            }


                            // 处理 UV 区域，如果没有指定，自动根据当前面的坐标设置
                            std::vector<int> uvRegion = { 0, 0, 16, 16 }; // 默认 UV 区域

                            std::array<int, 4> uvIndices;
                            if (face.value().contains("uv")) {
                                auto uv = face.value()["uv"];
                                uvRegion = { uv[0].get<int>(), uv[1].get<int>(), uv[2].get<int>(), uv[3].get<int>() };
                            }

                            // 获取旋转角度，默认为0
                            int rotation = face.value().value("rotation", 0);


                            // 计算四个 UV 坐标点
                            std::vector<std::vector<float>> uvCoords = {
                                {uvRegion[2] / 16.0f, 1 - uvRegion[3] / 16.0f},   
                                {uvRegion[2] / 16.0f, 1 - uvRegion[1] / 16.0f},
                                {uvRegion[0] / 16.0f, 1 - uvRegion[1] / 16.0f},
                                {uvRegion[0] / 16.0f, 1 - uvRegion[3] / 16.0f}
                                
                            };

                            // 根据旋转角度调整 UV 坐标
                            switch (rotation) {
                            case 90:
                                std::swap(uvCoords[0], uvCoords[3]);
                                std::swap(uvCoords[0], uvCoords[2]);
                                std::swap(uvCoords[0], uvCoords[1]);
                                break;
                            case 180:  
                                std::swap(uvCoords[0], uvCoords[2]);
                                std::swap(uvCoords[1], uvCoords[3]);
                                break;
                            case 270:
                                std::swap(uvCoords[0], uvCoords[3]);
                                std::swap(uvCoords[1], uvCoords[3]);
                                std::swap(uvCoords[2], uvCoords[3]);
                                break;
                            default:
                                break;
                            }

                            // 插入UV数据并记录索引
                            for (int i = 0; i < 4; ++i) {
                                const auto& uv = uvCoords[i];
                                std::string uvKey =
                                    std::to_string(uv[0]) + "," +
                                    std::to_string(uv[1]);

                                if (uvCache.find(uvKey) == uvCache.end()) {
                                    uvCache[uvKey] = data.uvCoordinates.size() / 2; // 新索引计算方式
                                    data.uvCoordinates.insert(data.uvCoordinates.end(),
                                        { uv[0], uv[1] });
                                }
                                uvIndices[i] = uvCache[uvKey];
                            }

                            // 插入UV面数据
                            data.uvFaces.insert(data.uvFaces.end(),
                                { uvIndices[0], uvIndices[1],
                                 uvIndices[2], uvIndices[3] });
                        }

                        // 增加面ID
                        faceId++;
                    }
                }
            }
        }
    }
}

// 处理模型数据的方法
ModelData ProcessModelData(const nlohmann::json& modelJson) {
    ModelData data;

    // 处理纹理和材质
    std::unordered_map<std::string, int> textureKeyToMaterialIndex;

    // 处理元素生成材质数据
    processTextures(modelJson, data, textureKeyToMaterialIndex);

    // 处理元素生成几何数据
    processElements(modelJson, data, textureKeyToMaterialIndex);

    return data;
}

// 将model类型的json文件变为网格数据
ModelData ProcessModelJson(const std::string& namespaceName, const std::string& blockId, int rotationX, int rotationY) {    
    // 生成唯一缓存键
    std::string cacheKey = namespaceName + ":" + blockId;
    // 在访问缓存前加锁
    std::lock_guard<std::mutex> lock(cacheMutex);
    // 检查缓存是否存在
    auto cacheIt = modelCache.find(cacheKey);
    if (cacheIt != modelCache.end()) {
        // 从缓存中获取原始模型数据
        ModelData cachedModel = cacheIt->second;

        ApplyRotationToVertices(cachedModel.vertices, rotationX, rotationY);

        return cachedModel;
    }
    // 缓存未命中，正常加载模型
    nlohmann::json modelJson = GetModelJson(namespaceName, blockId);

    ModelData modelData;
    if (modelJson.is_null()) {
        return modelData;
    }
    // 递归加载父模型并合并属性
    modelJson = LoadParentModel(namespaceName, blockId, modelJson);
    
    // 处理模型数据（不包含旋转）
    modelData = ProcessModelData(modelJson);


    // 将原始数据存入缓存（不包含旋转）
    modelCache[cacheKey] = modelData;

    ApplyRotationToVertices(modelData.vertices, rotationX, rotationY);

    return modelData;
}


//——————————————合并网格体方法———————————————

ModelData MergeModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    // 使用哈希表进行顶点去重（Key结构：x,y,z）
    std::unordered_map<std::string, int> vertexMap;
    std::vector<float> uniqueVertices;

    // 使用哈希表进行UV去重（Key结构：u,v）
    std::unordered_map<std::string, int> uvMap;
    std::vector<float> uniqueUVs;

    // 索引映射表（旧索引 -> 新索引）
    std::vector<int> vertexIndexMap;
    std::vector<int> uvIndexMap;

    //------------------------ 阶段1：顶点处理 ------------------------
    auto processVertices = [&](const std::vector<float>& vertices) {
        const size_t count = vertices.size() / 3;
        vertexIndexMap.reserve(vertexIndexMap.size() + count);

        for (size_t i = 0; i < count; ++i) {
            // 生成唯一键（精度到小数点后6位）
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.6f,%.6f,%.6f",
                vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]);

            std::string key(buffer);

            auto it = vertexMap.find(key);
            if (it == vertexMap.end()) {
                // 新顶点
                int newIndex = uniqueVertices.size() / 3;
                uniqueVertices.insert(uniqueVertices.end(),
                    &vertices[i * 3], &vertices[i * 3 + 3]);
                vertexMap[key] = newIndex;
                vertexIndexMap.push_back(newIndex);
            }
            else {
                // 已存在顶点
                vertexIndexMap.push_back(it->second);
            }
        }
        };

    // 处理data1顶点
    processVertices(data1.vertices);
    // 处理data2顶点（偏移量保持不变）
    processVertices(data2.vertices);
    mergedData.vertices = std::move(uniqueVertices);

    //------------------------ 阶段2：UV处理 ------------------------
    auto processUVs = [&](const std::vector<float>& uvs) {
        const size_t count = uvs.size() / 2;
        uvIndexMap.reserve(uvIndexMap.size() + count);

        for (size_t i = 0; i < count; ++i) {
            // 生成唯一键（精度到小数点后6位）
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%.6f,%.6f",
                uvs[i * 2], uvs[i * 2 + 1]);

            std::string key(buffer);

            auto it = uvMap.find(key);
            if (it == uvMap.end()) {
                // 新UV
                int newIndex = uniqueUVs.size() / 2;
                uniqueUVs.push_back(uvs[i * 2]);
                uniqueUVs.push_back(uvs[i * 2 + 1]);
                uvMap[key] = newIndex;
                uvIndexMap.push_back(newIndex);
            }
            else {
                // 已存在UV
                uvIndexMap.push_back(it->second);
            }
        }
        };

    // 处理data1 UV
    processUVs(data1.uvCoordinates);
    // 处理data2 UV
    processUVs(data2.uvCoordinates);
    mergedData.uvCoordinates = std::move(uniqueUVs);

    //------------------------ 阶段3：面数据处理 ------------------------
    // 合并面数据（使用映射后的索引）
    auto remapFaces = [&](const std::vector<int>& faces, bool isData1) {
        const size_t faceCount = faces.size() / 4;
        const int indexOffset = isData1 ? 0 : data1.vertices.size() / 3;

        for (size_t i = 0; i < faceCount; ++i) {
            std::array<int, 4> remapped;
            for (int j = 0; j < 4; ++j) {
                const int originalIndex = faces[i * 4 + j] + (isData1 ? 0 : indexOffset);
                remapped[j] = vertexIndexMap[originalIndex];
            }
            mergedData.faces.insert(mergedData.faces.end(), remapped.begin(), remapped.end());
        }
        };

    remapFaces(data1.faces, true);
    remapFaces(data2.faces, false);

    // 合并UV面数据（使用映射后的索引）
    auto remapUVFaces = [&](const std::vector<int>& uvFaces, bool isData1) {
        const size_t faceCount = uvFaces.size() / 4;
        const int indexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;

        for (size_t i = 0; i < faceCount; ++i) {
            std::array<int, 4> remapped;
            for (int j = 0; j < 4; ++j) {
                const int originalIndex = uvFaces[i * 4 + j] + (isData1 ? 0 : indexOffset);
                remapped[j] = uvIndexMap[originalIndex];
            }
            mergedData.uvFaces.insert(mergedData.uvFaces.end(), remapped.begin(), remapped.end());
        }
        };

    remapUVFaces(data1.uvFaces, true);
    remapUVFaces(data2.uvFaces, false);

    // 阶段4：合并材质数据
    // 创建材质映射表（data2旧索引 -> 新索引）
    std::vector<int> materialIndexMap(data2.materialNames.size(), -1);

    // 初始化合并后的材质列表
    mergedData.materialNames = data1.materialNames;
    mergedData.texturePaths = data1.texturePaths;

    // 合并材质并建立映射
    for (size_t i = 0; i < data2.materialNames.size(); ++i) {
        auto it = std::find(mergedData.materialNames.begin(),
            mergedData.materialNames.end(),
            data2.materialNames[i]);

        if (it != mergedData.materialNames.end()) {
            // 材质已存在，记录映射关系
            materialIndexMap[i] = std::distance(mergedData.materialNames.begin(), it);
        }
        else {
            // 添加新材质
            materialIndexMap[i] = mergedData.materialNames.size();
            mergedData.materialNames.push_back(data2.materialNames[i]);
            mergedData.texturePaths.push_back(data2.texturePaths[i]);
        }
    }

    // 合并材质索引
    mergedData.materialIndices = data1.materialIndices;
    for (int original_idx : data2.materialIndices) {
        if (original_idx != -1) {
            mergedData.materialIndices.push_back(materialIndexMap[original_idx]);
        }
        else {
            mergedData.materialIndices.push_back(-1);
        }
    }

    // 合并其他元数据
    mergedData.objName = data1.objName + "_merged_" + data2.objName;

    return mergedData;
}


void MergeModelsDirectly(ModelData& data1, const ModelData& data2) {
    // 阶段1：合并顶点数据
    const int data1_vertex_count = data1.vertices.size() / 3;  // 计算原始顶点数量
    data1.vertices.insert(data1.vertices.end(),
        data2.vertices.begin(),
        data2.vertices.end());

    // 阶段2：合并UV坐标
    const int data1_uv_count = data1.uvCoordinates.size() / 2;  // 计算原始UV数量
    data1.uvCoordinates.insert(data1.uvCoordinates.end(),
        data2.uvCoordinates.begin(),
        data2.uvCoordinates.end());

    // 阶段3：合并面数据
    const size_t original_face_count = data1.faces.size() / 4;

    // 调整并合并顶点索引
    std::vector<int> adjusted_faces;
    adjusted_faces.reserve(data2.faces.size());
    for (int idx : data2.faces) {
        adjusted_faces.push_back(idx + data1_vertex_count);
    }
    data1.faces.insert(data1.faces.end(),
        adjusted_faces.begin(),
        adjusted_faces.end());

    // 调整并合并UV索引
    std::vector<int> adjusted_uv_faces;
    adjusted_uv_faces.reserve(data2.uvFaces.size());
    for (int uv_idx : data2.uvFaces) {
        adjusted_uv_faces.push_back(uv_idx + data1_uv_count);
    }
    data1.uvFaces.insert(data1.uvFaces.end(),
        adjusted_uv_faces.begin(),
        adjusted_uv_faces.end());

    // 阶段4：合并材质信息
    std::vector<int> materialIndexMap(data2.materialNames.size(), -1);

    // 建立材质映射表
    for (size_t i = 0; i < data2.materialNames.size(); ++i) {
        auto it = std::find(data1.materialNames.begin(),
            data1.materialNames.end(),
            data2.materialNames[i]);

        if (it != data1.materialNames.end()) {
            materialIndexMap[i] = std::distance(data1.materialNames.begin(), it);
        }
        else {
            materialIndexMap[i] = data1.materialNames.size();
            data1.materialNames.push_back(data2.materialNames[i]);
            data1.texturePaths.push_back(data2.texturePaths[i]);
        }
    }

    // 合并材质索引
    const size_t original_material_count = data1.materialIndices.size();
    data1.materialIndices.reserve(original_material_count + data2.materialIndices.size());
    for (int original_idx : data2.materialIndices) {
        data1.materialIndices.push_back(
            (original_idx != -1) ? materialIndexMap[original_idx] : -1
        );
    }
}



//——————————————导出.obg/.mtl方法—————————————

// 创建 .obj 文件并写入内容
void createObjFile(const ModelData& data, const std::string& objName) {
    std::string exeDir = getExecutableDir();

    std::string objFilePath = exeDir + objName + ".obj";
    std::string mtlFilePath = objName + ".mtl";

    std::ofstream objFile(objFilePath);
    if (objFile.is_open()) {
        // 写入文件头
        objFile << "# Generated by DeepSeek Model Processor\n";
        objFile << "mtllib " << mtlFilePath << "\n";
        objFile << "o " << data.objName << "\n\n"; // 使用模型自带名称

        // 写入顶点数据（每3个元素一个顶点）
        objFile << "# Vertices (" << data.vertices.size() / 3 << ")\n";
        for (size_t i = 0; i < data.vertices.size(); i += 3) {
            objFile << "v " << data.vertices[i] << " "
                << data.vertices[i + 1] << " "
                << data.vertices[i + 2] << "\n";
        }

        // 写入UV坐标（每2个元素一个UV）
        objFile << "\n# UVs (" << data.uvCoordinates.size() / 2 << ")\n";
        for (size_t i = 0; i < data.uvCoordinates.size(); i += 2) {
            objFile << "vt " << data.uvCoordinates[i] << " "
                << data.uvCoordinates[i + 1] << "\n";
        }

        // 按材质分组面（优化分组算法）
        std::unordered_map<int, std::vector<size_t>> materialGroups;
        const size_t totalFaces = data.faces.size() / 4;
        for (size_t faceIdx = 0; faceIdx < totalFaces; ++faceIdx) {
            const int matIndex = data.materialIndices[faceIdx];
            if (matIndex != -1) {
                materialGroups[matIndex].push_back(faceIdx);
            }
        }

        // 写入面数据（优化内存访问模式）
        objFile << "\n# Faces (" << totalFaces << ")\n";
        for (const auto& matPair : materialGroups) { // C++14兼容写法
            const int matIndex = matPair.first;
            const std::vector<size_t>& faceIndices = matPair.second;

            objFile << "usemtl " << data.materialNames[matIndex] << "\n";

            for (const size_t faceIdx : faceIndices) {
                const size_t base = faceIdx * 4;
                objFile << "f ";
                for (int i = 0; i < 4; ++i) {
                    const int vIdx = data.faces[base + i] + 1;
                    const int uvIdx = data.uvFaces[base + i] + 1;
                    objFile << vIdx << "/" << uvIdx << " ";
                }
                objFile << "\n";
            }
        }


        objFile.close();
    }
    else {
        std::cerr << "Error: Failed to create " << objFilePath << "\n";
    }
}

// 创建 .mtl 文件，接收 textureToPath 作为参数
void createMtlFile(const ModelData& data, const std::string& mtlFileName) {
    // 获取可执行文件目录路径
    std::string exeDir = getExecutableDir();
    // 构建完整路径
    std::string fullMtlPath = exeDir + mtlFileName + ".mtl";

    std::ofstream mtlFile(fullMtlPath);
    if (mtlFile.is_open()) {
        for (size_t i = 0; i < data.materialNames.size(); ++i) {
            const std::string& textureName = data.materialNames[i];  // 材质名称
            std::string texturePath = data.texturePaths[i];  // 材质对应的文件路径

            // 确保路径以 ".png" 结尾，如果没有则加上
            if (texturePath.substr(texturePath.find_last_of(".") + 1) != "png") {
                texturePath += ".png";
            }

            // 写入材质名称
            mtlFile << "newmtl " << textureName << std::endl;

            // 下面是固定的材质属性，您可以根据需要修改
            mtlFile << "Ns 90.000000" << std::endl; // 光泽度
            mtlFile << "Ka 1.000000 1.000000 1.000000" << std::endl; // 环境光颜色
            mtlFile << "Ks 0.000000 0.000000 0.000000" << std::endl; // 镜面反射
            mtlFile << "Ke 0.000000 0.000000 0.000000" << std::endl; // 自发光
            mtlFile << "Ni 1.500000" << std::endl; // 折射率
            mtlFile << "illum 1" << std::endl; // 照明模型

            // 写入纹理信息，注意这里是相对路径
            mtlFile << "map_Kd " << texturePath << std::endl; // 颜色纹理
            mtlFile << "map_d " << texturePath << std::endl;  // 透明度纹理

            mtlFile << std::endl; // 添加空行，以便分隔不同材质
        }

        // 关闭文件
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFileName << std::endl;
    }
}

// 单独的文件创建方法
void CreateModelFiles(const ModelData& data, const std::string& filename) {
    // 创建OBJ文件
    createObjFile(data, filename);
    // 创建MTL文件
    createMtlFile(data, filename);
}
