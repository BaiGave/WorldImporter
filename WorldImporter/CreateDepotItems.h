#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create depot / weighted ejector (DepotRenderer.renderItemsOf), belt controllers
// (BeltRenderer.renderItems), placard (PlacardRenderer) and basin
// (BasinRenderer items only): append held / inventory items from the block
// entity NBT, using ItemRender.
void AppendCreateDepotItems(const std::string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model);
