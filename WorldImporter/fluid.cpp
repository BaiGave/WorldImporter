#include "fluid.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include "block.h"
#include "model.h"
#include "texture.h"

using namespace std;

// 流体注册数据
std::unordered_map<std::string, FluidInfo> fluidDefinitions;
// 模型缓存
static std::unordered_map<size_t, ModelData> fluidModelCache;
static std::shared_mutex fluidModelCacheMutex;

// --------------------------------------------------------------------------------
// 原版高度规则
// --------------------------------------------------------------------------------

float GetFluidOwnHeight(int level) {
    // 原版 FluidState#getOwnHeight = amount/9：
    //   水源 level 0        -> amount 8
    //   流动 level 1..7     -> amount 8-level
    //   下落 level >= 8     -> falling，amount 8（8/9 格高）
    if (level >= 8) return 8.0f / 9.0f;
    if (level < 0) return 0.0f;
    level = std::clamp(level, 0, 7);
    return static_cast<float>(8 - level) / 9.0f;
}

// 原版 FlowingFluidRenderer#addWeightedHeight
static void AddWeightedHeight(float sum[2], float height) {
    if (height >= 0.8f) {
        sum[0] += height * 10.0f;
        sum[1] += 10.0f;
    }
    else if (height >= 0.0f) {
        sum[0] += height;
        sum[1] += 1.0f;
    }
}

float CalculateFluidCornerHeight(float selfHeight, float cornerHeight, float sideA, float sideB) {
    if (sideA >= 1.0f || sideB >= 1.0f) {
        return 1.0f;
    }
    float sum[2] = { 0.0f, 0.0f };
    if (sideA > 0.0f || sideB > 0.0f) {
        if (cornerHeight >= 1.0f) {
            return 1.0f;
        }
        AddWeightedHeight(sum, cornerHeight);
    }
    AddWeightedHeight(sum, selfHeight);
    AddWeightedHeight(sum, sideA);
    AddWeightedHeight(sum, sideB);
    return (sum[1] == 0.0f) ? 0.0f : sum[0] / sum[1];
}

void ComputeFluidCornerHeights(const FluidModelParams& params, float outCorner[4]) {
    // 原版 FluidRenderer：中心高度 >= 1（上方仍是同种流体）时，四角直接满高，
    // 不再做加权平均。缺了这步会让水柱的每个方块按自身格子重算角高，
    // 出现每层约 0.09 格的横向缝隙（水面阶梯）。
    if (params.selfHeight >= 1.0f) {
        outCorner[0] = 1.0f;
        outCorner[1] = 1.0f;
        outCorner[2] = 1.0f;
        outCorner[3] = 1.0f;
        return;
    }
    // 角顺序：西北、东北、东南、西南；侧面顺序：北、南、西、东
    outCorner[0] = CalculateFluidCornerHeight(params.selfHeight, params.cornerHeight[0], params.sideHeight[0], params.sideHeight[2]);
    outCorner[1] = CalculateFluidCornerHeight(params.selfHeight, params.cornerHeight[1], params.sideHeight[0], params.sideHeight[3]);
    outCorner[2] = CalculateFluidCornerHeight(params.selfHeight, params.cornerHeight[2], params.sideHeight[1], params.sideHeight[3]);
    outCorner[3] = CalculateFluidCornerHeight(params.selfHeight, params.cornerHeight[3], params.sideHeight[1], params.sideHeight[2]);
}

// --------------------------------------------------------------------------------
// UV 约定：帧内 UV -> 导出 UV（条带顶帧区域，插件负责动画）
// MC 的 V=0 在图片顶部，导出(Blender)的 V=1 在顶部，因此 V 需要翻转。
// --------------------------------------------------------------------------------
namespace {
    struct UVLayout {
        float maxV = 1.0f;    // 1/帧数
        float startV = 0.0f;  // 1 - maxV（顶帧起始）
    };

    inline float ExportV(const UVLayout& layout, float vLocal) {
        return layout.startV + (1.0f - vLocal) * layout.maxV;
    }

    inline float ExportU(float uLocal) {
        return uLocal;
    }
}

ModelData GenerateFluidModel(const FluidModelParams& params, const std::string& fluidId) {
    ModelData model;

    // ---- 缓存键：包含所有影响几何/UV/材质的输入 ----
    size_t key = std::hash<std::string>{}(fluidId);
    auto mixFloat = [&key](float v) {
        int q = static_cast<int>(std::lround(v * 10000.0f));
        key ^= std::hash<int>{}(q) + 0x9e3779b9 + (key << 6) + (key >> 2);
    };
    mixFloat(params.selfHeight);
    for (int i = 0; i < 4; ++i) {
        mixFloat(params.sideHeight[i]);
        mixFloat(params.cornerHeight[i]);
    }
    mixFloat(params.flowX);
    mixFloat(params.flowZ);
    for (int i = 0; i < 6; ++i) {
        key ^= std::hash<bool>{}(params.keepFace[i]) + 0x9e3779b9 + (key << 6) + (key >> 2);
    }
    key ^= std::hash<bool>{}(params.falling) + 0x9e3779b9 + (key << 6) + (key >> 2);
    key ^= std::hash<bool>{}(params.downCanRender) + 0x9e3779b9 + (key << 6) + (key >> 2);
    key ^= std::hash<bool>{}(params.topCanRender) + 0x9e3779b9 + (key << 6) + (key >> 2);

    {
        std::shared_lock<std::shared_mutex> lock(fluidModelCacheMutex);
        auto cacheIt = fluidModelCache.find(key);
        if (cacheIt != fluidModelCache.end()) return cacheIt->second;
    }
    // Geometry generation/metadata lookup must not serialize all water blocks.
    // Duplicate concurrent misses are harmless; publish the completed model.

    // ---- 角高（原版 calculateAverageHeight；单位：格）----
    // 角顺序：西北、东北、东南、西南；侧面顺序：北、南、西、东
    float fluidCorners[4]; // NW, NE, SE, SW
    ComputeFluidCornerHeights(params, fluidCorners);
    float h_nw = fluidCorners[0];
    float h_ne = fluidCorners[1];
    float h_se = fluidCorners[2];
    float h_sw = fluidCorners[3];

    // 原版：顶面渲染时四角统一 -0.001（用于避免与顶面相关的 z-fighting）
    if (params.topCanRender) {
        h_nw -= 0.001f;
        h_ne -= 0.001f;
        h_se -= 0.001f;
        h_sw -= 0.001f;
    }

    // 原版：底面渲染时所有侧面底部与底面抬高 0.001
    const float renderMinY = params.downCanRender ? 0.001f : 0.0f;

    model.vertices = {
        // 底面 (bottom)
        0.0f, renderMinY, 0.0f,   // 0
        1.0f, renderMinY, 0.0f,   // 1
        1.0f, renderMinY, 1.0f,   // 2
        0.0f, renderMinY, 1.0f,   // 3

        // 顶面 (top)
        0.0f, h_nw, 0.0f, // 4 西北
        1.0f, h_ne, 0.0f, // 5 东北
        1.0f, h_se, 1.0f, // 6 东南
        0.0f, h_sw, 1.0f, // 7 西南

        // 北面 (north, z-)
        0.0f, renderMinY, 0.0f, // 8
        1.0f, renderMinY, 0.0f, // 9
        1.0f, h_ne, 0.0f,       // 10
        0.0f, h_nw, 0.0f,       // 11

        // 南面 (south, z+)
        0.0f, renderMinY, 1.0f, // 12
        1.0f, renderMinY, 1.0f, // 13
        1.0f, h_se, 1.0f,       // 14
        0.0f, h_sw, 1.0f,       // 15

        // 西面 (west, x-)
        0.0f, renderMinY, 0.0f, // 16
        0.0f, renderMinY, 1.0f, // 17
        0.0f, h_sw, 1.0f,       // 18
        0.0f, h_nw, 0.0f,       // 19

        // 东面 (east, x+)
        1.0f, renderMinY, 0.0f, // 20
        1.0f, renderMinY, 1.0f, // 21
        1.0f, h_se, 1.0f,       // 22
        1.0f, h_ne, 0.0f        // 23
    };

    // ---- 材质/贴图信息（沿用现有约定）----
    std::string namespace_name = "minecraft";
    std::string base_id = fluidId;
    size_t colonPos = fluidId.find(':');
    if (colonPos != std::string::npos) {
        namespace_name = fluidId.substr(0, colonPos);
        base_id = fluidId.substr(colonPos + 1);
    }
    size_t bracketPos = base_id.find('[');
    if (bracketPos != std::string::npos) {
        base_id = base_id.substr(0, bracketPos);
    }

    std::string stillTexturePath = "block/" + base_id + "_still";
    std::string flowTexturePath = "block/" + base_id + "_flow";
    auto fluidIt = fluidDefinitions.find(namespace_name + ":" + base_id);
    if (fluidIt != fluidDefinitions.end()) {
        const FluidInfo& info = fluidIt->second;
        stillTexturePath = info.folder + "/" + base_id + info.still_texture;
        flowTexturePath = info.folder + "/" + base_id + info.flow_texture;
    }

    float stillAspectRatio = 1.0f;
    float flowAspectRatio = 1.0f;
    MaterialType stillType = DetectMaterialType(namespace_name, stillTexturePath, stillAspectRatio);
    MaterialType flowType = DetectMaterialType(namespace_name, flowTexturePath, flowAspectRatio);
    if (stillAspectRatio < 1.0f) stillAspectRatio = 1.0f;
    if (flowAspectRatio < 1.0f) flowAspectRatio = 1.0f;

    UVLayout stillLayout;
    stillLayout.maxV = 1.0f / stillAspectRatio;
    stillLayout.startV = 1.0f - stillLayout.maxV;
    UVLayout flowLayout;
    flowLayout.maxV = 1.0f / flowAspectRatio;
    flowLayout.startV = 1.0f - flowLayout.maxV;

    // ---- UV（原版 FluidRenderer 的帧内映射，映射到导出顶帧区域）----
    // still 贴图：整帧 (0..1)
    const float noFlow = (params.flowX == 0.0f && params.flowZ == 0.0f) ? 1.0f : 0.0f;
    const bool useFlowTop = (noFlow == 0.0f);

    float angle = 0.0f, sinA = 0.0f, cosA = 0.0f;
    if (useFlowTop) {
        constexpr float kPi = 3.14159265358979323846f;
        angle = std::atan2(params.flowZ, params.flowX) - kPi / 2.0f;
        sinA = std::sin(angle) * 0.25f;
        cosA = std::cos(angle) * 0.25f;
    }

    // 顶面四角帧内 UV（原版）：西北、西南、东南、东北
    float topU[4], topV[4];
    if (!useFlowTop) {
        topU[0] = 0.0f; topV[0] = 0.0f; // NW
        topU[1] = 0.0f; topV[1] = 1.0f; // SW
        topU[2] = 1.0f; topV[2] = 1.0f; // SE
        topU[3] = 1.0f; topV[3] = 0.0f; // NE
    }
    else {
        topU[0] = 0.5f - cosA - sinA; topV[0] = 0.5f - cosA + sinA; // NW
        topU[1] = 0.5f - cosA + sinA; topV[1] = 0.5f + cosA + sinA; // SW
        topU[2] = 0.5f + cosA + sinA; topV[2] = 0.5f + cosA - sinA; // SE
        topU[3] = 0.5f + cosA - sinA; topV[3] = 0.5f - cosA - sinA; // NE
    }

    model.uvCoordinates = {
        // 底面（still，整帧）：顶点 0,3,2,1
        ExportU(0.0f), ExportV(stillLayout, 0.0f),
        ExportU(0.0f), ExportV(stillLayout, 1.0f),
        ExportU(1.0f), ExportV(stillLayout, 1.0f),
        ExportU(1.0f), ExportV(stillLayout, 0.0f),

        // 顶面（still 或 flow）：顶点 4(NW),7(SW),6(SE),5(NE)
        ExportU(topU[0]), useFlowTop ? ExportV(flowLayout, topV[0]) : ExportV(stillLayout, topV[0]),
        ExportU(topU[1]), useFlowTop ? ExportV(flowLayout, topV[1]) : ExportV(stillLayout, topV[1]),
        ExportU(topU[2]), useFlowTop ? ExportV(flowLayout, topV[2]) : ExportV(stillLayout, topV[2]),
        ExportU(topU[3]), useFlowTop ? ExportV(flowLayout, topV[3]) : ExportV(stillLayout, topV[3]),

        // 北面（flow）：顶点 8,11,10,9
        ExportU(0.0f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, (1.0f - h_nw) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, (1.0f - h_ne) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, 0.5f),

        // 南面（flow）：顶点 12,13,14,15
        ExportU(0.5f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, (1.0f - h_se) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, (1.0f - h_sw) * 0.5f),

        // 西面（flow）：顶点 16,17,18,19
        ExportU(0.5f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, (1.0f - h_sw) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, (1.0f - h_nw) * 0.5f),

        // 东面（flow）：顶点 20,23,22,21
        ExportU(0.0f), ExportV(flowLayout, 0.5f),
        ExportU(0.0f), ExportV(flowLayout, (1.0f - h_ne) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, (1.0f - h_se) * 0.5f),
        ExportU(0.5f), ExportV(flowLayout, 0.5f)
    };

    // ---- 面（按需保留）----
    struct FaceDef {
        std::array<int, 4> vertices;
        std::array<int, 4> uvs;
        FaceType dir;
        int material; // 0=still, 1=flow
    };
    const FaceDef faceDefs[6] = {
        { { 0, 3, 2, 1 },   { 0, 1, 2, 3 },   FaceType::DOWN,  0 }, // 底面 still
        { { 4, 7, 6, 5 },   { 4, 5, 6, 7 },   FaceType::UP,    useFlowTop ? 1 : 0 }, // 顶面
        { { 8, 11, 10, 9 }, { 8, 9, 10, 11 }, FaceType::NORTH, 1 },
        { { 12, 13, 14, 15 }, { 12, 13, 14, 15 }, FaceType::SOUTH, 1 },
        { { 16, 17, 18, 19 }, { 16, 17, 18, 19 }, FaceType::WEST, 1 },
        { { 20, 23, 22, 21 }, { 20, 21, 22, 23 }, FaceType::EAST, 1 }
    };

    for (int i = 0; i < 6; ++i) {
        if (!params.keepFace[i]) continue;
        Face face;
        face.vertexIndices = faceDefs[i].vertices;
        face.uvIndices = faceDefs[i].uvs;
        face.materialIndex = faceDefs[i].material;
        // 已在生成阶段完成剔除决策，交给后续流程时不再二次剔除
        face.faceDirection = FaceType::DO_NOT_CULL;
        model.faces.push_back(face);
    }

    // ---- 材质 ----
    Material stillMaterial;
    stillMaterial.name = namespace_name + ":" + base_id + "#still~" + MaterialTextureBasename(stillTexturePath);
    stillMaterial.texturePath = "textures/" + namespace_name + "/" + stillTexturePath + ".png";
    stillMaterial.tintIndex = (base_id.find("water") != string::npos) ? 2 : -1;
    stillMaterial.type = stillType;
    stillMaterial.aspectRatio = stillAspectRatio;

    Material flowMaterial;
    flowMaterial.name = namespace_name + ":" + base_id + "#flow~" + MaterialTextureBasename(flowTexturePath);
    flowMaterial.texturePath = "textures/" + namespace_name + "/" + flowTexturePath + ".png";
    flowMaterial.tintIndex = (base_id.find("water") != string::npos) ? 2 : -1;
    flowMaterial.type = flowType;
    flowMaterial.aspectRatio = flowAspectRatio;

    model.materials = { stillMaterial, flowMaterial };

    {
        std::unique_lock<std::shared_mutex> lock(fluidModelCacheMutex);
        fluidModelCache.emplace(key, model);
    }
    return model;
}

void AssignFluidMaterials(ModelData& model, const std::string& fluidId) {
    // Definitions and texture metadata are immutable after initialization.
    // Cache the two material templates per worker, not per water voxel.
    thread_local std::unordered_map<std::string, std::vector<Material>> templates;
    auto cached = templates.find(fluidId);
    if (cached != templates.end()) {
        model.materials = cached->second;
        return;
    }
    if (fluidId.find("minecraft:water") != string::npos) {
        for (auto& material : model.materials) {
            material.tintIndex = 2;
        }
    }
    else {
        for (auto& material : model.materials) {
            material.tintIndex = -1;
        }
    }

    // 提取基础 ID 和状态值
    std::string baseId;
    std::unordered_map<std::string, std::string> stateValues;

    size_t bracketPos = fluidId.find('[');
    if (bracketPos != std::string::npos) {
        baseId = fluidId.substr(0, bracketPos);

        std::string statePart = fluidId.substr(bracketPos + 1, fluidId.size() - bracketPos - 2);
        std::stringstream ss(statePart);
        std::string statePair;

        while (std::getline(ss, statePair, ',')) {
            size_t equalPos = statePair.find(':');
            if (equalPos != std::string::npos) {
                std::string key = statePair.substr(0, equalPos);
                std::string value = statePair.substr(equalPos + 1);
                stateValues[key] = value;
            }
        }
    }
    else {
        baseId = fluidId;
    }

    auto fluidIt = fluidDefinitions.find(baseId);
    if (fluidIt == fluidDefinitions.end()) {
        for (const auto& entry : fluidDefinitions) {
            if (stateValues.count(entry.second.property) > 0) {
                fluidIt = fluidDefinitions.find(entry.first);
                break;
            }
        }
        if (fluidIt == fluidDefinitions.end()) {
            for (const auto& entry : fluidDefinitions) {
                if (entry.second.liquid_blocks.count(baseId) > 0) {
                    fluidIt = fluidDefinitions.find(entry.first);
                    break;
                }
            }
        }
        if (fluidIt == fluidDefinitions.end()) {
            return;
        }
    }

    const FluidInfo& fluidInfo = fluidIt->second;
    std::string fluidName = fluidIt->first;
    model.materials.clear();

    size_t colonPosDef = fluidName.find(':');
    std::string ns = (colonPosDef != std::string::npos) ? fluidName.substr(0, colonPosDef) : "";
    std::string pureName = (colonPosDef != std::string::npos) ? fluidName.substr(colonPosDef + 1) : fluidName;
    std::string nsPrefix = ns.empty() ? std::string() : (ns + ":");

    float stillAspectRatio = 1.0f;
    float flowAspectRatio = 1.0f;

    Material stillFluid;
    stillFluid.name = nsPrefix + pureName + "#still~" +
        MaterialTextureBasename(fluidInfo.folder + "/" + pureName + fluidInfo.still_texture);
    stillFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.still_texture + ".png";
    stillFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    stillFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.still_texture, stillAspectRatio);
    stillFluid.aspectRatio = stillAspectRatio;

    Material flowFluid;
    flowFluid.name = nsPrefix + pureName + "#flow~" +
        MaterialTextureBasename(fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture);
    flowFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture + ".png";
    flowFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    flowFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture, flowAspectRatio);
    flowFluid.aspectRatio = flowAspectRatio;

    model.materials = { stillFluid, flowFluid };
    templates.emplace(fluidId, model.materials);
}
