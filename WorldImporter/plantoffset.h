// plantoffset.h
// 原版方块渲染的随机平移（BlockBehaviour.OffsetType XZ / XYZ）。
// 种子是 Mth.getSeed(x, 0, z)：方块自身的 Y 不参与。
#ifndef PLANTOFFSET_H
#define PLANTOFFSET_H

#include <string_view>

struct ModelData;

// blockId 是不带命名空间的方块 id，可以带 [state]。
// 该 id 没有原版随机偏移时返回 false，且不写 ox/oy/oz。
bool VanillaPlantOffset(std::string_view blockId, int x, int z,
                        double& ox, double& oy, double& oz);

// 把偏移加到模型顶点上。ns 不是 minecraft，或 id 不在表里时不动。
// 顶点是 float；加上 (float)偏移，与原版 PoseStack 的 float 平移一致。
void ApplyVanillaPlantOffset(ModelData& model, std::string_view ns,
                             std::string_view blockId, int x, int z);

#endif
