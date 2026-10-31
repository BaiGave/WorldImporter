#ifndef OCCLUSION_H
#define OCCLUSION_H

#include <cstddef>

// 每个唯一 block state 的遮挡信息（运行时自建，等价于原版的
// BlockState#canOcclude / 几何形状判定，不再依赖手工维护的方块名单）。
//
// 判定来源：该 block state 的模型几何 + 材质纹理的 alpha。
//   occludes      : 6 个方向都存在恰好位于方块界面、完整覆盖 1x1 且材质全不透明的面
//                   （≈ 原版 canOcclude()，水面/方块面剔除用它）
//   solidLike     : 非空气且具有实体几何（完整立方体或任一界面有面，≈ blocksMotion()，
//                   用于水面高度平均里 isSolid 的近似）
//   selfFaceFull  : 自身模型在同一平面上的面并集是否完整覆盖该方向的 1x1 面
//                   （≈ 原版 FluidRenderer#isFaceOccludedBySelf，用于含水方块的水面自遮挡）
struct BlockOcclusion {
    bool hasModel = false;   // 是否解析到模型
    bool occludes = false;   // 完整不透明立方体（遮挡体）
    bool solidLike = false;  // 实体几何近似
    // 方向索引与 FaceType 一致：UP=0, DOWN=1, NORTH=2, SOUTH=3, WEST=4, EAST=5
    bool selfFaceFull[6] = { false, false, false, false, false, false };
};

// 对全局方块调色板从 fromIndex 起增量建表。
// 必须在批次加载阶段（模型解析完成、缓存冻结前）串行调用；模型阶段只读。
void BuildBlockOcclusionTable(size_t fromIndex);

// 按全局调色板下标查询；越界或未建表返回默认（不遮挡）。
const BlockOcclusion& GetBlockOcclusion(int blockId);

#endif // OCCLUSION_H
