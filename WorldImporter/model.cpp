#include "model.h"
#include "fileutils.h"
#include "SpecialBlock.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
#include <omp.h>
#include <chrono>
#include <span>

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
// 应用缩放(以0.5,0.5,0.5为中心)
void ApplyScaleToVertices(std::span<float> vertices, float sx, float sy, float sz) {
    constexpr float center = 0.5f;
    for (size_t i = 0; i < vertices.size(); i += 3) {
        // 平移至中心点相对坐标
        float x = vertices[i] - center;
        float y = vertices[i + 1] - center;
        float z = vertices[i + 2] - center;

        // 应用缩放
        x *= sx;
        y *= sy;
        z *= sz;

        // 平移回原坐标系
        vertices[i] = x + center;
        vertices[i + 1] = y + center;
        vertices[i + 2] = z + center;
    }
}
// 应用旋转(以0.5,0.5,0.5为中心,按X->Y->Z轴顺序)
void ApplyRotationToVertices(std::span<float> vertices, float rx, float ry, float rz) {
    constexpr float center = 0.5f;

    // 转换角度为弧度(按原始值直接使用,若需/16则改为 rx/16.0f)
    const float radX = rx * (M_PI / 180.0f);
    const float radY = ry * (M_PI / 180.0f);
    const float radZ = rz * (M_PI / 180.0f);

    // 预计算三角函数值
    const float cosX = cos(radX), sinX = sin(radX);
    const float cosY = cos(radY), sinY = sin(radY);
    const float cosZ = cos(radZ), sinZ = sin(radZ);

    for (size_t i = 0; i < vertices.size(); i += 3) {
        // 平移至中心点相对坐标
        float x = vertices[i] - center;
        float y = vertices[i + 1] - center;
        float z = vertices[i + 2] - center;

        // 按X轴旋转(与element处理逻辑一致)
        if (rx != 0.0f) {
            float new_y = y * cosX - z * sinX;
            float new_z = y * sinX + z * cosX;
            y = new_y;
            z = new_z;
        }

        // 按Y轴旋转(与element处理逻辑一致)
        if (ry != 0.0f) {
            float new_x = x * cosY + z * sinY;
            float new_z = -x * sinY + z * cosY;
            x = new_x;
            z = new_z;
        }

        // 按Z轴旋转(与element处理逻辑一致)
        if (rz != 0.0f) {
            float new_x = x * cosZ - y * sinZ;
            float new_y = x * sinZ + y * cosZ;
            x = new_x;
            y = new_y;
        }

        // 平移回原坐标系
        vertices[i] = x + center;
        vertices[i + 1] = y + center;
        vertices[i + 2] = z + center;
    }
}


// 旋转函数 - 使用整数参数的版本,C++20版本
void ApplyRotationToVertices(std::span<float> vertices, int rotationX, int rotationY) {
    // 参数校验
    if (vertices.size() % 3 != 0) {
        throw std::invalid_argument("Invalid vertex data size");
    }

    // 绕 X 轴旋转(90度增量)
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

    // 绕 Y 轴旋转(90度增量)
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

// 带旋转中心的UV旋转(内联优化)
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

// 预计算三角函数值(包含常见角度优化)
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

// 优化后的UV分离,使用Face结构体中的uvIndices
static void createUniqueUVs(ModelData& modelData) {
    std::vector<float> newUVs;
    const size_t faceCount = modelData.faces.size();
    const size_t vertexCount = faceCount * 4; // 每个面4个点
    
    newUVs.reserve(vertexCount * 2); // 每个点2个UV坐标
    
    // 遍历每个Face,提取并重建UV坐标,更新uvIndices
    for (size_t fi = 0; fi < faceCount; ++fi) {
        Face& face = modelData.faces[fi];
        for (int j = 0; j < 4; ++j) {
            int oldIdx = face.uvIndices[j];
            // 边界检查
            if (oldIdx >= 0 && oldIdx*2+1 < modelData.uvCoordinates.size()) {
                newUVs.push_back(modelData.uvCoordinates[oldIdx * 2]);
                newUVs.push_back(modelData.uvCoordinates[oldIdx * 2 + 1]);
                face.uvIndices[j] = static_cast<int>(newUVs.size()/2 - 1); // 更新为新索引
            } else {
                // 无效索引处理,使用默认UV值
                newUVs.push_back(0.0f);
                newUVs.push_back(0.0f);
                face.uvIndices[j] = static_cast<int>(newUVs.size()/2 - 1);
            }
        }
    }
    
    modelData.uvCoordinates = std::move(newUVs);
}

// 应用旋转到单个Face的UV(批量处理优化)
static void applyFaceRotation(ModelData& modelData, size_t faceIdx, int angle) {
    if (angle == 0 || faceIdx >= modelData.faces.size()) return;
    
    float cosA, sinA;
    getCosSin(angle, cosA, sinA);
    if (cosA == 1.0f && sinA == 0.0f) return;
    
    Face& f = modelData.faces[faceIdx];
    for (int j = 0; j < 4; ++j) {
        int uvIdx = f.uvIndices[j];
        // 边界检查
        if (uvIdx >= 0 && uvIdx*2+1 < modelData.uvCoordinates.size()) {
            float& u = modelData.uvCoordinates[uvIdx * 2];
            float& v = modelData.uvCoordinates[uvIdx * 2 + 1];
            fastRotateUV(u, v, cosA, sinA);
        }
    }
}

// 主逻辑优化(预处理面类型+并行处理)
void ApplyRotationToUV(ModelData& modelData, int rotationX, int rotationY) {
    createUniqueUVs(modelData);
    
    // 预处理面类型
    std::vector<FaceType> faceTypes;
    size_t faceCount = modelData.faces.size(); // 每个 Face 结构体代表一个完整的面
    faceTypes.reserve(faceCount);
    
    for (size_t i = 0; i < faceCount; ++i) {
        FaceType faceType = modelData.faces[i].faceDirection;
        faceTypes.push_back(faceType);
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
void ApplyRotationToFaceDirections(std::vector<Face>& faces, int rotationX, int rotationY) {
    // 定义旋转规则
    auto rotateY = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::EAST;
            case FaceType::EAST: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::WEST;
            case FaceType::WEST: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateYReverse = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::WEST;
            case FaceType::WEST: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::EAST;
            case FaceType::EAST: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateX = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::UP;
            case FaceType::UP: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::DOWN;
            case FaceType::DOWN: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateXReverse = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::DOWN;
            case FaceType::DOWN: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::UP;
            case FaceType::UP: return FaceType::NORTH;
            default: return direction;
        }
    };

    // 对每个面的方向应用旋转
    for (Face& face : faces) {
        // 不旋转 DO_NOT_CULL
        if (face.faceDirection == FaceType::DO_NOT_CULL) continue;
        
        // 绕 X 轴旋转(90度增量)
        switch (rotationX) {
        case 270: face.faceDirection = rotateX(face.faceDirection); break;
        case 180: face.faceDirection = rotateX(rotateX(face.faceDirection)); break;
        case 90: face.faceDirection = rotateXReverse(face.faceDirection); break;
        }
        
        // 绕 Y 轴旋转(90度增量)
        switch (rotationY) {
        case 90: face.faceDirection = rotateY(face.faceDirection); break;
        case 180: face.faceDirection = rotateY(rotateY(face.faceDirection)); break;
        case 270: face.faceDirection = rotateYReverse(face.faceDirection); break;
        }
    }
}

void ApplyPositionOffset(ModelData& model, int x, int y, int z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
    }
}

void ApplyDoublePositionOffset(ModelData& model, double x, double y, double z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
    }
}

//============== 模型数据处理模块 ==============//
//---------------- JSON处理 ----------------
nlohmann::json LoadParentModel(const std::string& namespaceName, const std::string& blockId, nlohmann::json& currentModelJson) {
    // 如果当前模型没有 parent 属性,直接返回
    if (!currentModelJson.contains("parent")) {
        return currentModelJson;
    }

    // 获取当前模型的 parent
    std::string parentModelId = currentModelJson["parent"];

    // 判断 parentModelId 是否包含冒号(即是否包含命名空间)
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

            // 如果父模型没有 parent 属性,停止递归
            if (!parentModelJson.contains("parent")) {
                return currentModelJson;
            }

            // 递归加载父模型的父模型
            return LoadParentModel(parentNamespace, parentModelId, currentModelJson);
        }
    }

    // 缓存未命中,加载父模型
    nlohmann::json parentModelJson = GetModelJson(parentNamespace, parentModelId);

    // 如果父模型不存在,直接返回当前模型
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

    // 如果父模型没有 parent 属性,停止递归
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
                // 处理变量引用(如 #texture)
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

            // ---- START MODIFIED CODE ----
            // 检查 pathPart 是否有问题 (例如, 空, 以'/'结尾), 或者原始 textureKey 是否为 "missing"
            if (textureKey == "missing" || pathPart.empty() || pathPart.back() == '/') {
                std::string placeholderMaterialName = namespaceName + ":" + pathPart + (textureKey == "missing" ? "missing_placeholder" : "empty_path_placeholder");
                
                // 使用原始 textureKey 进行映射,确保唯一性
                std::string uniqueMaterialKeyForMap = textureKey; 

                if (processedMaterials.find(placeholderMaterialName) == processedMaterials.end()) {
                    Material newMaterial;
                    newMaterial.name = placeholderMaterialName;
                    newMaterial.texturePath = ""; // 空路径表示缺失纹理
                    newMaterial.tintIndex = -1;
                    newMaterial.type = NORMAL;
                    
                    int materialIndex = data.materials.size();
                    data.materials.push_back(newMaterial);
                    processedMaterials[placeholderMaterialName] = materialIndex;
                    textureKeyToMaterialIndex[uniqueMaterialKeyForMap] = materialIndex;
                } else {
                    textureKeyToMaterialIndex[uniqueMaterialKeyForMap] = processedMaterials[placeholderMaterialName];
                }
                continue; // 跳过对此纹理条目的常规处理
            }
            // ---- END MODIFIED CODE ----

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
                Material newMaterial;
                newMaterial.name = fullMaterialName;
                newMaterial.texturePath = textureSavePath;
                newMaterial.tintIndex = -1;  // 默认值
                
                // 检测材质类型(是否为动态材质或CTM)
                newMaterial.type = DetectMaterialType(namespaceName, pathPart);
                
                int materialIndex = data.materials.size();
                data.materials.push_back(newMaterial);
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
    short tintindex = -1;
    std::unordered_map<std::string, int> faceCountMap; // 面计数映射

    auto elements = modelJson["elements"];

    for (const auto& element : elements) {
        if (element.contains("from") && element.contains("to") && element.contains("faces")) {
            auto from = element["from"];
            auto to = element["to"];
            auto faces = element["faces"];


            // 转换原始坐标为 OBJ 坐标系(/16)
            float x1 = from[0].get<float>() / 16.0f;
            float y1 = from[1].get<float>() / 16.0f;
            float z1 = from[2].get<float>() / 16.0f;
            float x2 = to[0].get<float>() / 16.0f;
            float y2 = to[1].get<float>() / 16.0f;
            float z2 = to[2].get<float>() / 16.0f;
            
            // 检测是否为"极薄"块
            constexpr float THIN_THRESHOLD = 0.01f; // 1/100 方块单位的阈值
            bool isThinX = std::abs(x2 - x1) < THIN_THRESHOLD;
            bool isThinY = std::abs(y2 - y1) < THIN_THRESHOLD;
            bool isThinZ = std::abs(z2 - z1) < THIN_THRESHOLD;
            
            // 生成基础顶点数据
            std::unordered_map<std::string, std::vector<std::vector<float>>> elementVertices;
            
            // 遍历元素的面,动态生成顶点数据
            for (auto& face : faces.items()) { // 'faces' is from element["faces"]
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
                    elementVertices[faceName] = { {x2, y2, z2}, {x2, y2, z1} ,{x1, y2, z1}, {x1, y2, z2}  };
                }
                else if (faceName == "down") {
                    elementVertices[faceName] = {  {x1, y1, z2}, {x1, y1, z1}, {x2, y1, z1},{ x2, y1, z2 }};
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
                // 在旋转处理部分的缩放逻辑修改如下:
                bool rescale = rotation.value("rescale", false);
                if (rescale) {
                    // 将原始角度转回度数进行比较
                    float angle_deg_conv = angle_rad * 180.0f / M_PI;
                    bool applyScaling = false;
                    float scale = 1.0f;

                    // 检查是否为22.5°或45°的整数倍(考虑浮点精度)
                    if (std::fabs(angle_deg_conv - 22.5f) < 1e-6 || std::fabs(angle_deg_conv + 22.5f) < 1e-6) {
                        applyScaling = true;
                        scale = std::sqrt(2.0f - std::sqrt(2.0f)); // 22.5°对应的缩放因子
                    }
                    else if (std::fabs(angle_deg_conv - 45.0f) < 1e-6 || std::fabs(angle_deg_conv + 45.0f) < 1e-6) {
                        applyScaling = true;
                        scale = std::sqrt(2.0f);           // 45°对应的缩放因子
                    }

                    if (applyScaling) {
                        // 根据旋转轴应用缩放,保留原有旋转中心偏移逻辑
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

            // --- 新增:检测并移除相反方向的重叠面 ---
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

            // 用于记录需要覆盖 uv 的信息
            std::vector<std::string> facesToRemove;
            // 创建映射,记录哪个面被哪个面替代,键是被移除的面名称,值是保留的面名称
            std::unordered_map<std::string, std::string> faceReplacementMap;
            
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
                            faceReplacementMap[faceName] = opposite; // 记录面替换关系
                        }
                        else {
                            facesToRemove.push_back(opposite);
                            faceReplacementMap[opposite] = faceName; // 记录面替换关系
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

            // 遍历每个面的数据,判断面是否存在,如果存在则处理
            for (auto& face : faces.items()) {
                std::string faceName = face.key();
                
                if (elementVertices.find(faceName) != elementVertices.end()) {
                    // 处理当前面
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

                        // 将法线方向四舍五入到两位小数
                        crossX = std::round(crossX * 100.0f) / 100.0f;
                        crossY = std::round(crossY * 100.0f) / 100.0f;
                        crossZ = std::round(crossZ * 100.0f) / 100.0f;

                        // 生成面的几何特征指纹
                        // 使用所有顶点坐标而不仅仅是中心点
                        std::stringstream vertexStream;
                        vertexStream << std::fixed << std::setprecision(4);
                        
                        // 按照几何坐标排序,以确保相同的面(即使顶点顺序不同)有相同的键
                        std::vector<std::string> vertexKeys;
                        for (const auto& v : faceVertices) {
                            std::stringstream vs;
                            vs << std::fixed << std::setprecision(4)
                               << v[0] << "," << v[1] << "," << v[2];
                            vertexKeys.push_back(vs.str());
                        }
                        std::sort(vertexKeys.begin(), vertexKeys.end());
                        
                        // 创建基于所有顶点的键
                        std::string vertexFingerprint;
                        for (const auto& vk : vertexKeys) {
                            if (!vertexFingerprint.empty()) {
                                vertexFingerprint += "|";
                            }
                            vertexFingerprint += vk;
                        }
                        
                        // 组合法线和顶点指纹作为面的唯一标识
                        std::stringstream keyStream;
                        keyStream << std::fixed << std::setprecision(2)
                            << crossX << "," << crossY << "," << crossZ << "_"
                            << vertexFingerprint;
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

                    Face newFace;
                    newFace.vertexIndices = vertexIndices;
                    newFace.materialIndex = -1;
                    newFace.faceDirection = StringToFaceType("DO_NOT_CULL");
                    data.faces.push_back(newFace);

                    if (face.value().contains("texture")) {
                        std::string texture = face.value()["texture"];
                        if (texture.front() == '#') texture.erase(0, 1);

                        auto it = textureKeyToMaterialIndex.find(texture);
                        if (it != textureKeyToMaterialIndex.end()) {
                            data.faces.back().materialIndex = it->second;
                        }
                        
                        // UV数据处理
                        nlohmann::json faceData = face.value();
                        
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
                        if (faceData.contains("uv")) {
                            auto uv = faceData["uv"];
                           
                            uvRegion = {
                                uv[0].get<float>(),
                                uv[1].get<float>(),
                                uv[2].get<float>(),
                                uv[3].get<float>()
                            };
                        }
                  
                        std::array<int, 4> uvIndices;
                        
                        // 检测UV区域是否有镜像翻转
                        bool flipX = uvRegion[0] > uvRegion[2]; // X方向镜像
                        bool flipY = uvRegion[1] > uvRegion[3]; // Y方向镜像
                        
                        // 确保UV坐标范围正确(起点小于终点)
                        if (flipX) {
                            std::swap(uvRegion[0], uvRegion[2]);
                        }
                        if (flipY) {
                            std::swap(uvRegion[1], uvRegion[3]);
                        }
                        
                        // 计算四个 UV 坐标点(左下,左上,右上,右下)
                        std::vector<std::vector<float>> uvCoords = {
                            {uvRegion[2] / 16.0f, 1 - uvRegion[3] / 16.0f},
                            {uvRegion[2] / 16.0f, 1 - uvRegion[1] / 16.0f},
                            {uvRegion[0] / 16.0f, 1 - uvRegion[1] / 16.0f},
                            {uvRegion[0] / 16.0f, 1 - uvRegion[3] / 16.0f}
                        };

                        
                        // 应用镜像翻转(如果需要)
                        if (flipX) {
                            // 水平镜像:交换左右顶点
                            std::swap(uvCoords[0], uvCoords[3]); // 交换左下和右下
                            std::swap(uvCoords[1], uvCoords[2]); // 交换左上和右上
                        }
                        if (flipY) {
                            // 垂直镜像:交换上下顶点
                            std::swap(uvCoords[0], uvCoords[1]); // 交换左下和左上
                            std::swap(uvCoords[3], uvCoords[2]); // 交换右下和右上
                        }

                        // 获取旋转值
                        int rotation = faceData.value("rotation", 0);                        
                        int steps = ((rotation % 360) + 360) % 360 / 90;

                        if (steps != 0) {
                            std::vector<std::vector<float>> rotatedUV(4);
                            for (int i = 0; i < 4; i++) {
                                rotatedUV[i] = uvCoords[(i - steps + 4) % 4];
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

                        data.faces.back().uvIndices = uvIndices;
                    }

                    std::string faceDirection;
                    if (face.value().contains("cullface")) {
                        faceDirection = face.value()["cullface"].get<std::string>();
                    }
                    else {
                        faceDirection = "DO_NOT_CULL";
                    }
                    
                    short localTintIndex = -1;
                    if (face.value().contains("tintindex")) {
                        localTintIndex = face.value()["tintindex"].get<int>();
                    }
                    // 更新此材质的tintIndex
                    if (!data.materials.empty() && data.faces.back().materialIndex >= 0 && data.faces.back().materialIndex < data.materials.size()) {
                        data.materials[data.faces.back().materialIndex].tintIndex = localTintIndex;
                    }
                    
                    // 将字符串方向转换为枚举类型并添加到faceDirections
                    data.faces.back().faceDirection = StringToFaceType(faceDirection);
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
        // 当模型中没有 "elements" 字段时,生成实体方块模型
        data = SpecialBlock::GenerateSpecialBlockModel(blockName);
    }
    

    return data;
}

// 将model类型的json文件变为网格数据
ModelData ProcessModelJson(const std::string& namespaceName, const std::string& blockId,
    int rotationX, int rotationY, bool uvlock, int randomIndex,const std::string& blockstateName) {
    // 生成唯一缓存键(添加模型索引)
    std::string cacheKey = namespaceName + ":" + blockId + ":" + std::to_string(randomIndex);

    // 在访问缓存前加锁
    std::lock_guard<std::mutex> lock(cacheMutex);
    // 检查缓存是否存在
    auto cacheIt = modelCache.find(cacheKey);
    if (cacheIt != modelCache.end()) {
        // 从缓存中获取原始模型数据
        ModelData cachedModel = cacheIt->second;
        if (rotationX != 0 || rotationY != 0) {
            // 使用C++20的span进行函数调用
            ApplyRotationToVertices(std::span<float>(cachedModel.vertices.data(), cachedModel.vertices.size()), rotationX, rotationY);
        }
        if (uvlock)
        {
            ApplyRotationToUV(cachedModel, rotationX, rotationY);
        }
        // 施加旋转到 faceDirections
        ApplyRotationToFaceDirections(cachedModel.faces, rotationX, rotationY);
        return cachedModel;
    }
    // 缓存未命中,正常加载模型
    nlohmann::json modelJson = GetModelJson(namespaceName, blockId);

    ModelData modelData;
    if (modelJson.is_null()) {
        return modelData;
    }
    // 递归加载父模型并合并属性
    modelJson = LoadParentModel(namespaceName, blockId, modelJson);
    
    // 处理模型数据(不包含旋转)
    modelData = ProcessModelData(modelJson,blockstateName);


    // 将原始数据存入缓存(不包含旋转)
    modelCache[cacheKey] = modelData;

    if (rotationX != 0 || rotationY != 0) {
        // 如果指定了旋转,则应用旋转
        ApplyRotationToVertices(std::span<float>(modelData.vertices.data(), modelData.vertices.size()), rotationX, rotationY);
    }
    if (uvlock)
    {
        ApplyRotationToUV(modelData, rotationX, rotationY);
    }

    // 施加旋转到 faceDirections
    ApplyRotationToFaceDirections(modelData.faces, rotationX, rotationY);
    return modelData;
}


//——————————————合并网格体方法———————————————

ModelData MergeModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1:顶点处理 ------------------------
    std::unordered_map<VertexKey, int> vertexMap;
    // 预估合并后顶点数量,避免重复 rehash
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
            // 转为整数表示(保留6位小数)
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

    //------------------------ 阶段2:UV处理 ------------------------
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

    //------------------------ 阶段3:面数据处理 ------------------------
    // 先处理材质映射,以便在处理面时使用
    std::vector<int> materialIndexMap;
    {
        // 使用哈希映射快速判断 data1 中已存在的材质
        std::unordered_map<std::string, int> materialMap;
        materialMap.reserve(data1.materials.size());
        mergedData.materials = data1.materials;

        for (size_t i = 0; i < data1.materials.size(); i++) {
            materialMap[data1.materials[i].name] = static_cast<int>(i);
        }

        materialIndexMap.resize(data2.materials.size(), -1);
        for (size_t i = 0; i < data2.materials.size(); ++i) {
            auto it = materialMap.find(data2.materials[i].name);
            if (it != materialMap.end()) {
                materialIndexMap[i] = it->second;
            }
            else {
                int newIndex = mergedData.materials.size();
                materialMap[data2.materials[i].name] = newIndex;
                materialIndexMap[i] = newIndex;
                mergedData.materials.push_back(data2.materials[i]);
            }
        }
    }

    // 面和 UV 面数据合并时直接使用映射后的索引
    auto remapFaces = [&](const std::vector<Face>& faces, bool isData1) {
        // data2 的顶点需要加上 data1 原始顶点数偏移
        int vertexIndexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        int uvIndexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        
        for (const auto& face : faces) {
            Face newFace;
            
            // 重映射顶点索引
            for (int j = 0; j < 4; ++j) {
                int originalVertIndex = face.vertexIndices[j];
                int globalVertIndex = originalVertIndex + (isData1 ? 0 : vertexIndexOffset);
                if (globalVertIndex >= 0 && globalVertIndex < vertexIndexMap.size()) {
                    newFace.vertexIndices[j] = vertexIndexMap[globalVertIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.vertexIndices[j] = 0;
                }
            }
            
            // 重映射UV索引
            for (int j = 0; j < 4; ++j) {
                int originalUVIndex = face.uvIndices[j];
                int globalUVIndex = originalUVIndex + (isData1 ? 0 : uvIndexOffset);
                if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                    newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.uvIndices[j] = 0;
                }
            }
            
            // 重映射材质索引
            if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                newFace.materialIndex = materialIndexMap[face.materialIndex];
            } else {
                newFace.materialIndex = face.materialIndex;
            }
            
            // 保留面方向
            newFace.faceDirection = face.faceDirection;
            
            mergedData.faces.push_back(newFace);
        }
    };

    remapFaces(data1.faces, true);
    remapFaces(data2.faces, false);

    //------------------------ 阶段4:材质数据合并 ------------------------
    // 材质数据已在上面处理完成

    //------------------------ 阶段5:合并其他数据 ------------------------
    // 合并处理tintIndex
    // 注意:这里使用新的方式处理tint索引,我们需要遍历所有材质检查tintIndex
    bool hasTintIndex1 = false;
    for (const auto& material : data1.materials)
    {
        if (material.tintIndex != -1)
        {
            hasTintIndex1 = true;
            break;
        }
    }
    // 如果data1没有tint索引,检查data2
    if (!hasTintIndex1)
    {
        for (size_t i = 0; i < data2.materials.size(); ++i)
        {
            if (data2.materials[i].tintIndex != -1 && i < materialIndexMap.size())
            {
                int targetIndex = materialIndexMap[i];
                if (targetIndex >= 0 && targetIndex < mergedData.materials.size())
                {
                    mergedData.materials[targetIndex].tintIndex = data2.materials[i].tintIndex;
                }
            }
        }
    }

    return mergedData;
}

// 以下方法用于合并两个模型数据,针对流体/水方块模型,对重复面做严格剔除:
// 如果 mesh2 的面被包含在 mesh1 的某个面内,则跳过该面及其对应数据
ModelData MergeFluidModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1:顶点处理 ------------------------
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
            // 转为整数表示(保留6位小数)
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

    //------------------------ 阶段2:UV处理 ------------------------
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

    //------------------------ 阶段3:材质数据处理 ------------------------
    std::unordered_map<std::string, int> materialMap;
    materialMap.reserve(data1.materials.size());
    mergedData.materials = data1.materials;
    for (size_t i = 0; i < data1.materials.size(); i++) {
        materialMap[data1.materials[i].name] = static_cast<int>(i);
    }
    std::vector<int> materialIndexMap; // 用于记录 data2 材质索引映射
    materialIndexMap.resize(data2.materials.size(), -1);
    for (size_t i = 0; i < data2.materials.size(); i++) {
        auto it = materialMap.find(data2.materials[i].name);
        if (it != materialMap.end()) {
            materialIndexMap[i] = it->second;
        }
        else {
            int newIndex = mergedData.materials.size();
            materialMap[data2.materials[i].name] = newIndex;
            materialIndexMap[i] = newIndex;
            mergedData.materials.push_back(data2.materials[i]);
        }
    }

    //------------------------ 阶段4:网格体1面数据处理 ------------------------
    // 直接将 data1 的面(及 UV 面)数据进行重映射后加入 mergedData
    auto remapFaces = [&](const std::vector<Face>& faces, bool isData1) {
        // data2 的顶点需要加上 data1 原始顶点数偏移
        int vertexIndexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        int uvIndexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        
        for (const auto& face : faces) {
            Face newFace;
            
            // 重映射顶点索引
            for (int j = 0; j < 4; ++j) {
                int originalVertIndex = face.vertexIndices[j];
                int globalVertIndex = originalVertIndex + (isData1 ? 0 : vertexIndexOffset);
                if (globalVertIndex >= 0 && globalVertIndex < vertexIndexMap.size()) {
                    newFace.vertexIndices[j] = vertexIndexMap[globalVertIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.vertexIndices[j] = 0;
                }
            }
            
            // 重映射UV索引
            for (int j = 0; j < 4; ++j) {
                int originalUVIndex = face.uvIndices[j];
                int globalUVIndex = originalUVIndex + (isData1 ? 0 : uvIndexOffset);
                if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                    newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.uvIndices[j] = 0;
                }
            }
            
            // 重映射材质索引
            if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                newFace.materialIndex = materialIndexMap[face.materialIndex];
            } else {
                newFace.materialIndex = face.materialIndex;
            }
            
            // 保留面方向
            newFace.faceDirection = face.faceDirection;
            
            mergedData.faces.push_back(newFace);
        }
    };

    auto remapUVFaces = [&](const std::vector<Face>& faces, bool isData1) {
        int indexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const auto& face = faces[faceIndex];
            // 获取对应的新面索引
            size_t newFaceIndex = isData1 ? faceIndex : (data1.faces.size() + faceIndex);
            
            // 确保新面索引在合法范围内
            if (newFaceIndex < mergedData.faces.size()) {
                Face& newFace = mergedData.faces[newFaceIndex];
                
                // 重映射UV索引
                for (int j = 0; j < 4; ++j) {
                    int originalUVIndex = face.uvIndices[j];
                    int globalUVIndex = originalUVIndex + (isData1 ? 0 : indexOffset);
                    
                    // 边界检查
                    if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                        newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                    } else {
                        // 处理无效索引
                        newFace.uvIndices[j] = 0;
                    }
                }
                
                // 重映射材质索引
                if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                    newFace.materialIndex = materialIndexMap[face.materialIndex];
                } else {
                    newFace.materialIndex = face.materialIndex;
                }
                
                // 保留面方向
                newFace.faceDirection = face.faceDirection;
            }
        }
    };

    // mesh1 的面数据直接加入
    remapFaces(data1.faces, true);
    remapUVFaces(data1.faces, true);
    int mesh1FaceCount = data1.faces.size(); // mesh1 面数量

    //------------------------ 阶段5:辅助几何检测函数 ------------------------

    // 计算面(四边形)的法向,使用前三个顶点
    auto computeNormal = [&](const std::vector<float>& vertices, const std::array<int, 4>& faceIndices) -> std::array<float, 3> {
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

    // 根据法向判断投影时丢弃哪个坐标轴:返回 0 表示丢弃 x,1 表示丢弃 y,2 表示丢弃 z
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

    // 将某个顶点投影到二维平面,dropAxis 指定丢弃的坐标
    auto projectPoint = [&](const std::vector<float>& vertices, int vertexIndex, int dropAxis) -> std::pair<float, float> {
        float x = vertices[vertexIndex * 3];
        float y = vertices[vertexIndex * 3 + 1];
        float z = vertices[vertexIndex * 3 + 2];
        if (dropAxis == 0) return { y, z };
        else if (dropAxis == 1) return { x, z };
        else return { x, y };
        };

    // 判断二维点 p 是否在三角形 (a, b, c) 内(采用重心坐标法)
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

    // 判断二维点 p 是否在四边形内(将四边形分解为两个三角形)
    auto pointInQuad = [&](const std::array<int, 4>& quadIndices, const std::vector<float>& vertices, const std::pair<float, float>& p, int dropAxis) -> bool {
        std::vector<std::pair<float, float>> proj;
        for (int idx : quadIndices) {
            proj.push_back(projectPoint(vertices, idx, dropAxis));
        }
        if (pointInTriangle(p, proj[0], proj[1], proj[2])) return true;
        if (pointInTriangle(p, proj[0], proj[2], proj[3])) return true;
        return false;
        };

    // 判断 mesh2 的某个面(给定其4个顶点索引,均为 mergedData.vertices 的下标)是否被包含在 mesh1 的任一面内
    auto isFaceContained = [&](const std::array<int, 4>& faceIndices2) -> bool {
        // 遍历所有 mesh1 的面(已在 mergedData.faces 中,前 mesh1FaceCount 个面)
        for (size_t i = 0; i < static_cast<size_t>(mesh1FaceCount); i++) {
            const auto& faceIndices1 = mergedData.faces[i].vertexIndices;
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

    //------------------------ 阶段6:网格体2面数据处理(严格检查重复面) ------------------------
    int data2FaceCount = data2.faces.size();
    int data2VertexOffset = data1.vertices.size() / 3;
    int data2UVOffset = data1.uvCoordinates.size() / 2;
    for (size_t i = 0; i < static_cast<size_t>(data2FaceCount); i++) {
        // 重映射 mesh2 面的顶点索引
        std::array<int, 4> faceIndices;
        for (int j = 0; j < 4; j++) {
            int originalIndex = data2.faces[i].vertexIndices[j] + data2VertexOffset;
            faceIndices[j] = vertexIndexMap[originalIndex];
        }

        // 获取 data2 对应面的 faceDirections(假设每个面有4个方向信息,索引为 i*4 ~ i*4+3)
        // 根据 Face 结构体获取剔除标志
        bool doNotCull = (data2.faces[i].faceDirection == FaceType::DO_NOT_CULL);

        // 如果不标记 DO_NOT_CULL 且该面完全被包含在 mesh1 的某个面内,则跳过该面
        if (!doNotCull && isFaceContained(faceIndices))
            continue;

        // 添加该面到合并结果
        Face newFace;
        newFace.vertexIndices = faceIndices;
        newFace.materialIndex = (data2.faces[i].materialIndex != -1) ? materialIndexMap[data2.faces[i].materialIndex] : -1;
        newFace.faceDirection = data2.faces[i].faceDirection;
        mergedData.faces.push_back(newFace);

        // 对应的UV面处理
        std::array<int, 4> uvFaceIndices;
        for (int j = 0; j < 4; j++) {
            int originalUVIndex = data2.faces[i].uvIndices[j] + data2UVOffset;
            uvFaceIndices[j] = uvIndexMap[originalUVIndex];
        }
        mergedData.faces.back().uvIndices = uvFaceIndices;
    }

    return mergedData;
}

void MergeModelsDirectly(ModelData& data1, const ModelData& data2) {
    // 优化:预分配并按倍增扩容,减少内存重分配
    {
        size_t oldV = data1.vertices.size();
        size_t addV = data2.vertices.size();
        size_t needV = oldV + addV;
        size_t capV = data1.vertices.capacity();
        if (needV > capV) {
            size_t newCapV = capV * 2 > needV ? capV * 2 : needV;
            data1.vertices.reserve(newCapV);
        }
    }
    {
        size_t oldUV = data1.uvCoordinates.size();
        size_t addUV = data2.uvCoordinates.size();
        size_t needUV = oldUV + addUV;
        size_t capUV = data1.uvCoordinates.capacity();
        if (needUV > capUV) {
            size_t newCapUV = capUV * 2 > needUV ? capUV * 2 : needUV;
            data1.uvCoordinates.reserve(newCapUV);
        }
    }
    {
        size_t oldMat = data1.materials.size();
        size_t addMat = data2.materials.size();
        size_t needMat = oldMat + addMat;
        size_t capMat = data1.materials.capacity();
        if (needMat > capMat) {
            size_t newCapMat = capMat * 2 > needMat ? capMat * 2 : needMat;
            data1.materials.reserve(newCapMat);
        }
    }
    {
        size_t oldF = data1.faces.size();
        size_t addF = data2.faces.size();
        size_t needF = oldF + addF;
        size_t capF = data1.faces.capacity();
        if (needF > capF) {
            size_t newCapF = capF * 2 > needF ? capF * 2 : needF;
            data1.faces.reserve(newCapF);
        }
    }

    // 顶点偏移
    const size_t vertexOffset = data1.vertices.size() / 3;

    // 复制顶点数据
    data1.vertices.insert(data1.vertices.end(), 
        data2.vertices.begin(), data2.vertices.end());

    // UV坐标偏移
    const size_t uvOffset = data1.uvCoordinates.size() / 2;
    
    // 复制UV坐标
    data1.uvCoordinates.insert(data1.uvCoordinates.end(),
        data2.uvCoordinates.begin(), data2.uvCoordinates.end());

    // 材质索引映射
    std::unordered_map<std::string, int> materialNameMap;
    for (size_t i = 0; i < data1.materials.size(); ++i) {
        materialNameMap[data1.materials[i].name] = static_cast<int>(i);
    }

    std::vector<int> materialIndexMap(data2.materials.size(), -1);
    for (size_t i = 0; i < data2.materials.size(); ++i) {
        auto it = materialNameMap.find(data2.materials[i].name);
        if (it != materialNameMap.end()) {
            materialIndexMap[i] = it->second;
        } else {
            materialIndexMap[i] = data1.materials.size();
            data1.materials.push_back(data2.materials[i]);
        }
    }

    // 复制面数据
    for (const auto& face : data2.faces) {
        Face newFace;
        
        // 顶点索引偏移
        for (int j = 0; j < 4; ++j) {
            newFace.vertexIndices[j] = face.vertexIndices[j] + vertexOffset;
        }
        
        // UV索引偏移
        for (int j = 0; j < 4; ++j) {
            newFace.uvIndices[j] = face.uvIndices[j] + uvOffset;
        }
        
        // 材质索引映射
        if (face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
            newFace.materialIndex = materialIndexMap[face.materialIndex];
        } else {
            newFace.materialIndex = 0; // 默认第一个材质
        }
        
        // 保留面方向
        newFace.faceDirection = face.faceDirection;
        
        data1.faces.push_back(newFace);
    }
}

// 辅助函数:将字符串方向转换为FaceType枚举
FaceType StringToFaceType(const std::string& dirString) {
    if (dirString == "down") return FaceType::DOWN;
    if (dirString == "up") return FaceType::UP;
    if (dirString == "north") return FaceType::NORTH;
    if (dirString == "south") return FaceType::SOUTH;
    if (dirString == "west") return FaceType::WEST;
    if (dirString == "east") return FaceType::EAST;
    if (dirString == "DO_NOT_CULL") return FaceType::DO_NOT_CULL;
    return FaceType::UNKNOWN;
}


// 辅助函数:根据面索引获取面方向 (每4个顶点构成一个面)
FaceType GetFaceTypeByIndex(size_t faceIndex) {
    // 标准立方体模型的面顺序通常是:底面、顶面、北面、南面、西面、东面
    size_t normalizedIndex = faceIndex / 4; // 每个面由4个顶点索引组成
    switch (normalizedIndex % 6) {
        case 0: return FaceType::DOWN;  // 底面
        case 1: return FaceType::UP;    // 顶面
        case 2: return FaceType::NORTH; // 北面
        case 3: return FaceType::SOUTH; // 南面
        case 4: return FaceType::WEST;  // 西面
        case 5: return FaceType::EAST;  // 东面
        default: return FaceType::UNKNOWN;
    }
}



