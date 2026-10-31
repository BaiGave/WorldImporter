// SkullModel.h: 头颅/头(实体渲染方块, 方块模型没有 elements)
#ifndef SKULL_MODEL_H
#define SKULL_MODEL_H

#include "model.h"
#include <string>

// 按原版 SkullBlockRenderer + SkullModel/DragonHeadModel/PiglinHeadModel 生成几何;
// 不是头颅方块时返回空模型。玩家头使用默认皮肤(steve)。
ModelData GenerateSkullModel(const std::string& blockName);

#endif  // SKULL_MODEL_H
