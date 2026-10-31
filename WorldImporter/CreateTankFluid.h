#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Static Create fluid contents, appended onto the block model (ChunkGenerator calls
// this for every Create block entity; the block id selects the shape):
// - fluid_tank / creative_fluid_tank controller with a window (FluidTankRenderer):
//   Forge TankContent{FluidName,Amount} or Fabric TankContent{Variant,Amount,Capacity}.
// - basin (BasinRenderer.renderFluids): InputTanks list, then OutputTanks list.
// - spout (SpoutRenderer fluid column): primary tank Tanks[0].
// A fluid box is the five faces except the bottom. The top uses the still texture.
void AppendCreateTankFluid(const std::string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model);
