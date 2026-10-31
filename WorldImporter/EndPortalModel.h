#ifndef END_PORTAL_MODEL_H
#define END_PORTAL_MODEL_H

#include "model.h"
#include <string>

// 末地传送门 / 折跃门：方块模型只有 particle，原版用着色器画。
// blockName 形如 "minecraft:end_portal" 或 "end_gateway"，可带 [状态]；缺命名空间视为 minecraft。
// 其他方块返回空 ModelData。
ModelData GenerateEndPortalModel(const std::string& blockName);

#endif
