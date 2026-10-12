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

// 原版 FluidState#getOwnHeight（单位：格）：
//   水源(level 0) = 8/9；流动(level 1..7) = (8-level)/9；下落(level >= 8) = 8/9
float GetFluidOwnHeight(int level);

// 原版 calculateAverageHeight：由中心高度、对角方块高度与两个相邻侧面高度求角高（单位：格）。
// -1 表示固体邻居（不参与平均），0 表示空气/非固体。
float CalculateFluidCornerHeight(float selfHeight, float cornerHeight, float sideA, float sideB);
// 原版流体渲染参数（由 ChunkGenerator 依据邻居方块与原版规则计算）
struct FluidModelParams {
    float selfHeight = 0.0f;            // 中心 getHeight（上方同种流体时 = 1）
    float sideHeight[4] = { 0, 0, 0, 0 };   // 北、南、西、东
    float cornerHeight[4] = { 0, 0, 0, 0 }; // 西北、东北、东南、西南
    float flowX = 0.0f;                 // 原版 getFlow 水平分量（未归一化也可）
    float flowZ = 0.0f;
    bool falling = false;
    bool downCanRender = false;         // 底面渲染：侧面底部内缩 0.001（原版）
    bool topCanRender = false;          // 顶面渲染：四个角高统一 -0.001（原版）
    bool keepFace[6] = { true, true, true, true, true, true }; // 底面,顶面,北,南,西,东
};

// 生成流体模型：几何/UV/贴图选择按原版 FluidRenderer。
// 材质名/贴图路径沿用现有约定（still/flow 两种材质，索引 0/1）。
ModelData GenerateFluidModel(const FluidModelParams& params, const std::string& fluidId = "minecraft:water");

// 计算四个角高（单位：格；顺序 西北、东北、东南、西南）。
// 原版：中心高度 >= 1（上方是同种流体）时四角直接满高 1.0，否则逐角做加权平均。
void ComputeFluidCornerHeights(const FluidModelParams& params, float outCorner[4]);

// 为流体模型分配材质
void AssignFluidMaterials(ModelData& model, const std::string& fluidId);

#endif // FLUID_H
