#ifndef CAMPFIRE_ITEMS_H
#define CAMPFIRE_ITEMS_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// CampfireRenderer: cooking Items on campfire / soul_campfire.
bool AppendCampfireItems(ModelData& blockModel, const NbtTagPtr& blockEntityNbt,
                         const std::string& blockNameWithState, int x, int y, int z);

#endif
