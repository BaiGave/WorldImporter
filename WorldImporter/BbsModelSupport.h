#pragma once

#include "model.h"
#include "nbtutils.h"

// Loads Blockbuster/BBS custom .bbs.json models referenced by bbs:model block entities.
// The model files and textures live outside the mod jar in config/bbs/assets/models.
bool TryGenerateBbsModel(const NbtTagPtr& blockEntityNbt, int x, int y, int z, ModelData& outModel);
