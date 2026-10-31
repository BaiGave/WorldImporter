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
#include <cstdint>
#include <compare>      // C++20特性:三路比较运算符
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "GlobalCache.h"
#include "blocktint.h"
#pragma once

#define _USE_MATH_DEFINES
#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif
enum  FaceType{
    UP, DOWN, NORTH, SOUTH, WEST, EAST, DO_NOT_CULL, UNKNOWN
};



//---------------- 材质信息定义 ----------------
struct Material {
    std::string name;       // 材质名称
    std::string texturePath;// 纹理路径
    std::string textureKey; // 模型 textures 中的贴图键(如 all/side/top)，用于按方块 id 重命名
    int8_t  tintIndex;      // tint 索引
    TintResult tint;        // 解析后的 tint 结果（None 表示不上色）
    bool tintLocked = false;// tint 已由 CTM 规则(tintIndex/tintBlock)锁定, 不再参与重解析
    MaterialType type;      // 材质类型
    float aspectRatio;      // 动态材质长宽比（高/宽）
    // 周期 atlas 材质(仅 repeat CTM): 面 UV 落在 atlas 格子里, 且格子按世界
    // 坐标周期排列。贪心合并时可跨格扩展 UV, 由纹理 REPEAT 回绕。
    bool  uvAtlas = false;      // 该材质是一张 atlas(非整图贴图)
    bool  uvPeriodic = false;
    float uvCellW = 0.0f;   // 一格在 UV 空间的宽度 (1/cols)
    float uvCellH = 0.0f;   // 一格在 UV 空间的高度 (1/rows)

    // 构造函数,默认为普通材质
    Material() : name(""), texturePath(""), tintIndex(-1), type(NORMAL), aspectRatio(1.0f) {}
    Material(const std::string& name, const std::string& path, int8_t tint) 
        : name(name), texturePath(path), tintIndex(tint), type(NORMAL), aspectRatio(1.0f) {}
    Material(const std::string& name, const std::string& path, int8_t tint, MaterialType materialType)
        : name(name), texturePath(path), tintIndex(tint), type(materialType), aspectRatio(1.0f) {}
    Material(const std::string& name, const std::string& path, int8_t tint, MaterialType materialType, float ratio)
        : name(name), texturePath(path), tintIndex(tint), type(materialType), aspectRatio(ratio) {}
};

//---------------- 数据类型定义 ----------------
// 在 ModelData 定义之前新增 Face 结构体定义,用于包含顶点索引、UV 索引、材质索引和面方向
struct Face {
    std::array<int, 4> vertexIndices; // 四个顶点索引
    std::array<int, 4> uvIndices;     // 四个 UV 索引
    int materialIndex;                // 材质索引
    FaceType faceDirection;           // 剔除方向
    int8_t uvFace = -1;               // 模型里声明的面方向(FaceType), uvlock 用
    int8_t tintIndex = -1;            // 面级 tintindex（-1 表示不染色）
    // 来自零厚度贴片显式定义的正反两面（同一元素内朝向相反且完全重合）。
    // 这是"双面几何"唯一可靠的依据：合并后网格里相邻方块的接触面虽然同样共面
    // 重合，但那不是双面几何，必须靠这个标记区分（见 doubleSided.json 的收录口径）。
    bool bothSided = false;
    // 极薄元素(厚度 < 1 像素)恰好贴在方块界面上的那一面(如装饰贴片贴着方块边界)。
    // 这类面与相邻方块的接触面严格共面且朝向相反: 单模型内去重管不到(顶点索引不同)、
    // 邻居遮挡也删不掉(贴片自身在该方向没有 cullface)，会在 Blender 里 z-fighting。
    // 导出时若该方向邻居是完整遮挡体，就沿面法线外推 overlayLayerStep 把两个面分开。
    bool thinEdge = false;
};

// 共面叠加层配对(如草方块侧面的 overlay 元素):
// overlayFace 与 baseFace 同位置且最终 UV 一致。导出时若底层存活则删除叠加面,
// 并把叠加层材质/贴图/tint 登记进 overlay.json,由 Blender 侧材质节点实现叠加。
struct OverlayPair {
    int baseFace = -1;
    int overlayFace = -1;
};

// 修改 ModelData,使用统一 Face 结构体替换原有的 faces、uvFaces、materialIndices 和 faceDirections
struct ModelData {
    // 顶点数据(x,y,z顺序存储)
    std::vector<float> vertices;          // 每3个元素构成一个顶点
    std::vector<float> uvCoordinates;     // 每2个元素构成一个UV坐标

    // 面数据(使用 Face 结构体优化性能,支持 GreedyMesh 算法)
    std::vector<Face> faces;              // 包含顶点索引、UV索引、材质索引和面方向

    // 材质系统(保持原优化方案)
    std::vector<Material> materials;      // 每个材质包含名称、纹理路径和 tint 索引

    // 共面叠加层配对(见 OverlayPair)
    std::vector<OverlayPair> overlayPairs;

    // 每个面在模型 JSON 里的 uv 区域(像素)与 rotation，与 faces 一一对应；
    // 仅在刚加载的模型上有效，供 uvlock 按原版 FaceBakery.recomputeUVs 重算。
    struct UvSpec { float region[4]; int rotation; };
    std::vector<UvSpec> uvSpecs;
};

// Quantized vertex coordinates, with a 1e-4 step.
struct VertexKey {
    int x, y, z;
    
    // C++20: 使用<=>运算符简化比较操作
    auto operator<=>(const VertexKey&) const = default;
};

inline VertexKey MakeVertexKey(float x, float y, float z) {
    auto quantize = [](float coordinate) {
        // Keep the original float multiplication before rounding. Add the
        // half step in double: float addition could round a value just below
        // a half boundary up to it, or move an already integral large value.
        // Truncation then matches std::round (ties away from zero) wherever
        // the original rounded value is representable as an int.
        const float scaled = coordinate * 10000.0f;
        return static_cast<int>(static_cast<double>(scaled) + std::copysign(0.5, scaled));
    };
    return VertexKey{quantize(x), quantize(y), quantize(z)};
}

// 自定义 UV 键
struct UVKey {
    int u, v;
    
    // C++20: 使用<=>运算符简化比较操作
    auto operator<=>(const UVKey&) const = default;
};

// 自定义顶点键
// 注意: 必须包含 UV 索引。CTM overlay 会在同一方块面上叠加多个 tile
// (顶点/材质相同, 仅 UV 不同), 只按顶点+材质去重会把它们误删(缺角/缺边)。
struct FaceKey {
    std::array<int, 4> sortedVerts;
    std::array<int, 4> sortedUVs;   // 与 sortedVerts 一一对应
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
        for (int u : k.sortedUVs) {
            seed ^= std::hash<int>()(u) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
        return seed;
    }
};

namespace std {
    template <>
    struct hash<VertexKey> {
        std::size_t operator()(const VertexKey& key) const noexcept {
            // Mix all coordinate bits, including the low zeros common in grid
            // coordinates. Pack x/y without overlap, then mix in z separately.
            auto mix = [](std::uint64_t value) {
                value ^= value >> 30;
                value *= 0xbf58476d1ce4e5b9ULL;
                value ^= value >> 27;
                value *= 0x94d049bb133111ebULL;
                return value ^ (value >> 31);
            };
            const std::uint64_t xy =
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32) |
                static_cast<std::uint32_t>(key.y);
            return static_cast<std::size_t>(mix(mix(xy) ^
                (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.z)) +
                 0x9e3779b97f4a7c15ULL)));
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


// 辅助函数:将字符串方向转换为FaceType枚举
FaceType StringToFaceType(const std::string& dirString);
// uvlock UV rotation (degrees, OBJ UV space) for a declared face under a blockstate x/y rotation.
int UvLockAngle(FaceType face, int rotationX, int rotationY);
// Vanilla FaceBakery.recomputeUVs: uv region (pixels) + face rotation under uvlock.
void RecomputeUvLock(const float region[4], int rotation, FaceType face, int rotationX, int rotationY,
                     float outRegion[4], int& outRotation);

// 辅助函数:根据面索引获取面方向 (每4个顶点构成一个面)
FaceType GetFaceTypeByIndex(size_t faceIndex);


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

// 按方块 id 重命名模型中的材质（方块id 或 方块id#贴图键~贴图名，详见 model.cpp）
void RenameBlockMaterials(ModelData& model,
    const std::string& namespaceName,
    const std::string& baseBlockId);

// 模组方块复用同一贴图材质而省下的次数（导出日志用）
size_t GetModMaterialReuseCount();

// 剥掉材质名末尾的 tint/CTM 后缀（@grass/@foliage/@cRRGGBB 等），得到基础规范全名。
// 与 Crafter 侧 Defs.material_base_name() 的 @ 处理对称：同一方块的不同 tint 变体在
// Blender 里是不同的材质，但双面几何清单只需要基础名一条。
std::string StripTintSuffix(const std::string& name);

// 一对共面正反面的 UV 关系，决定能否安全合并成一张面：
//   Same  : 同材质且按顶点一一对应——两面世界 UV 同向，合并后从背面看到的镜像
//           与保留两张面时完全一致，可安全合并；
//   Mirror: 同材质但 UV 镜像（模型里最常见的双面写法，两面从左到右都正常），
//           合并会让一面变成镜像，不能合并；
//   Other : 材质或 UV 区域不同，同样不能合并。
enum class DoubleSidedPairKind : uint8_t { Same = 0, Mirror = 1, Other = 2 };

// Blender 的网格校验会把"顶点索引集合完全相同"的多边形当成重复面删除（不看绕序），
// 导致共面正反两面导入后只剩一张。这里给每组重合面里第二张及以后的面复制一个顶点
// （坐标不变、仅索引不同），让 Blender 保留双面几何。return: 被处理的面数
size_t SeparateCoincidentFaces(ModelData& model);

// 登记一个使用了双面几何的材质（内部剥掉 @ 后缀后按材质名去重，多线程安全）。
// 供 SeparateCoincidentFaces 命中重合面时调用，结果由 CreateDoubleSidedJsonFile 写出。
void RegisterDoubleSidedMaterial(const Material& material);

// 登记一对双面几何的 UV 关系（Same/Mirror/Other），供 doubleSided.json 输出分类统计。
// merged=true 表示这一对实际被合并成一张面（only for Same）。多线程安全。
void RegisterDoubleSidedPairKind(const Material& material, DoubleSidedPairKind kind, bool merged);

// 取贴图路径的文件名部分并去掉扩展名（textures/minecraft/block/oak_log.png -> oak_log）
std::string MaterialTextureBasename(const std::string& texturePath);

// 模型合并
ModelData MergeModelData(const ModelData& data1, const ModelData& data2);

ModelData MergeFluidModelData(const ModelData& data1, const ModelData& data2);

// Optional destination-owned lookup for repeated appends; reset it whenever
// the destination materials are replaced/reordered.
void MergeModelsDirectly(ModelData& data1, const ModelData& data2,
    std::unordered_map<std::string, int>* materialLookup = nullptr);

// 剪掉没有任何面引用的材质（被剔除方块留下的、以及模型里未被任何面使用的贴图键，
// 如只作破坏粒子用的 particle 键），并重映射面的材质索引。
// keepNames: 额外需要保留的材质名（例如 overlay 序列引用到的名字），可为空。
// return: 被移除的材质个数
size_t PruneUnusedMaterials(ModelData& model,
    const std::unordered_set<std::string>* keepNames = nullptr);

// 使用C++20的span来改进参数传递(避免复制)
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


//---------------- JSON处理 ----------------
nlohmann::json GetModelJson(const std::string& namespaceName,
    const std::string& modelPath);
nlohmann::json LoadParentModel(const std::string& namespaceName,
    const std::string& blockId,
    nlohmann::json& currentModelJson,
    std::unordered_set<std::string>* visiting = nullptr,
    int depth = 0);
nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson,
    const nlohmann::json& currentModelJson);

// 使用现代C++特性改进旋转函数声明
void ApplyRotationToVertices(std::span<float> vertices, float rx, float ry, float rz);
void ApplyRotationToVertices(std::span<float> vertices, int rotationX, int rotationY);
void ApplyScaleToVertices(std::span<float> vertices, float sx, float sy, float sz);

// 旋转应用到面方向
void ApplyRotationToFaceDirections(std::vector<Face>& faces, int rotationX, int rotationY);

#endif // MODEL_H
