#ifndef MODEL_H
#define MODEL_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cmath>
#include "include/json.hpp"
#include <mutex>
#include <future>
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "GlobalCache.h"
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

    short tintindex;

    std::vector<std::string> faceDirections; // 每个面的方向 faceDirections 4个一组代表一个面
    std::vector<std::string> faceNames;       // 每个面的名称
};

// 自定义顶点键：用整数表示，精度保留到小数点后6位
struct VertexKey {
    int x, y, z;
    bool operator==(const VertexKey& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
};

// 自定义 UV 键
struct UVKey {
    int u, v;
    bool operator==(const UVKey& other) const {
        return u == other.u && v == other.v;
    }
};
// 自定义顶点键
struct FaceKey {
    std::array<int, 4> sortedVerts;
    int materialIndex;
    bool operator==(const FaceKey& other) const {
        return sortedVerts == other.sortedVerts &&
            materialIndex == other.materialIndex;
    }
};

struct FaceKeyHasher {
    size_t operator()(const FaceKey& k) const {
        size_t seed = 0;
        // 结合材质信息和每个顶点索引
        seed ^= std::hash<int>()(k.materialIndex) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        for (int v : k.sortedVerts) {
            seed ^= std::hash<int>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};
namespace std {
    template <>
    struct hash<VertexKey> {
        std::size_t operator()(const VertexKey& key) const {
            size_t res = 17;
            res = res * 31 + hash<int>()(key.x);
            res = res * 31 + hash<int>()(key.y);
            res = res * 31 + hash<int>()(key.z);
            return res;
        }
    };

    template <>
    struct hash<UVKey> {
        std::size_t operator()(const UVKey& key) const {
            size_t res = 17;
            res = res * 31 + hash<int>()(key.u);
            res = res * 31 + hash<int>()(key.v);
            return res;
        }
    };
}

enum FaceType { UP, DOWN, NORTH, SOUTH, WEST, EAST, UNKNOWN };

//---------------- 缓存管理 ----------------
static std::mutex cacheMutex;
static std::recursive_mutex parentModelCacheMutex;

// 静态缓存
static std::unordered_map<std::string, ModelData> modelCache; // Key: "namespace:blockId"
static std::unordered_map<std::string, nlohmann::json> parentModelCache;

//---------------- 核心功能声明 ----------------
// 模型处理
ModelData ProcessModelJson(const std::string& namespaceName,
    const std::string& blockId,
    int rotationX, int rotationY,bool uvlock, int randomIndex = 0, const std::string& blockstateName="");

// 模型合并
ModelData MergeModelData(const ModelData& data1, const ModelData& data2);

ModelData MergeFluidModelData(const ModelData& data1, const ModelData& data2);

void MergeModelsDirectly(ModelData& data1, const ModelData& data2);

void ApplyPositionOffset(ModelData& model, int x, int y, int z);
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
