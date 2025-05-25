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

ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels) {
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

    float v_nw = 1 - (h_nw) / 32.0f;
    float v_ne = 1 - (h_ne) / 32.0f;
    float v_se = 1 - (h_se) / 32.0f;
    float v_sw = 1 - (h_sw) / 32.0f;

    model.uvCoordinates = {
        // 下面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 31.0 / 32.0f , 0.0f, 31.0 / 32.0f,
        // 上面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f , 0.0f, 0.0f,
        // 北面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_ne, 0.0f, v_nw,
        // 南面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f, v_sw ,
        // 西面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_sw, 0.0f, v_nw,
        // 东面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f, v_ne
    };

    if (currentLevel == 0 || currentLevel == 8) {
        model.uvCoordinates = {
            // 下面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 31.0 / 32.0f , 0.0f, 31.0 / 32.0f,
            // 上面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 31.0 / 32.0f , 0.0f, 31.0 / 32.0f,
            // 北面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_ne, 0.0f, v_nw,
            // 南面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f, v_sw ,
            // 西面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_sw, 0.0f, v_nw,
            // 东面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se , 0.0f, v_ne
        };
        
        // 设置材质索引
        for (int i = 0; i < 6; i++) {
            model.faces[i].materialIndex = (i == 0 || i == 1) ? 0 : 1; // 前两个面用still材质,其他用flow材质
        }
    }
    else {
        // 计算梯度和旋转角度
        float gradientX0 = h_ne - h_nw;
        if (h_nw < 0.0f) gradientX0 = 0.0f;

        float gradientX1 = h_se - h_sw;
        if (h_sw < 0.0f) gradientX1 = 0.0f;

        float gradientZ0 = h_nw - h_sw;
        if (h_sw < 0.0f) gradientZ0 = 0.0f;

        float gradientZ1 = h_ne - h_se;
        if (h_se < 0.0f) gradientZ1 = 0.0f;

        float gradientX = (gradientX0 + gradientX1) * 16;
        float gradientZ = (gradientZ0 + gradientZ1) * 16;

        float gradientLength = static_cast<float>(sqrt(gradientX * gradientX + gradientZ * gradientZ));
        float angle = 0.0f;

        if (gradientLength > 0.0f) {
            gradientX /= gradientLength;
            gradientZ /= gradientLength;
            angle = static_cast<float>(atan2(gradientX, -gradientZ));
            // 将弧度转换为角度
            angle = static_cast<float>(angle * (180.0f / M_PI));

            // 确保角度在 [-180, 180] 范围内
            if (angle < -180.0f) angle += 360.0f;
            if (angle > 180.0f) angle -= 360.0f;

            // 将角度归一化到 [0, 360] 范围内
            angle = fmod(angle + 360.0f, 360.0f);

            // 将角度量化为 22.5 度的倍数
            angle = static_cast<float>(floor(angle / 22.5f + 0.5f) * 22.5f);
        }

        // 将角度转换为弧度
        float angleRad = angle * (M_PI / 180.0f);

        // 计算旋转矩阵
        float cosTheta = cos(angleRad);
        float sinTheta = sin(angleRad);

        // 旋转中心点
        constexpr float centerX = 0.5f;
        constexpr float centerY = 0.5f;

        // 遍历 UV 坐标数组
        for (size_t i = 0; i < model.uvCoordinates.size(); i += 8) {
            // 仅对上顶面的 UV 坐标进行旋转
            if (i >= 8 && i < 16) { // 上顶面对应的 UV 坐标范围
                // 提取 4 个 UV 顶点
                float u0 = model.uvCoordinates[i];
                float v0 = model.uvCoordinates[i + 1];
                float u1 = model.uvCoordinates[i + 2];
                float v1 = model.uvCoordinates[i + 3];
                float u2 = model.uvCoordinates[i + 4];
                float v2 = model.uvCoordinates[i + 5];
                float u3 = model.uvCoordinates[i + 6];
                float v3 = model.uvCoordinates[i + 7];

                // 旋转 UV 坐标
                auto rotateUV = [&](float& u, float& v) {
                    const float relU = u - centerX;
                    const float relV = v - centerY;
                    const float newU = relU * cosTheta - relV * sinTheta + centerX;
                    const float newV = relU * sinTheta + relV * cosTheta + centerY;
                    u = newU;
                    v = newV;
                    };

                rotateUV(u0, v0);
                rotateUV(u1, v1);
                rotateUV(u2, v2);
                rotateUV(u3, v3);

                // 将 x 值从 [1 到 0] 缩放到 [1 到 31/32]
                auto scaleU = [](float u) {
                    return 1.0f - (1.0f - 31.0f / 32.0f) * u;
                    };

                v0 = scaleU(v0);
                v1 = scaleU(v1);
                v2 = scaleU(v2);
                v3 = scaleU(v3);

                // 更新 UV 坐标
                model.uvCoordinates[i] = u0;
                model.uvCoordinates[i + 1] = v0;
                model.uvCoordinates[i + 2] = u1;
                model.uvCoordinates[i + 3] = v1;
                model.uvCoordinates[i + 4] = u2;
                model.uvCoordinates[i + 5] = v2;
                model.uvCoordinates[i + 6] = u3;
                model.uvCoordinates[i + 7] = v3;
            }
        }
        
        // 设置材质索引
        model.faces[0].materialIndex = 0; // 底面使用still材质
        for (int i = 1; i < 6; i++) {
            model.faces[i].materialIndex = 1; // 其他面使用flow材质
        }
    }

    // 添加材质
    // 静止水材质(still)- 用于顶部和底部
    Material stillMaterial;
    stillMaterial.name = "water_still";
    stillMaterial.texturePath = "textures/minecraft/block/water_still.png";
    stillMaterial.tintIndex = 2; // 水的色调索引
    stillMaterial.type = DetectMaterialType("minecraft", "block/water_still");

    // 流动水材质(flow)- 用于侧面
    Material flowMaterial;
    flowMaterial.name = "water_flow"; 
    flowMaterial.texturePath = "textures/minecraft/block/water_flow.png";
    flowMaterial.tintIndex = 2; // 水的色调索引
    flowMaterial.type = DetectMaterialType("minecraft", "block/water_flow");

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
    
    // 创建静止流体材质
    Material stillFluid;
    stillFluid.name = fluidInfo.folder + "/" + pureName + fluidInfo.still_texture;
    stillFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.still_texture + ".png";
    stillFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    stillFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.still_texture);
    
    // 创建流动流体材质
    Material flowFluid;
    flowFluid.name = fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture;
    flowFluid.texturePath = "textures/" + ns + "/" + fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture + ".png";
    flowFluid.tintIndex = (pureName.find("water") != std::string::npos) ? 2 : -1;
    flowFluid.type = DetectMaterialType(ns, fluidInfo.folder + "/" + pureName + fluidInfo.flow_texture);
    
    model.materials = { stillFluid, flowFluid };
}