#ifndef MODEL_H
#define MODEL_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cmath>
#include <nlohmann/json.hpp>  // 用于解析 JSON
#include <mutex>
#include <future>
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "GlobalCache.h"
#include "version.h"
#pragma once

#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

//---------------- 数据类型定义 ----------------
// 模型数据结构体
struct ModelData {
    // 顶点数据（x,y,z顺序存储）
    std::vector<float> vertices;          // 每3个元素构成一个顶点
    std::vector<float> uvCoordinates;     // 每2个元素构成一个UV坐标
    
    // 面数据（四边面）
    std::vector<int> faces;               // 每4个顶点索引构成一个面
    std::vector<int> uvFaces;             // 每4个UV索引构成一个面
    
    // 材质系统（保持原优化方案）
    std::vector<int> materialIndices;     // 每个面对应的材质索引
    std::vector<std::string> materialNames;
    std::vector<std::string> texturePaths;

    std::vector<std::string> faceDirections; // 每个面的方向
    std::vector<std::string> faceNames;           // 每个面的名称
};

enum FaceType { UP, DOWN, NORTH, SOUTH, WEST, EAST, UNKNOWN };

//---------------- 缓存管理 ----------------
static std::mutex cacheMutex;
static std::recursive_mutex parentModelCacheMutex;
static std::mutex texturePathCacheMutex; 

// 静态缓存
static std::unordered_map<std::string, ModelData> modelCache; // Key: "namespace:blockId"
static std::unordered_map<std::string, nlohmann::json> parentModelCache;
static std::unordered_map<std::string, std::string> texturePathCache; // Key: "namespace:pathPart", Value: 保存的材质路径

//---------------- 核心功能声明 ----------------
// 模型处理
ModelData ProcessModelJson(const std::string& namespaceName,
    const std::string& blockId,
    int rotationX, int rotationY,bool uvlock);

// 模型合并
ModelData MergeModelData(const ModelData& data1, const ModelData& data2);
void MergeModelsDirectly(ModelData& data1, const ModelData& data2);


// exe路径获取
std::string getExecutableDir();
//---------------- JSON处理 ----------------
nlohmann::json GetModelJson(const std::string& namespaceName,
    const std::string& modelPath);
nlohmann::json LoadParentModel(const std::string& namespaceName,
    const std::string& blockId,
    nlohmann::json& currentModelJson);
nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson,
    const nlohmann::json& currentModelJson);

#endif // MODEL_H
