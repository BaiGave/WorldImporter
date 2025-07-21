#ifndef FLUID_H
#define FLUID_H

#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include "model.h"

// 流体定义信息结构
struct FluidInfo {
    std::string folder;               // 纹理文件夹，默认为"block"
    std::string still_texture;        // 静止纹理后缀，默认为"_still"
    std::string flow_texture;         // 流动纹理后缀，默认为"_flow"
    std::string property;             // 识别流体属性，默认为空
    std::string level_property;       // 液位属性名，默认为"level" 
    std::unordered_set<std::string> liquid_blocks; // 关联方块ID列表
};

// 全局流体定义数据
extern std::unordered_map<std::string, FluidInfo> fluidDefinitions;

// 获取流体高度的函数
float getHeight(int level);

// 计算角落高度的函数
float getCornerHeight(float currentHeight, float NWHeight, float NHeight, float WHeight);

// 使用fluidId和周围的液位生成流体模型
ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels, const std::string& fluidId = "minecraft:water");

// 为流体模型分配材质
void AssignFluidMaterials(ModelData& model, const std::string& fluidId);

#endif // FLUID_H