#ifndef FLUID_H
#define FLUID_H

#include <array>
#include <string>
#include <unordered_map>
#include "model.h"

struct FluidInfo {
    std::string folder;
    std::string property;          // 流体特殊属性(如waterlogged)
    std::string level_property;    // level属性名称(默认为"level")
    std::unordered_set<std::string> liquid_blocks; // 强制含水方块
    std::string still_texture;     // 静止材质路径(如"_still")
    std::string flow_texture;      // 流动材质路径(如"_flow")
};
extern std::unordered_map<std::string, FluidInfo> fluidDefinitions;
// 获取流体高度的函数
float getHeight(int level);

// 计算角落高度的函数
float getCornerHeight(float currentHeight, float NWHeight, float NHeight, float WHeight);

// 生成流体模型的函数
ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels);

// 分配流体材质的函数
void AssignFluidMaterials(ModelData& model, const std::string& fluidId);

#endif // FLUID_H