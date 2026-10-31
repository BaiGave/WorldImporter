// HangingSignModel.h: 1.20-1.21 悬挂告示牌(方块模型只有 particle, 无 elements)
#ifndef HANGING_SIGN_MODEL_H
#define HANGING_SIGN_MODEL_H

#include "model.h"
#include <string>

// 非原版悬挂告示牌或贴图缺失时返回空 ModelData
ModelData GenerateHangingSignModel(const std::string& blockName);

#endif  // HANGING_SIGN_MODEL_H
