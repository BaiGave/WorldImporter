// MODEL_H.h
#ifndef MODEL_H
#define MODEL_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <array>
#include <string>
#include <cmath>
#include "include/json.hpp"
#include <mutex>
#include <future>
#include <concepts>     // C++20特性
#include <span>         // C++20特性
#include <ranges>       // C++20特性
#include <compare>      // C++20特性：三路比较运算符
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "GlobalCache.h"
#pragma once

#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif
enum FaceType { UP, DOWN, NORTH, SOUTH, WEST, EAST, DO_NOT_CULL, UNKNOWN };

//---------------- 材质信息定义 ----------------
struct Material {
    std::string name;       // 材质名称
    std::string texturePath;// 纹理路径
    short tintIndex;        // tint 索引
};

//---------------- 数据类型定义 ----------------
// 在 ModelData 定义之前新增 Face 结构体定义，用于包含顶点索引、UV 索引、材质索引和面方向
struct Face {
    std::array<int, 4> vertexIndices; // 四个顶点索引
    std::array<int, 4> uvIndices;     // 四个 UV 索引
    int materialIndex;                // 材质索引
    FaceType faceDirection;           // 剔除方向
};

// 修改 ModelData，使用统一 Face 结构体替换原有的 faces、uvFaces、materialIndices 和 faceDirections
struct ModelData {
    // 顶点数据（x,y,z顺序存储）
    std::vector<float> vertices;          // 每3个元素构成一个顶点
    std::vector<float> uvCoordinates;     // 每2个元素构成一个UV坐标

    // 面数据（使用 Face 结构体优化性能，支持 GreedyMesh 算法）
    std::vector<Face> faces;              // 包含顶点索引、UV索引、材质索引和面方向

    // 材质系统（保持原优化方案）
    std::vector<Material> materials;      // 每个材质包含名称、纹理路径和 tint 索引
};

// 自定义顶点键：用整数表示，精度保留到小数点后6位
struct VertexKey {
    int x, y, z;
    
    // C++20: 使用<=>运算符简化比较操作
    auto operator<=>(const VertexKey&) const = default;
};

// 自定义 UV 键
struct UVKey {
    int u, v;
    
    // C++20: 使用<=>运算符简化比较操作
    auto operator<=>(const UVKey&) const = default;
};

// 自定义顶点键
struct FaceKey {
    std::array<int, 4> sortedVerts;
    int materialIndex;
    
    // C++20: 使用<=>运算符简化比较操作
    auto operator<=>(const FaceKey&) const = default;
};

// 使用C++20的概念定义模型处理相关的约束
template<typename T>
concept Vertex = requires(T v) {
    { v.x } -> std::convertible_to<float>;
    { v.y } -> std::convertible_to<float>;
    { v.z } -> std::convertible_to<float>;
};

template<typename T>
concept TextureCoord = requires(T uv) {
    { uv.u } -> std::convertible_to<float>;
    { uv.v } -> std::convertible_to<float>;
};

// 修改哈希函数以使用C++20的功能
struct FaceKeyHasher {
    size_t operator()(const FaceKey& k) const {
        size_t seed = 0;
        // 结合材质信息和每个顶点索引
        seed ^= std::hash<int>()(k.materialIndex) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        
        // 使用C++20的ranges功能来简化遍历
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
            // 使用C++20的组合哈希功能
            return std::hash<int>()(key.x) ^ 
                  (std::hash<int>()(key.y) << 1) ^ 
                  (std::hash<int>()(key.z) << 2);
        }
    };

    template <>
    struct hash<UVKey> {
        std::size_t operator()(const UVKey& key) const {
            // 使用C++20的组合哈希功能
            return std::hash<int>()(key.u) ^ 
                  (std::hash<int>()(key.v) << 1);
        }
    };
}


// 辅助函数：将字符串方向转换为FaceType枚举
FaceType StringToFaceType(const std::string& dirString);

// 辅助函数：根据面索引获取面方向 (每4个顶点构成一个面)
FaceType GetFaceTypeByIndex(size_t faceIndex);

// 辅助函数：将FaceType枚举转换为字符串
std::string FaceTypeToString(FaceType faceType);

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

// 使用C++20的span来改进参数传递（避免复制）
void ApplyPositionOffset(ModelData& model, int x, int y, int z);
void ApplyDoublePositionOffset(ModelData& model, double x, double y, double z);

// C++20: 使用span高效处理顶点数据
template<typename T>
requires std::is_floating_point_v<T>
void ProcessVertices(std::span<T> vertices, auto process_function) {
    for (size_t i = 0; i < vertices.size(); i += 3) {
        process_function(vertices[i], vertices[i+1], vertices[i+2]);
    }
}

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

// 使用现代C++特性改进旋转函数声明
void ApplyRotationToVertices(std::span<float> vertices, float rx, float ry, float rz);
void ApplyRotationToVertices(std::span<float> vertices, int rotationX, int rotationY);
void ApplyScaleToVertices(std::span<float> vertices, float sx, float sy, float sz);

// 旋转应用到面方向
void ApplyRotationToFaceDirections(std::vector<Face>& faces, int rotationX, int rotationY);

#endif // MODEL_H
