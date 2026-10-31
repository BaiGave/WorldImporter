// BannerModel.h: 旗帜(实体渲染方块, 方块模型没有 elements)
#ifndef BANNER_MODEL_H
#define BANNER_MODEL_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// 按原版 BannerRenderer + BannerModel/BannerFlagModel 生成立式/墙上旗帜。
// 旗面贴图按底色 + 方块实体图案(Patterns / patterns)烘焙成一张 PNG。
// 不是旗帜时返回 false。
bool TryGenerateBannerModel(const std::string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& outModel);

#endif  // BANNER_MODEL_H
