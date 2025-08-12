#include "Fluid.h"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include "block.h"
#include <unordered_map>
#include <mutex>
#include "model.h"

using namespace std;

// 流体注册数据
std::unordered_map<std::string, FluidInfo> fluidDefinitions;
// 模型缓存
static std::unordered_map<int, ModelData> fluidModelCache;
static std::mutex fluidModelCacheMutex; 

float getHeight(int level) {
    if (level == 0)
        return 14.166666f; // 水源

    if (level == -1)
        return 0.0f; // 空气

    if (level == -2)
        return -1.0f; // 一般方块

    if (level == 8)
        return 16.0f; // 流动水

    // 中间值的线性插值计算
    return 2.0f + (12.0f / 7.0f) * (7 - level);
}

float getCornerHeight(float currentHeight, float NWHeight, float NHeight, float WHeight) {
    float totalWeight = 0.0f;
    float res = 0.0f;
    bool sourceBlock = false;

    if (currentHeight >= 16.0f || NWHeight >= 16.0f || NHeight >= 16.0f || WHeight >= 16.0f) {
        return 16.0f;
    }

    if (currentHeight == 14.166666f) {
        res += currentHeight * 11.0f;
        totalWeight += 11.0f;
        sourceBlock = true;
    }
    if (NWHeight == 14.166666f) {
        res += NWHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }
    if (NHeight == 14.166666f) {
        res += NHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }
    if (WHeight == 14.166666f) {
        res += WHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }

    if (sourceBlock) {
        if (currentHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (NWHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (NHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (WHeight == 0.0f) {
            totalWeight += 1.0f;
        }
    }
    else {
        if (currentHeight >= 0.0f) {
            res += currentHeight;
            totalWeight += 1.0f;
        }
        if (NWHeight >= 0.0f) {
            res += NWHeight;
            totalWeight += 1.0f;
        }
        if (NHeight >= 0.0f) {
            res += NHeight;
            totalWeight += 1.0f;
        }
        if (WHeight >= 0.0f) {
            res += WHeight;
            totalWeight += 1.0f;
        }
    }

    return (totalWeight == 0.0f) ? 0.0f : res / totalWeight;
}

ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels, const std::string& fluidId) {
    ModelData model;

    // 获取当前方块的液位和周围液位的高度
    int currentLevel = fluidLevels[0];
    int northLevel = fluidLevels[1];    // 北
    int southLevel = fluidLevels[2];    // 南
    int eastLevel = fluidLevels[3];     // 东
    int westLevel = fluidLevels[4];     // 西
    int northeastLevel = fluidLevels[5]; // 东北
    int northwestLevel = fluidLevels[6]; // 西北
    int southeastLevel = fluidLevels[7]; // 东南
    int southwestLevel = fluidLevels[8]; // 西南
    int aboveLevel = fluidLevels[9];     // 上方

    size_t key = 0;
    for (int level : fluidLevels) {
        key = (key << 3) ^ (level + (level << 5));
    }
    // 结合流体ID，确保不同流体类型有不同的缓存键值
    key = key ^ std::hash<std::string>{}(fluidId);

    // Lock the mutex to safely access the cache
    std::lock_guard<std::mutex> lock(fluidModelCacheMutex);

    // 检查缓存中是否存在该模型
    if (fluidModelCache.find(key) != fluidModelCache.end()) {
        return fluidModelCache[key]; // 返回缓存中的模型
    }

    float currentHeight = getHeight(currentLevel);
    float northHeight = getHeight(northLevel);
    float southHeight = getHeight(southLevel);
    float eastHeight = getHeight(eastLevel);
    float westHeight = getHeight(westLevel);
    float northeastHeight = getHeight(northeastLevel);
    float northwestHeight = getHeight(northwestLevel);
    float southeastHeight = getHeight(southeastLevel);
    float southwestHeight = getHeight(southwestLevel);

    // 计算四个上顶点的高度
    float h_nw = getCornerHeight(currentHeight, northwestHeight, northHeight, westHeight) / 16.0f;
    float h_ne = getCornerHeight(currentHeight, northeastHeight, northHeight, eastHeight) / 16.0f;
    float h_se = getCornerHeight(currentHeight, southeastHeight, southHeight, eastHeight) / 16.0f;
    float h_sw = getCornerHeight(currentHeight, southwestHeight, southHeight, westHeight) / 16.0f;
    h_nw = ceil(h_nw * 10.0f) / 10.0f;
    h_ne = ceil(h_ne * 10.0f) / 10.0f;
    h_se = ceil(h_se * 10.0f) / 10.0f;
    h_sw = ceil(h_sw * 10.0f) / 10.0f;

    model.vertices = {
        // 底面 (bottom) - Y轴负方向
        0.0f, 0.0f, 0.0f,       // 0
        1.0f, 0.0f, 0.0f,       // 1
        1.0f, 0.0f, 1.0f,       // 2
        0.0f, 0.0f, 1.0f,       // 3

        // 顶面 (top) - Y轴正方向
        0.0f, h_nw, 0.0f, // 4 西北角
        1.0f, h_ne, 0.0f, // 5 东北角
        1.0f, h_se, 1.0f, // 6 东南角
        0.0f, h_sw, 1.0f, // 7 西南角

        // 北面 (north) - Z轴负方向(保持原顺序正确)
        0.0f, 0.0f, 0.0f, // 8
        1.0f, 0.0f, 0.0f, // 9
        1.0f, h_ne, 0.0f, // 10
        0.0f, h_nw, 0.0f, // 11

        // 南面 (south) - Z轴正方向,需反转顺序
        0.0f, 0.0f, 1.0f, // 12
        1.0f, 0.0f, 1.0f, // 13
        1.0f, h_se, 1.0f, // 14
        0.0f, h_sw, 1.0f, // 15

        // 西面 (west) - X轴负方向(保持原顺序正确)
        0.0f, 0.0f, 0.0f, // 16
        0.0f, 0.0f, 1.0f, // 17
        0.0f, h_sw, 1.0f, // 18
        0.0f, h_nw, 0.0f, // 19

        // 东面 (east) - X轴正方向,需反转顺序
        1.0f, 0.0f, 0.0f, // 20
        1.0f, 0.0f, 1.0f, // 21
        1.0f, h_se, 1.0f, // 22
        1.0f, h_ne, 0.0f  // 23
    };

    // 创建模型的六个面(底面,顶面,北面,南面,西面,东面)
    model.faces.resize(6);
    
    // 底面 (y-)
    model.faces[0].vertexIndices = { 0, 3, 2, 1 };
    model.faces[0].uvIndices = { 0, 3, 2, 1 };
    model.faces[0].faceDirection = DOWN;
    model.faces[0].materialIndex = 0; // still材质
    
    // 顶面 (y+)
    model.faces[1].vertexIndices = { 4, 7, 6, 5 };
    model.faces[1].uvIndices = { 4, 7, 6, 5 };
    // 根据上方方块决定顶面是否剔除
    model.faces[1].faceDirection = (aboveLevel < 0) ? DO_NOT_CULL : UP;
    model.faces[1].materialIndex = 1; // flow材质
    
    // 北面 (z-)
    model.faces[2].vertexIndices = { 8, 11, 10, 9 };
    model.faces[2].uvIndices = { 8, 11, 10, 9 };
    model.faces[2].faceDirection = NORTH;
    model.faces[2].materialIndex = 1; // flow材质
    
    // 南面 (z+)
    model.faces[3].vertexIndices = { 12, 13, 14, 15 };
    model.faces[3].uvIndices = { 12, 13, 14, 15 };
    model.faces[3].faceDirection = SOUTH;
    model.faces[3].materialIndex = 1; // flow材质
    
    // 西面 (x-)
    model.faces[4].vertexIndices = { 16, 17, 18, 19 };
    model.faces[4].uvIndices = { 16, 17, 18, 19 };
    model.faces[4].faceDirection = WEST;
    model.faces[4].materialIndex = 1; // flow材质
    
    // 东面 (x+)
    model.faces[5].vertexIndices = { 20, 23, 22, 21 };
    model.faces[5].uvIndices = { 20, 23, 22, 21 };
    model.faces[5].faceDirection = EAST;
    model.faces[5].materialIndex = 1; // flow材质

    // 提取流体ID的命名空间和基础名
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
    
    // 寻找流体定义
    std::string stillTexturePath = "block/" + base_id + "_still";
    std::string flowTexturePath = "block/" + base_id + "_flow";
    
    // 如果有对应的流体定义，使用其中的路径
    auto fluidIt = fluidDefinitions.find(namespace_name + ":" + base_id);
    if (fluidIt != fluidDefinitions.end()) {
        const FluidInfo& info = fluidIt->second;
        stillTexturePath = info.folder + "/" + base_id + info.still_texture;
        flowTexturePath = info.folder + "/" + base_id + info.flow_texture;
    }
    
    // 获取流体纹理的长宽比
    float stillAspectRatio = 1.0f;
    float flowAspectRatio = 1.0f;

    // 获取材质的长宽比
    MaterialType stillType = DetectMaterialType(namespace_name, stillTexturePath, stillAspectRatio);
    MaterialType flowType = DetectMaterialType(namespace_name, flowTexturePath, flowAspectRatio);

    // 确保长宽比至少为1，避免除以0的错误
    if (stillAspectRatio < 1.0f) {
        stillAspectRatio = 1.0f;
    }
    if (flowAspectRatio < 1.0f) {
        flowAspectRatio = 1.0f;
    }
    float temp_offset = (1.0 / flowAspectRatio) * (flowAspectRatio - 1);
    // 计算流动贴图的V坐标缩放因子
    float v_nw = h_nw / flowAspectRatio + temp_offset;
    float v_ne = h_ne / flowAspectRatio + temp_offset;
    float v_se = h_se / flowAspectRatio + temp_offset;
    float v_sw = h_sw / flowAspectRatio + temp_offset;
    
    // 单独计算静止贴图的UV坐标
    float still_bottom_v = (stillAspectRatio - 1.0f) / stillAspectRatio;

    model.uvCoordinates = {
        // 下面(使用静止贴图的长宽比)
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, still_bottom_v, 0.0f, still_bottom_v,
        // 上面(使用静止贴图的长宽比)
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, still_bottom_v, 0.0f, still_bottom_v,
        // 北面(使用流动贴图的长宽比)
        0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_ne, 0.0f, v_nw,
        // 南面
        0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_se, 0.0f, v_sw,
        // 西面
        0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_sw, 0.0f, v_nw,
        // 东面
        0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_se, 0.0f, v_ne
    };

    if (currentLevel == 0 || currentLevel == 8) {
        model.uvCoordinates = {
            // 下面(使用静止贴图的长宽比)
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, still_bottom_v, 0.0f, still_bottom_v,
            // 上面(使用静止贴图的长宽比)
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, still_bottom_v, 0.0f, still_bottom_v,
            // 北面(使用流动贴图的长宽比)
            0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_ne, 0.0f, v_nw,
            // 南面
            0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_se, 0.0f, v_sw,
            // 西面
            0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_sw, 0.0f, v_nw,
            // 东面
            0.0f, 0.0f + temp_offset, 1.0f, 0.0f + temp_offset, 1.0f, v_se, 0.0f, v_ne
        };
        
        // 侧面UV缩放：与流动顶面一致，将U围绕0.5缩放，将V围绕当前帧中心缩放
        {
            const float centerU = 0.5f;
            const float maxV = 1.0f / flowAspectRatio;
            const float startV = 1.0f - maxV;
            const float vCenter = startV + maxV * 0.5f;
            const float uvScale = 0.5f; // 与顶面的0.25振幅等效
            for (size_t idx = 16; idx <= 40; idx += 8) { // 四个侧面：北、南、西、东
                model.uvCoordinates[idx + 0] = centerU + (model.uvCoordinates[idx + 0] - centerU) * uvScale;
                model.uvCoordinates[idx + 1] = vCenter + (model.uvCoordinates[idx + 1] - vCenter) * uvScale;
                model.uvCoordinates[idx + 2] = centerU + (model.uvCoordinates[idx + 2] - centerU) * uvScale;
                model.uvCoordinates[idx + 3] = vCenter + (model.uvCoordinates[idx + 3] - vCenter) * uvScale;
                model.uvCoordinates[idx + 4] = centerU + (model.uvCoordinates[idx + 4] - centerU) * uvScale;
                model.uvCoordinates[idx + 5] = vCenter + (model.uvCoordinates[idx + 5] - vCenter) * uvScale;
                model.uvCoordinates[idx + 6] = centerU + (model.uvCoordinates[idx + 6] - centerU) * uvScale;
                model.uvCoordinates[idx + 7] = vCenter + (model.uvCoordinates[idx + 7] - vCenter) * uvScale;
            }
        }
        
        // 设置材质索引
        for (int i = 0; i < 6; i++) {
            model.faces[i].materialIndex = (i == 0 || i == 1) ? 0 : 1; // 前两个面用still材质,其他用flow材质
        }
    }
    else {
        // 使用和MC一致的流向计算方法
        // 计算X方向和Z方向梯度
        float gradientX = (h_ne + h_se - h_nw - h_sw) * 0.5f;
        float gradientZ = (h_sw + h_se - h_nw - h_ne) * 0.5f;

        // 计算流向角度 (使用和MC类似的方法)
        float angle = atan2(gradientZ, gradientX) - (M_PI / 2.0f); // 注意这里减去PI/2
        
        // 计算UV旋转偏移
        float sinAngle = sin(angle) * 0.25f;
        float cosAngle = cos(angle) * 0.25f;
        
        // 中心点
        constexpr float centerU = 0.5f;
        constexpr float centerV = 0.5f;
        // 定义v坐标的最大值，根据材质长宽比计算
        float maxV = 1.0f / flowAspectRatio;
        // 定义v坐标起点（最上面一帧的起始位置）
        float startV = 1.0f - maxV;
        
        // 遍历 UV 坐标数组
        for (size_t i = 0; i < model.uvCoordinates.size(); i += 8) {
            // 仅对上顶面的 UV 坐标进行旋转
            if (i >= 8 && i < 16) { // 上顶面对应的 UV 坐标范围
                // 纹理坐标计算 - 基于MC的方式
                // 左上角 (西北)
                model.uvCoordinates[i] = centerU + (-cosAngle - sinAngle);
                model.uvCoordinates[i + 1] = startV + (centerV + (-cosAngle + sinAngle)) * maxV;
                
                // 右上角 (东北)
                model.uvCoordinates[i + 2] = centerU + (cosAngle - sinAngle);
                model.uvCoordinates[i + 3] = startV + (centerV + (-cosAngle - sinAngle)) * maxV;
                
                // 右下角 (东南)
                model.uvCoordinates[i + 4] = centerU + (cosAngle + sinAngle);
                model.uvCoordinates[i + 5] = startV + (centerV + (cosAngle - sinAngle)) * maxV;
                
                // 左下角 (西南)
                model.uvCoordinates[i + 6] = centerU + (-cosAngle + sinAngle);
                model.uvCoordinates[i + 7] = startV + (centerV + (cosAngle + sinAngle)) * maxV;
            }
        }
        
        // 侧面UV缩放：与流动顶面一致，将U围绕0.5缩放，将V围绕当前帧中心缩放
        {
            const float vCenter = startV + maxV * 0.5f;
            const float uvScale = 0.5f; // 与顶面的0.25振幅等效
            for (size_t idx = 16; idx <= 40; idx += 8) { // 四个侧面：北、南、西、东
                model.uvCoordinates[idx + 0] = centerU + (model.uvCoordinates[idx + 0] - centerU) * uvScale;
                model.uvCoordinates[idx + 1] = vCenter + (model.uvCoordinates[idx + 1] - vCenter) * uvScale;
                model.uvCoordinates[idx + 2] = centerU + (model.uvCoordinates[idx + 2] - centerU) * uvScale;
                model.uvCoordinates[idx + 3] = vCenter + (model.uvCoordinates[idx + 3] - vCenter) * uvScale;
                model.uvCoordinates[idx + 4] = centerU + (model.uvCoordinates[idx + 4] - centerU) * uvScale;
                model.uvCoordinates[idx + 5] = vCenter + (model.uvCoordinates[idx + 5] - vCenter) * uvScale;
                model.uvCoordinates[idx + 6] = centerU + (model.uvCoordinates[idx + 6] - centerU) * uvScale;
                model.uvCoordinates[idx + 7] = vCenter + (model.uvCoordinates[idx + 7] - vCenter) * uvScale;
            }
        }
        
        
        // 设置材质索引
        model.faces[0].materialIndex = 0; // 底面使用still材质
        for (int i = 1; i < 6; i++) {
            model.faces[i].materialIndex = 1; // 其他面使用flow材质
        }
    }

    // 添加材质
    // 静止流体材质(still)- 用于顶部和底部
    Material stillMaterial;
    stillMaterial.name = base_id + "_still";
    stillMaterial.texturePath = "textures/" + namespace_name + "/" + stillTexturePath + ".png";
    stillMaterial.tintIndex = (base_id.find("water") != string::npos) ? 2 : -1; // 只对水使用色调索引2
    stillMaterial.type = stillType;
    stillMaterial.aspectRatio = stillAspectRatio; // 设置材质长宽比

    // 流动流体材质(flow)- 用于侧面
    Material flowMaterial;
    flowMaterial.name = base_id + "_flow"; 
    flowMaterial.texturePath = "textures/" + namespace_name + "/" + flowTexturePath + ".png";
    flowMaterial.tintIndex = (base_id.find("water") != string::npos) ? 2 : -1; // 只对水使用色调索引2
    flowMaterial.type = flowType;
    flowMaterial.aspectRatio = flowAspectRatio; // 设置材质长宽比

    model.materials = { stillMaterial, flowMaterial };

    fluidModelCache[key] = model;
    return model;
}

void AssignFluidMaterials(ModelData& model, const std::string& fluidId) {
    if (fluidId.find("minecraft:water") != string::npos) {
        // 为所有材质设置水的着色索引
        for (auto& material : model.materials) {
            material.tintIndex = 2;
        }
    }
    else
    {
        // 为所有材质设置无着色
        for (auto& material : model.materials) {
            material.tintIndex = -1;
        }
    }
    // 提取基础 ID 和状态值(如果有多个状态值)
    std::string baseId;
    std::unordered_map<std::string, std::string> stateValues;

    size_t bracketPos = fluidId.find('[');
    if (bracketPos != std::string::npos) {
        baseId = fluidId.substr(0, bracketPos);

        std::string statePart = fluidId.substr(bracketPos + 1, fluidId.size() - bracketPos - 2); // Remove the closing ']'
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

    // 尝试查找流体定义
    auto fluidIt = fluidDefinitions.find(baseId);
    if (fluidIt == fluidDefinitions.end()) {
        // 尝试匹配 level_property
        for (const auto& entry : fluidDefinitions) {
            if (stateValues.count(entry.second.property) > 0) {
                fluidIt = fluidDefinitions.find(entry.first);
                break;
            }
        }

        if (fluidIt == fluidDefinitions.end()) {
            // 尝试匹配 liquid_blocks
            for (const auto& entry : fluidDefinitions) {
                if (entry.second.liquid_blocks.count(baseId) > 0) {
                    fluidIt = fluidDefinitions.find(entry.first);
                    break;
                }
            }
        }

        if (fluidIt == fluidDefinitions.end()) {
            // 如果仍然没找到,直接返回
            return;
        }
    }

    const FluidInfo& fluidInfo = fluidIt->second;
    std::string fluidName = fluidIt->first;
    // 清空旧数据
    model.materials.clear();

    // 使用fluidName解析命名空间和纯名称
    size_t colonPosDef = fluidName.find(':');
    std::string ns = (colonPosDef != std::string::npos) ? fluidName.substr(0, colonPosDef) : "";
    std::string pureName = (colonPosDef != std::string::npos) ? fluidName.substr(colonPosDef + 1) : fluidName;
    
    // 获取材质长宽比
    float stillAspectRatio = 1.0f;
    float flowAspectRatio = 1.0f;
    
    // 创建静止流体材质
    Material stillFluid;
    stillFluid.name = fluidInfo.folder + "/" + pureName + fluidInfo.still_texture;
    stillFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.still_texture + ".png";
    stillFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    stillFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.still_texture, stillAspectRatio);
    stillFluid.aspectRatio = stillAspectRatio;
    
    // 创建流动流体材质
    Material flowFluid;
    flowFluid.name = fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture;
    flowFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture + ".png";
    flowFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    flowFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture, flowAspectRatio);
    flowFluid.aspectRatio = flowAspectRatio;
    
    model.materials = { stillFluid, flowFluid };
}