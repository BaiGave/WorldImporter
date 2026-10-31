#pragma once

#include "model.h"
#include "nbtutils.h"

// Adds the vanilla beacon beam (static frame, animation time 0) onto the
// beacon block model. Does not replace the block model.
void AppendBeaconBeam(ModelData& blockModel, const NbtTagPtr& blockEntityNbt, int x, int y, int z);
