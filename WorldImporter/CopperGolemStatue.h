#pragma once
#include <string>
#include "model.h"

// 铜傀儡雕像(1.21.9+): 方块模型只有 particle, 由 CopperGolemStatueBlockRenderer 绘制。
// 非铜傀儡雕像时返回空模型。
ModelData GenerateCopperGolemStatueModel(const std::string& blockName);
