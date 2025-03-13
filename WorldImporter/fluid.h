#ifndef FLUID_H
#define FLUID_H

#include <array>
#include <string>
#include <unordered_map>
#include "model.h"

// 获取流体高度的函数
float getHeight(int level);

// 计算角落高度的函数
float getCornerHeight(float currentHeight, float NWHeight, float NHeight, float WHeight);

// 生成流体模型的函数
ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels);

// 分配流体材质的函数
void AssignFluidMaterials(ModelData& model, const std::string& fluidId);

#endif // FLUID_H