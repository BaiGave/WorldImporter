#include "model.h"
#include "fileutils.h"
#include "EntityBlock.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <omp.h>
#include <chrono>

using namespace std::chrono;  

//============== 辅助函数模块 ==============//
//---------------- 路径处理 ----------------
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

//---------------- 几何变换 ----------------
// 旋转函数
void ApplyRotationToVertices(std::vector<float>& vertices, int rotationX, int rotationY) {
    // 参数校验
    if (vertices.size() % 3 != 0) {
        throw std::invalid_argument("Invalid vertex data size");
    }

    // 绕 X 轴旋转（90度增量）
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        // 将坐标平移到以 (0.5, 0.5) 为中心
        y -= 0.5f;
        z -= 0.5f;

        switch (rotationX) {
        case 90:
            std::tie(y, z) = std::make_pair(z, -y);
            break;
        case 180:
            y = -y;
            z = -z;
            break;
        case 270:
            std::tie(y, z) = std::make_pair(-z, y);
            break;
        default:
            break;
        }

        // 将坐标平移回原位置
        y += 0.5f;
        z += 0.5f;
    }

    // 绕 Y 轴旋转（90度增量）
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        // 将坐标平移到以 (0.5, 0.5) 为中心
        x -= 0.5f;
        z -= 0.5f;

        switch (rotationY) {
        case 90:
            std::tie(x, z) = std::make_pair(-z, x);
            break;
        case 180:
            x = -x;
            z = -z;
            break;
        case 270:
            std::tie(x, z) = std::make_pair(z, -x);
            break;
        default:
            break;
        }

        // 将坐标平移回原位置
        x += 0.5f;
        z += 0.5f;
    }
}
// 带旋转中心的UV旋转（内联优化）
static inline void fastRotateUV(float& u, float& v, float cosA, float sinA) {
    constexpr float centerU = 0.5f;
    constexpr float centerV = 0.5f;

    const float relU = u - centerU;
    const float relV = v - centerV;

    // 向量化友好计算
    const float newU = relU * cosA - relV * sinA + centerU;
    const float newV = relU * sinA + relV * cosA + centerV;

    // 快速clamp替代方案
    u = newU < 0.0f ? 0.0f : (newU > 1.0f ? 1.0f : newU);
    v = newV < 0.0f ? 0.0f : (newV > 1.0f ? 1.0f : newV);
}

// 预计算三角函数值（包含常见角度优化）
static void getCosSin(int angle, float& cosA, float& sinA) {
    angle = (angle % 360 + 360) % 360;

    // 常见角度快速返回
    switch (angle) {
    case 0:   cosA = 1.0f; sinA = 0.0f; return;
    case 90:  cosA = 0.0f; sinA = 1.0f; return;
    case 180: cosA = -1.0f; sinA = 0.0f; return;
    case 270: cosA = 0.0f; sinA = -1.0f; return;
    }

    const float rad = angle * (3.14159265f / 180.0f);
    cosA = std::cos(rad);
    sinA = std::sin(rad);
}

// 优化后的UV分离
static void createUniqueUVs(ModelData& modelData) {
    std::vector<float> newUVs;
    std::vector<int> newUVFaces;
    const size_t total = modelData.uvFaces.size();

    newUVs.reserve(total * 2);
    newUVFaces.reserve(total);

    // 循环展开优化
    for (size_t i = 0; i < total; ++i) {
        const int idx = modelData.uvFaces[i];
        newUVs.push_back(modelData.uvCoordinates[idx * 2]);
        newUVs.push_back(modelData.uvCoordinates[idx * 2 + 1]);
        newUVFaces.push_back(static_cast<int>(i));
    }

    modelData.uvCoordinates = std::move(newUVs);
    modelData.uvFaces = std::move(newUVFaces);
}

// 应用旋转到面（批量处理优化）
static void applyFaceRotation(ModelData& modelData, size_t faceIdx, int angle) {
    if (angle == 0) return;

    float cosA, sinA;
    getCosSin(angle, cosA, sinA);
    if (cosA == 1.0f && sinA == 0.0f) return;

    const int base = static_cast<int>(faceIdx) * 4;
    int indices[4]; // 局部缓存索引

    // 预加载索引
    for (int i = 0; i < 4; ++i) {
        indices[i] = modelData.uvFaces[base + i] * 2;
    }

    // 批量处理顶点
    for (int i = 0; i < 4; ++i) {
        float& u = modelData.uvCoordinates[indices[i]];
        float& v = modelData.uvCoordinates[indices[i] + 1];
        fastRotateUV(u, v, cosA, sinA);
    }
}

// 主逻辑优化（预处理面类型+并行处理）
void ApplyRotationToUV(ModelData& modelData, int rotationX, int rotationY) {
    createUniqueUVs(modelData);

    // 预处理面类型
    std::vector<FaceType> faceTypes;
    faceTypes.reserve(modelData.faceNames.size());
    for (const auto& name : modelData.faceNames) {
        if (name == "up") faceTypes.push_back(UP);
        else if (name == "down") faceTypes.push_back(DOWN);
        else if (name == "north") faceTypes.push_back(NORTH);
        else if (name == "south") faceTypes.push_back(SOUTH);
        else if (name == "west") faceTypes.push_back(WEST);
        else if (name == "east") faceTypes.push_back(EAST);
        else faceTypes.push_back(UNKNOWN);
    }

    // 根据旋转组合处理UV旋转
    auto getCase = [rotationX, rotationY]() {
        return std::to_string(rotationX) + "-" + std::to_string(rotationY);
        };

    // OpenMP并行处理
#pragma omp parallel for
    for (int i = 0; i < static_cast<int>(faceTypes.size()); ++i) {
        const FaceType face = faceTypes[i];
        int angle = 0;

        const std::string caseKey = getCase();

        if (caseKey == "0-0") {
            // 不旋转
        }
        else if (caseKey == "0-90" || caseKey == "0-180" || caseKey == "0-270") {
            if (face == UP) angle = -rotationY;
            else if (face == DOWN) angle = -rotationY;
        }
        else if (caseKey == "90-0") {
            if (face == UP) angle = 180;
            else if (face == EAST) angle = 180;
            else if (face == WEST) angle = 90;
            else if (face == NORTH) angle = -90;
        }
        else if (caseKey == "90-90") {
            if (face == UP) angle = 180;
            else if (face == EAST) angle = -90;
            else if (face == DOWN) angle = -90;
            else if (face == WEST) angle = 90;
            else if (face == NORTH) angle = -90;
        }
        else if (caseKey == "90-180") {
            if (face == NORTH) angle = 180;
            else if (face == DOWN) angle = 180;
            else if (face == WEST) angle = 90;
            else if (face == UP) angle = -90;
        }
        else if (caseKey == "90-270") {
            if (face == UP) angle = 180;
            else if (face == EAST) angle = 90;
            else if (face == DOWN) angle = 90;
            else if (face == WEST) angle = 90;
            else if (face == NORTH) angle = -90;
        }
        else if (caseKey == "180-0") {
            if (face == UP) angle = rotationY;
            else if (face == EAST) angle = 180;
            else if (face == SOUTH) angle = 180;
            else if (face == WEST) angle = 180;
            else if (face == NORTH) angle = 180;
            else if (face == DOWN) angle = rotationY;
        }
        else if (caseKey == "180-90") {
            if (face == UP) angle = rotationY;
            else if (face == EAST) angle = 180;
            else if (face == SOUTH) angle = 180;
            else if (face == WEST) angle = 180;
            else if (face == NORTH) angle = 180;
            else if (face == DOWN) angle = rotationY;
        }
        else if (caseKey == "180-180") {
            if (face == UP) angle = rotationY;
            else if (face == EAST) angle = 180;
            else if (face == SOUTH) angle = 180;
            else if (face == WEST) angle = 180;
            else if (face == NORTH) angle = 180;
            else if (face == DOWN) angle = rotationY;
        }
        else if (caseKey=="180-270") {
            if (face == UP) angle = rotationY;
            else if (face == EAST) angle = 180;
            else if (face == SOUTH) angle = 180;
            else if (face == WEST) angle = 180;
            else if (face == NORTH) angle = 180;
            else if (face == DOWN) angle = rotationY;
        }
        else if (caseKey == "270-0") {
            if (face == EAST) angle = 180;
            else if (face == WEST) angle = -90;
            else if (face == NORTH) angle = 90;
            else if (face == SOUTH) angle = 180;
        }
        else if (caseKey == "270-90") {
            if (face == EAST) angle = 90;
            else if (face == DOWN) angle = 90;
            else if (face == WEST) angle = -90;
            else if (face == NORTH) angle = 90;
            else if (face == SOUTH) angle = 180;
        }
        else if (caseKey == "270-180") {
            if (face == DOWN) angle = 180;
            else if (face == WEST) angle = -90;
            else if (face == NORTH) angle = 90;
            else if (face == SOUTH) angle = 180;
        }
        else if (caseKey == "270-270") {
            if (face == EAST) angle = -90;
            else if (face == DOWN) angle = -90;
            else if (face == WEST) angle = -90;
            else if (face == NORTH) angle = 90;
            else if (face == SOUTH) angle = 180;
        }
        else {
            // 未处理的旋转组合
            static std::unordered_set<std::string> warnedCases;
            if (warnedCases.find(caseKey) == warnedCases.end()) {
                warnedCases.insert(caseKey);
                std::cerr << "Bad UV lock rotation in model: " << caseKey << std::endl;
            }
        }

        if (angle != 0) {
            applyFaceRotation(modelData, i, angle);
        }
    }
}

// 旋转函数
void ApplyRotationToFaceDirections(std::vector<std::string>& faceDirections, int rotationX, int rotationY) {
    // 定义旋转规则
    auto rotateY = [](const std::string& direction) -> std::string {
        if (direction == "north") return "east";
        else if (direction == "east") return "south";
        else if (direction == "south") return "west";
        else if (direction == "west") return "north";
        else return direction;
        };

    auto rotateYReverse = [](const std::string& direction) -> std::string {
        if (direction == "north") return "west";
        else if (direction == "west") return "south";
        else if (direction == "south") return "east";
        else if (direction == "east") return "north";
        else return direction;
        };

    auto rotateX = [](const std::string& direction) -> std::string {
        if (direction == "north") return "up";
        else if (direction == "up") return "south";
        else if (direction == "south") return "down";
        else if (direction == "down") return "north";
        else return direction;
        };

    auto rotateXReverse = [](const std::string& direction) -> std::string {
        if (direction == "north") return "down";
        else if (direction == "down") return "south";
        else if (direction == "south") return "up";
        else if (direction == "up") return "north";
        else return direction;
        };
    // 绕 X 轴旋转（90度增量）
    for (std::string& dir : faceDirections) {
        switch (rotationX) {
        case 270:  dir = rotateX(dir); break;
        case 180: dir = rotateX(rotateX(dir)); break;
        case 90: dir = rotateXReverse(dir); break;
        }
    }
    // 绕 Y 轴旋转（90度增量）
    for (std::string& dir : faceDirections) {
        switch (rotationY) {
        case 90:  dir = rotateY(dir); break;
        case 180: dir = rotateY(rotateY(dir)); break;
        case 270: dir = rotateYReverse(dir); break;
        }
    }

    
}

//============== 模型数据处理模块 ==============//
//---------------- JSON处理 ----------------
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

nlohmann::json GetModelJson(const std::string& namespaceName, const std::string& modelPath) {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    // 按照 JAR 文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + modelPath;
        
        auto it = GlobalCache::models.find(cacheKey);
        if (it != GlobalCache::models.end()) {
            return it->second;
        }
    }

    std::cerr << "Model not found: " << namespaceName << ":" << modelPath << std::endl;
    return nlohmann::json();
}

//———————————将JSON数据转为结构体的方法———————————————
//---------------- 材质处理 ----------------
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
                        textureSavePath = "textures/" + namespaceName+"/"+pathPart + ".png";
                        // 调用注册材质的方法
                        RegisterTexture(namespaceName, pathPart, textureSavePath);
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
//---------------- 几何数据处理 ----------------
void processElements(const nlohmann::json& modelJson, ModelData& data,
    const std::unordered_map<std::string, int>& textureKeyToMaterialIndex)
{
    std::unordered_map<std::string, int> vertexCache;
    std::unordered_map<std::string, int> uvCache;
    int faceId = 0;
    std::unordered_map<std::string, int> faceCountMap; // 面计数映射

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
            std::unordered_map<std::string, std::vector<std::vector<float>>> elementVertices;
            // 遍历元素的面，动态生成顶点数据
            for (auto& face : faces.items()) {
                std::string faceName = face.key();
                if (faceName == "north") {
                    elementVertices[faceName] = { {x1, y1, z1}, {x1, y2, z1}, {x2, y2, z1}, {x2, y1, z1} };
                }
                else if (faceName == "south") {
                    elementVertices[faceName] = { {x2, y1, z2}, {x2, y2, z2}, {x1, y2, z2}, {x1, y1, z2} };
                }
                else if (faceName == "east") {
                    elementVertices[faceName] = { {x2, y1, z1}, {x2, y2, z1}, {x2, y2, z2}, {x2, y1, z2} };
                }
                else if (faceName == "west") {
                    elementVertices[faceName] = { {x1, y1, z2}, {x1, y2, z2}, {x1, y2, z1}, {x1, y1, z1} };
                }
                else if (faceName == "up") {
                    elementVertices[faceName] = { {x1, y2, z1}, {x1, y2, z2}, {x2, y2, z2}, {x2, y2, z1} };
                }
                else if (faceName == "down") {
                    elementVertices[faceName] = { {x2, y1, z2}, {x1, y1, z2}, {x1, y1, z1}, {x2, y1, z1} };
                }
            }

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

                // 处理rescale参数
                // 在旋转处理部分的缩放逻辑修改如下：
                bool rescale = rotation.value("rescale", false);
                if (rescale) {
                    float angle_deg = angle_rad * 180.0f / M_PI;
                    bool applyScaling = false;
                    float scale = 1.0f;

                    // 检查是否为22.5°或45°的整数倍（考虑浮点精度）
                    if (std::fabs(angle_deg - 22.5f) < 1e-6 || std::fabs(angle_deg + 22.5f) < 1e-6) {
                        applyScaling = true;
                        scale = std::sqrt(2.0f - std::sqrt(2.0f)); // 22.5°对应的缩放因子
                    }
                    else if (std::fabs(angle_deg - 45.0f) < 1e-6 || std::fabs(angle_deg + 45.0f) < 1e-6) {
                        applyScaling = true;
                        scale = std::sqrt(2.0f);           // 45°对应的缩放因子
                    }

                    if (applyScaling) {
                        // 根据旋转轴应用缩放，保留原有旋转中心偏移逻辑
                        for (auto& faceEntry : elementVertices) {
                            auto& vertices = faceEntry.second;
                            for (auto& vertex : vertices) {
                                float& vx = vertex[0];
                                float& vy = vertex[1];
                                float& vz = vertex[2];

                                // 平移至旋转中心相对坐标系
                                float tx = vx - ox;
                                float ty = vy - oy;
                                float tz = vz - oz;

                                // 根据轴类型应用缩放
                                if (axis == "x") {
                                    ty *= scale;
                                    tz *= scale;
                                }
                                else if (axis == "y") {
                                    tx *= scale;
                                    tz *= scale;
                                }
                                else if (axis == "z") {
                                    tx *= scale;
                                    ty *= scale;
                                }

                                // 平移回原坐标系
                                vx = tx + ox;
                                vy = ty + oy;
                                vz = tz + oz;
                            }
                        }
                    }
                }
            }

            // --- 新增：检测并移除相反方向的重叠面 ---
            auto getOppositeFace = [](const std::string& faceName) -> std::string {
                if (faceName == "north") return "south";
                if (faceName == "south") return "north";
                if (faceName == "east") return "west";
                if (faceName == "west") return "east";
                if (faceName == "up") return "down";
                if (faceName == "down") return "up";
                return "";
                };

            auto areFacesCoinciding = [](const std::vector<std::vector<float>>& face1,
                const std::vector<std::vector<float>>& face2) -> bool {
                    if (face1.size() != face2.size()) return false;

                    auto toKey = [](const std::vector<float>& v) {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "%.4f,%.4f,%.4f", v[0], v[1], v[2]);
                        return std::string(buffer);
                        };

                    std::unordered_set<std::string> set1;
                    for (const auto& v : face1) set1.insert(toKey(v));
                    for (const auto& v : face2) {
                        if (!set1.count(toKey(v))) return false;
                    }
                    return true;
                };

            // 用于记录需要覆盖 uv 的信息，key 为保留面的名称
            std::vector<std::string> facesToRemove;
            for (const auto& faceEntry : elementVertices) {
                const std::string& faceName = faceEntry.first;
                std::string opposite = getOppositeFace(faceName);
                auto oppositeIt = elementVertices.find(opposite);

                // 反向面存在且重叠时才移除
                if (oppositeIt != elementVertices.end()) {
                    if (areFacesCoinciding(faceEntry.second, oppositeIt->second)) {
                        // 根据 faceName 判断移除哪一面
                        if (faceName == "south" || faceName == "west" || faceName == "down") {
                            facesToRemove.push_back(faceName);
                        }
                        else {
                            facesToRemove.push_back(opposite);
                        }
                    }
                }
            }

            // 去重并移除面
            std::sort(facesToRemove.begin(), facesToRemove.end());
            auto last = std::unique(facesToRemove.begin(), facesToRemove.end());
            facesToRemove.erase(last, facesToRemove.end());
            for (const auto& face : facesToRemove) {
                elementVertices.erase(face);
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

                        float vec1x = v1[0] - v0[0];
                        float vec1y = v1[1] - v0[1];
                        float vec1z = v1[2] - v0[2];
                        float vec2x = v2[0] - v0[0];
                        float vec2y = v2[1] - v0[1];
                        float vec2z = v2[2] - v0[2];

                        float crossX = vec1y * vec2z - vec1z * vec2y;
                        float crossY = vec1z * vec2x - vec1x * vec2z;
                        float crossZ = vec1x * vec2y - vec1y * vec2x;

                        float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
                        if (length > 0) {
                            crossX /= length;
                            crossY /= length;
                            crossZ /= length;
                        }

                        crossX = std::round(crossX * 100.0f) / 100.0f;
                        crossY = std::round(crossY * 100.0f) / 100.0f;
                        crossZ = std::round(crossZ * 100.0f) / 100.0f;

                        float centerX = 0.0f, centerY = 0.0f, centerZ = 0.0f;
                        for (const auto& v : faceVertices) {
                            centerX += v[0];
                            centerY += v[1];
                            centerZ += v[2];
                        }
                        centerX /= faceVertices.size();
                        centerY /= faceVertices.size();
                        centerZ /= faceVertices.size();

                        centerX = std::round(centerX * 10000.0f) / 10000.0f;
                        centerY = std::round(centerY * 10000.0f) / 10000.0f;
                        centerZ = std::round(centerZ * 10000.0f) / 10000.0f;

                        std::stringstream keyStream;
                        keyStream << std::fixed << std::setprecision(2)
                            << crossX << "," << crossY << "," << crossZ << "_"
                            << std::setprecision(4)
                            << centerX << "," << centerY << "," << centerZ;
                        std::string key = keyStream.str();

                        bool skipFace = false;
                        if (config.allowDoubleFace) {
                            int count = ++faceCountMap[key];
                            float offset = (count - 1) * 0.001f;
                            for (auto& v : faceVertices) {
                                v[0] += crossX * offset;
                                v[1] += crossY * offset;
                                v[2] += crossZ * offset;
                            }
                        }
                        else {
                            if (faceCountMap[key]++ >= 1) {
                                skipFace = true;
                            }
                        }

                        if (skipFace) {
                            continue;
                        }

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

                        if (vertexCache.find(vertexKey) == vertexCache.end()) {
                            vertexCache[vertexKey] = data.vertices.size() / 3;
                            data.vertices.insert(data.vertices.end(),
                                { vertex[0], vertex[1], vertex[2] });
                        }
                        vertexIndices[i] = vertexCache[vertexKey];
                    }

                    data.faces.insert(data.faces.end(),
                        { vertexIndices[0], vertexIndices[1],
                            vertexIndices[2], vertexIndices[3] });

                    data.materialIndices.resize(faceId + 1, -1);
                    if (face.value().contains("texture")) {
                        std::string texture = face.value()["texture"];
                        if (texture.front() == '#') texture.erase(0, 1);

                        auto it = textureKeyToMaterialIndex.find(texture);
                        if (it != textureKeyToMaterialIndex.end()) {
                            data.materialIndices[faceId] = it->second;
                        }
                        std::vector<float> uvRegion;
                        if (faceName == "down")
                        {
                            uvRegion = { x1 * 16, (1 - z2) * 16, x2 * 16, (1 - z1) * 16 };
                        }
                        else if (faceName == "up")
                        {
                            uvRegion = { x1 * 16, z1 * 16, x2 * 16, z2 * 16 };
                        }
                        else if (faceName == "north")
                        {
                            uvRegion = { (1 - x2) * 16, (1 - y2) * 16, (1 - x1) * 16, (1 - y1) * 16 };
                        }
                        else if (faceName == "south")
                        {
                            uvRegion = { x1 * 16, (1 - y2) * 16, x2 * 16, (1 - y1) * 16 };
                        }
                        else if (faceName == "west")
                        {
                            uvRegion = { z1 * 16, (1 - y2) * 16, z2 * 16, (1 - y1) * 16 };
                        }
                        else if (faceName == "east")
                        {
                            uvRegion = { (1 - z2) * 16, (1 - y2) * 16, (1 - z1) * 16, (1 - y1) * 16 };
                        }

                        // 如果 JSON 中存在 uv 则使用其数据
                        if (face.value().contains("uv")) {
                            auto uv = face.value()["uv"];
                           
                            uvRegion = {
                                uv[0].get<float>(),
                                uv[1].get<float>(),
                                uv[2].get<float>(),
                                uv[3].get<float>()
                            };
                        }
                  
                        std::array<int, 4> uvIndices;
                        // 计算四个 UV 坐标点（注意这里直接使用 uvRegion 进行转换）
                        std::vector<std::vector<float>> uvCoords = {
                            {uvRegion[2] / 16.0f, 1 - uvRegion[3] / 16.0f},
                            {uvRegion[2] / 16.0f, 1 - uvRegion[1] / 16.0f},
                            {uvRegion[0] / 16.0f, 1 - uvRegion[1] / 16.0f},
                            {uvRegion[0] / 16.0f, 1 - uvRegion[3] / 16.0f}
                        };

                        int rotation = face.value().value("rotation", 0);
                        int steps = ((rotation % 360) + 360) % 360 / 90;

                        if (steps != 0) {
                            std::vector<std::vector<float>> rotatedUV(4);
                            for (int i = 0; i < 4; i++) {
                                rotatedUV[(i + steps) % 4] = uvCoords[i];
                            }
                            uvCoords = rotatedUV;
                        }

                        for (int i = 0; i < 4; ++i) {
                            const auto& uv = uvCoords[i];
                            std::string uvKey =
                                std::to_string(uv[0]) + "," +
                                std::to_string(uv[1]);

                            if (uvCache.find(uvKey) == uvCache.end()) {
                                uvCache[uvKey] = data.uvCoordinates.size() / 2;
                                data.uvCoordinates.insert(data.uvCoordinates.end(),
                                    { uv[0], uv[1] });
                            }
                            uvIndices[i] = uvCache[uvKey];
                        }

                        data.uvFaces.insert(data.uvFaces.end(),
                            { uvIndices[0], uvIndices[1],
                                uvIndices[2], uvIndices[3] });
                    }

                    std::string faceDirection;
                    if (face.value().contains("cullface")) {
                        faceDirection = face.value()["cullface"].get<std::string>();
                    }
                    else {
                        faceDirection = "DO_NOT_CULL";
                    }
                    short tintindex=-1;
                    if (face.value().contains("tintindex")) {
                        tintindex = face.value()["tintindex"].get<int>();
                    }
                    data.tintindex = tintindex;
                    
                    for (int i = 0; i < 4; ++i) {
                        data.faceDirections.push_back(faceDirection);
                    }
                    data.faceNames.push_back(faceName);
                    faceId++;
                }

            }
        }
    }

}


// 处理模型数据的方法
ModelData ProcessModelData(const nlohmann::json& modelJson, const std::string& blockName) {
    ModelData data;

    // 处理纹理和材质
    std::unordered_map<std::string, int> textureKeyToMaterialIndex;

    if (modelJson.contains("elements")) {
        // 处理元素生成材质数据
        processTextures(modelJson, data, textureKeyToMaterialIndex);

        // 处理元素生成几何数据
        processElements(modelJson, data, textureKeyToMaterialIndex);
    }
    else {
        // 当模型中没有 "elements" 字段时，生成实体方块模型
        data = EntityBlock::GenerateEntityBlockModel(blockName);
    }
    

    return data;
}

// 将model类型的json文件变为网格数据
ModelData ProcessModelJson(const std::string& namespaceName, const std::string& blockId,
    int rotationX, int rotationY, bool uvlock, int randomIndex,const std::string& blockstateName) {
    // 生成唯一缓存键（添加模型索引）
    std::string cacheKey = namespaceName + ":" + blockId + ":" + std::to_string(randomIndex);

    // 在访问缓存前加锁
    std::lock_guard<std::mutex> lock(cacheMutex);
    // 检查缓存是否存在
    auto cacheIt = modelCache.find(cacheKey);
    if (cacheIt != modelCache.end()) {
        // 从缓存中获取原始模型数据
        ModelData cachedModel = cacheIt->second;
        ApplyRotationToVertices(cachedModel.vertices, rotationX, rotationY);
        if (uvlock)
        {
            ApplyRotationToUV(cachedModel, rotationX, rotationY);
        }
        // 施加旋转到 faceDirections
        ApplyRotationToFaceDirections(cachedModel.faceDirections, rotationX, rotationY);
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
    modelData = ProcessModelData(modelJson,blockstateName);


    // 将原始数据存入缓存（不包含旋转）
    modelCache[cacheKey] = modelData;

    ApplyRotationToVertices(modelData.vertices, rotationX, rotationY);
    if (uvlock)
    {
        ApplyRotationToUV(modelData, rotationX, rotationY);
    }

    // 施加旋转到 faceDirections
    ApplyRotationToFaceDirections(modelData.faceDirections, rotationX, rotationY);
    return modelData;
}


//——————————————合并网格体方法———————————————

ModelData MergeModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1：顶点处理 ------------------------
    std::unordered_map<VertexKey, int> vertexMap;
    // 预估合并后顶点数量，避免重复 rehash
    vertexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);
    std::vector<float> uniqueVertices;
    uniqueVertices.reserve(data1.vertices.size() + data2.vertices.size());
    std::vector<int> vertexIndexMap;
    vertexIndexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);

    auto processVertices = [&](const std::vector<float>& vertices) {
        size_t count = vertices.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            float x = vertices[i * 3];
            float y = vertices[i * 3 + 1];
            float z = vertices[i * 3 + 2];
            // 转为整数表示（保留6位小数）
            int rx = static_cast<int>(std::round(x * 1000000.0f));
            int ry = static_cast<int>(std::round(y * 1000000.0f));
            int rz = static_cast<int>(std::round(z * 1000000.0f));
            VertexKey key{ rx, ry, rz };

            auto it = vertexMap.find(key);
            if (it == vertexMap.end()) {
                int newIndex = uniqueVertices.size() / 3;
                uniqueVertices.push_back(x);
                uniqueVertices.push_back(y);
                uniqueVertices.push_back(z);
                vertexMap[key] = newIndex;
                vertexIndexMap.push_back(newIndex);
            }
            else {
                vertexIndexMap.push_back(it->second);
            }
        }
        };

    processVertices(data1.vertices);
    processVertices(data2.vertices);
    mergedData.vertices = std::move(uniqueVertices);

    //------------------------ 阶段2：UV处理 ------------------------
    std::unordered_map<UVKey, int> uvMap;
    uvMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);
    std::vector<float> uniqueUVs;
    uniqueUVs.reserve(data1.uvCoordinates.size() + data2.uvCoordinates.size());
    std::vector<int> uvIndexMap;
    uvIndexMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);

    auto processUVs = [&](const std::vector<float>& uvs) {
        size_t count = uvs.size() / 2;
        for (size_t i = 0; i < count; ++i) {
            float u = uvs[i * 2];
            float v = uvs[i * 2 + 1];
            int ru = static_cast<int>(std::round(u * 1000000.0f));
            int rv = static_cast<int>(std::round(v * 1000000.0f));
            UVKey key{ ru, rv };

            auto it = uvMap.find(key);
            if (it == uvMap.end()) {
                int newIndex = uniqueUVs.size() / 2;
                uniqueUVs.push_back(u);
                uniqueUVs.push_back(v);
                uvMap[key] = newIndex;
                uvIndexMap.push_back(newIndex);
            }
            else {
                uvIndexMap.push_back(it->second);
            }
        }
        };

    processUVs(data1.uvCoordinates);
    processUVs(data2.uvCoordinates);
    mergedData.uvCoordinates = std::move(uniqueUVs);

    //------------------------ 阶段3：面数据处理 ------------------------
    // 面和 UV 面数据合并时直接使用映射后的索引
    auto remapFaces = [&](const std::vector<int>& faces, bool isData1) {
        size_t faceCount = faces.size() / 4;
        // data2 的顶点需要加上 data1 原始顶点数偏移
        int indexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        for (size_t i = 0; i < faceCount; ++i) {
            for (int j = 0; j < 4; ++j) {
                int originalIndex = faces[i * 4 + j] + (isData1 ? 0 : indexOffset);
                mergedData.faces.push_back(vertexIndexMap[originalIndex]);
            }
        }
        };

    remapFaces(data1.faces, true);
    remapFaces(data2.faces, false);

    auto remapUVFaces = [&](const std::vector<int>& uvFaces, bool isData1) {
        size_t faceCount = uvFaces.size() / 4;
        int indexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        for (size_t i = 0; i < faceCount; ++i) {
            for (int j = 0; j < 4; ++j) {
                int originalIndex = uvFaces[i * 4 + j] + (isData1 ? 0 : indexOffset);
                mergedData.uvFaces.push_back(uvIndexMap[originalIndex]);
            }
        }
        };

    remapUVFaces(data1.uvFaces, true);
    remapUVFaces(data2.uvFaces, false);

    //------------------------ 阶段4：材质数据合并 ------------------------
    // 使用哈希映射快速判断 data1 中已存在的材质
    std::unordered_map<std::string, int> materialMap;
    materialMap.reserve(data1.materialNames.size());
    mergedData.materialNames = data1.materialNames;
    mergedData.texturePaths = data1.texturePaths;

    for (size_t i = 0; i < data1.materialNames.size(); i++) {
        materialMap[data1.materialNames[i]] = static_cast<int>(i);
    }

    std::vector<int> materialIndexMap(data2.materialNames.size(), -1);
    for (size_t i = 0; i < data2.materialNames.size(); ++i) {
        auto it = materialMap.find(data2.materialNames[i]);
        if (it != materialMap.end()) {
            materialIndexMap[i] = it->second;
        }
        else {
            int newIndex = mergedData.materialNames.size();
            materialMap[data2.materialNames[i]] = newIndex;
            materialIndexMap[i] = newIndex;
            mergedData.materialNames.push_back(data2.materialNames[i]);
            mergedData.texturePaths.push_back(data2.texturePaths[i]);
        }
    }

    mergedData.materialIndices = data1.materialIndices;
    mergedData.materialIndices.reserve(data1.materialIndices.size() + data2.materialIndices.size());
    for (int original_idx : data2.materialIndices) {
        mergedData.materialIndices.push_back(
            (original_idx != -1) ? materialIndexMap[original_idx] : -1
        );
    }

    //------------------------ 阶段5：合并其他数据 ------------------------
    mergedData.faceDirections.reserve(data1.faceDirections.size() + data2.faceDirections.size());
    mergedData.faceDirections.insert(mergedData.faceDirections.end(),
        data1.faceDirections.begin(), data1.faceDirections.end());
    mergedData.faceDirections.insert(mergedData.faceDirections.end(),
        data2.faceDirections.begin(), data2.faceDirections.end());

    mergedData.faceNames.reserve(data1.faceNames.size() + data2.faceNames.size());
    mergedData.faceNames.insert(mergedData.faceNames.end(),
        data1.faceNames.begin(), data1.faceNames.end());
    mergedData.faceNames.insert(mergedData.faceNames.end(),
        data2.faceNames.begin(), data2.faceNames.end());

    if (data1.tintindex != -1)
    {
        mergedData.tintindex = data1.tintindex;
        return mergedData;
    }
    else if (data2.tintindex != -1)
    {
        mergedData.tintindex = data2.tintindex;
        return mergedData;
    }else
    {
        mergedData.tintindex = -1;
    }
    
    return mergedData;
}

// 以下方法用于合并两个模型数据，针对流体/水方块模型，对重复面做严格剔除：
// 如果 mesh2 的面被包含在 mesh1 的某个面内，则跳过该面及其对应数据
//
ModelData MergeFluidModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1：顶点处理 ------------------------
    std::unordered_map<VertexKey, int> vertexMap;
    vertexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);
    std::vector<float> uniqueVertices;
    uniqueVertices.reserve(data1.vertices.size() + data2.vertices.size());
    std::vector<int> vertexIndexMap;
    vertexIndexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);

    auto processVertices = [&](const std::vector<float>& vertices) {
        size_t count = vertices.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            float x = vertices[i * 3];
            float y = vertices[i * 3 + 1];
            float z = vertices[i * 3 + 2];
            // 转为整数表示（保留6位小数）
            int rx = static_cast<int>(std::round(x * 1000000.0f));
            int ry = static_cast<int>(std::round(y * 1000000.0f));
            int rz = static_cast<int>(std::round(z * 1000000.0f));
            VertexKey key{ rx, ry, rz };

            auto it = vertexMap.find(key);
            if (it == vertexMap.end()) {
                int newIndex = uniqueVertices.size() / 3;
                uniqueVertices.push_back(x);
                uniqueVertices.push_back(y);
                uniqueVertices.push_back(z);
                vertexMap[key] = newIndex;
                vertexIndexMap.push_back(newIndex);
            }
            else {
                vertexIndexMap.push_back(it->second);
            }
        }
        };

    processVertices(data1.vertices);
    processVertices(data2.vertices);
    mergedData.vertices = std::move(uniqueVertices);

    //------------------------ 阶段2：UV处理 ------------------------
    std::unordered_map<UVKey, int> uvMap;
    uvMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);
    std::vector<float> uniqueUVs;
    uniqueUVs.reserve(data1.uvCoordinates.size() + data2.uvCoordinates.size());
    std::vector<int> uvIndexMap;
    uvIndexMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);

    auto processUVs = [&](const std::vector<float>& uvs) {
        size_t count = uvs.size() / 2;
        for (size_t i = 0; i < count; ++i) {
            float u = uvs[i * 2];
            float v = uvs[i * 2 + 1];
            int ru = static_cast<int>(std::round(u * 1000000.0f));
            int rv = static_cast<int>(std::round(v * 1000000.0f));
            UVKey key{ ru, rv };

            auto it = uvMap.find(key);
            if (it == uvMap.end()) {
                int newIndex = uniqueUVs.size() / 2;
                uniqueUVs.push_back(u);
                uniqueUVs.push_back(v);
                uvMap[key] = newIndex;
                uvIndexMap.push_back(newIndex);
            }
            else {
                uvIndexMap.push_back(it->second);
            }
        }
        };

    processUVs(data1.uvCoordinates);
    processUVs(data2.uvCoordinates);
    mergedData.uvCoordinates = std::move(uniqueUVs);

    //------------------------ 阶段3：材质数据处理 ------------------------
    std::unordered_map<std::string, int> materialMap;
    materialMap.reserve(data1.materialNames.size());
    mergedData.materialNames = data1.materialNames;
    mergedData.texturePaths = data1.texturePaths;
    for (size_t i = 0; i < data1.materialNames.size(); i++) {
        materialMap[data1.materialNames[i]] = static_cast<int>(i);
    }
    std::vector<int> materialIndexMap; // 用于记录 data2 材质索引映射
    materialIndexMap.resize(data2.materialNames.size(), -1);
    for (size_t i = 0; i < data2.materialNames.size(); i++) {
        auto it = materialMap.find(data2.materialNames[i]);
        if (it != materialMap.end()) {
            materialIndexMap[i] = it->second;
        }
        else {
            int newIndex = mergedData.materialNames.size();
            materialMap[data2.materialNames[i]] = newIndex;
            materialIndexMap[i] = newIndex;
            mergedData.materialNames.push_back(data2.materialNames[i]);
            mergedData.texturePaths.push_back(data2.texturePaths[i]);
        }
    }
    // 先拷贝 data1 的面对应的材质索引、面朝向和面名称
    mergedData.materialIndices = data1.materialIndices;
    mergedData.faceDirections = data1.faceDirections;
    mergedData.faceNames = data1.faceNames;

    //------------------------ 阶段4：网格体1面数据处理 ------------------------
    // 直接将 data1 的面（及 UV 面）数据进行重映射后加入 mergedData
    auto remapFaces = [&](const std::vector<int>& faces, bool isData1) {
        size_t faceCount = faces.size() / 4;
        int indexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        for (size_t i = 0; i < faceCount; ++i) {
            for (int j = 0; j < 4; ++j) {
                int originalIndex = faces[i * 4 + j] + indexOffset;
                mergedData.faces.push_back(vertexIndexMap[originalIndex]);
            }
        }
        };

    auto remapUVFaces = [&](const std::vector<int>& uvFaces, bool isData1) {
        size_t faceCount = uvFaces.size() / 4;
        int indexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        for (size_t i = 0; i < faceCount; ++i) {
            for (int j = 0; j < 4; ++j) {
                int originalIndex = uvFaces[i * 4 + j] + indexOffset;
                mergedData.uvFaces.push_back(uvIndexMap[originalIndex]);
            }
        }
        };

    // mesh1 的面数据直接加入
    remapFaces(data1.faces, true);
    remapUVFaces(data1.uvFaces, true);
    int mesh1FaceCount = data1.faces.size() / 4; // mesh1 面数量

    //------------------------ 阶段5：辅助几何检测函数 ------------------------

    // 计算面（四边形）的法向，使用前三个顶点
    auto computeNormal = [&](const std::vector<float>& vertices, const std::vector<int>& faceIndices) -> std::array<float, 3> {
        float x0 = vertices[faceIndices[0] * 3];
        float y0 = vertices[faceIndices[0] * 3 + 1];
        float z0 = vertices[faceIndices[0] * 3 + 2];
        float x1 = vertices[faceIndices[1] * 3];
        float y1 = vertices[faceIndices[1] * 3 + 1];
        float z1 = vertices[faceIndices[1] * 3 + 2];
        float x2 = vertices[faceIndices[2] * 3];
        float y2 = vertices[faceIndices[2] * 3 + 1];
        float z2 = vertices[faceIndices[2] * 3 + 2];
        float ux = x1 - x0, uy = y1 - y0, uz = z1 - z0;
        float vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
        std::array<float, 3> normal = { uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
        float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (len > 1e-6f) {
            normal[0] /= len; normal[1] /= len; normal[2] /= len;
        }
        return normal;
        };

    // 根据法向判断投影时丢弃哪个坐标轴：返回 0 表示丢弃 x，1 表示丢弃 y，2 表示丢弃 z
    auto determineDropAxis = [&](const std::array<float, 3>& normal) -> int {
        float absX = std::fabs(normal[0]);
        float absY = std::fabs(normal[1]);
        float absZ = std::fabs(normal[2]);
        if (absX >= absY && absX >= absZ)
            return 0;
        else if (absY >= absX && absY >= absZ)
            return 1;
        else
            return 2;
        };

    // 将某个顶点投影到二维平面，dropAxis 指定丢弃的坐标
    auto projectPoint = [&](const std::vector<float>& vertices, int vertexIndex, int dropAxis) -> std::pair<float, float> {
        float x = vertices[vertexIndex * 3];
        float y = vertices[vertexIndex * 3 + 1];
        float z = vertices[vertexIndex * 3 + 2];
        if (dropAxis == 0) return { y, z };
        else if (dropAxis == 1) return { x, z };
        else return { x, y };
        };

    // 判断二维点 p 是否在三角形 (a, b, c) 内（采用重心坐标法）
    auto pointInTriangle = [&](const std::pair<float, float>& p,
        const std::pair<float, float>& a,
        const std::pair<float, float>& b,
        const std::pair<float, float>& c) -> bool {
            float denom = (b.second - c.second) * (a.first - c.first) + (c.first - b.first) * (a.second - c.second);
            if (std::fabs(denom) < 1e-6f) return false;
            float alpha = ((b.second - c.second) * (p.first - c.first) + (c.first - b.first) * (p.second - c.second)) / denom;
            float beta = ((c.second - a.second) * (p.first - c.first) + (a.first - c.first) * (p.second - c.second)) / denom;
            float gamma = 1.0f - alpha - beta;
            return (alpha >= 0.0f && beta >= 0.0f && gamma >= 0.0f);
        };

    // 判断二维点 p 是否在四边形内（将四边形分解为两个三角形）
    auto pointInQuad = [&](const std::vector<int>& quadIndices, const std::vector<float>& vertices, const std::pair<float, float>& p, int dropAxis) -> bool {
        std::vector<std::pair<float, float>> proj;
        for (int idx : quadIndices) {
            proj.push_back(projectPoint(vertices, idx, dropAxis));
        }
        if (pointInTriangle(p, proj[0], proj[1], proj[2])) return true;
        if (pointInTriangle(p, proj[0], proj[2], proj[3])) return true;
        return false;
        };

    // 判断 mesh2 的某个面（给定其4个顶点索引，均为 mergedData.vertices 的下标）是否被包含在 mesh1 的任一面内
    auto isFaceContained = [&](const std::vector<int>& faceIndices2) -> bool {
        // 遍历所有 mesh1 的面（已在 mergedData.faces 中，前 mesh1FaceCount 个面）
        for (size_t i = 0; i < static_cast<size_t>(mesh1FaceCount); i++) {
            std::vector<int> faceIndices1(4);
            for (int j = 0; j < 4; j++) {
                faceIndices1[j] = mergedData.faces[i * 4 + j];
            }
            auto normal = computeNormal(mergedData.vertices, faceIndices1);
            int dropAxis = determineDropAxis(normal);
            bool allInside = true;
            for (int idx : faceIndices2) {
                auto p = projectPoint(mergedData.vertices, idx, dropAxis);
                if (!pointInQuad(faceIndices1, mergedData.vertices, p, dropAxis)) {
                    allInside = false;
                    break;
                }
            }
            if (allInside)
                return true;
        }
        return false;
        };

    //------------------------ 阶段6：网格体2面数据处理（严格检查重复面） ------------------------
    int data2FaceCount = data2.faces.size() / 4;
    int data2VertexOffset = data1.vertices.size() / 3;
    int data2UVOffset = data1.uvCoordinates.size() / 2;
    for (size_t i = 0; i < static_cast<size_t>(data2FaceCount); i++) {
        // 重映射 mesh2 面的顶点索引
        std::vector<int> faceIndices(4);
        for (int j = 0; j < 4; j++) {
            int originalIndex = data2.faces[i * 4 + j] + data2VertexOffset;
            faceIndices[j] = vertexIndexMap[originalIndex];
        }

        // 获取 data2 对应面的 faceDirections（假设每个面有4个方向信息，索引为 i*4 ~ i*4+3）
        bool doNotCull = false;
        if (data2.faceDirections.size() >= (i + 1) * 4) {
            for (int k = 0; k < 4; k++) {
                if (data2.faceDirections[i * 4 + k] == "DO_NOT_CULL") {
                    doNotCull = true;
                    break;
                }
            }
        }

        // 如果不标记 DO_NOT_CULL 且该面完全被包含在 mesh1 的某个面内，则跳过该面
        if (!doNotCull && isFaceContained(faceIndices))
            continue;

        // 添加该面到合并结果
        mergedData.faces.insert(mergedData.faces.end(), faceIndices.begin(), faceIndices.end());

        // 对应的UV面处理
        std::vector<int> uvFaceIndices(4);
        for (int j = 0; j < 4; j++) {
            int originalUVIndex = data2.uvFaces[i * 4 + j] + data2UVOffset;
            uvFaceIndices[j] = uvIndexMap[originalUVIndex];
        }
        mergedData.uvFaces.insert(mergedData.uvFaces.end(), uvFaceIndices.begin(), uvFaceIndices.end());

        // 材质索引（通过映射处理）
        int originalMatIndex = data2.materialIndices[i];
        mergedData.materialIndices.push_back((originalMatIndex != -1) ? materialIndexMap[originalMatIndex] : -1);

        // faceDirections 4个一组代表一个面，将 data2 对应的4个方向信息拷贝到合并结果中
        if (data2.faceDirections.size() >= (i + 1) * 4) {
            for (int k = 0; k < 4; k++) {
                mergedData.faceDirections.push_back(data2.faceDirections[i * 4 + k]);
            }
        }
        
    }

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



