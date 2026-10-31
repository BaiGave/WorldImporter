// Create 6.0.8 chain conveyor block-entity parts that are not in the blockstate
// model: shaft, wheel, per-connection guard, and the chain strip between wheels.
#ifndef CREATECHAINCONVEYOR_H
#define CREATECHAINCONVEYOR_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// Speed is exported at rest (0): no kinetic spin, connection direction not reversed.
// Appends parts onto model. Faces are FaceType::DO_NOT_CULL.
void AppendCreateChainConveyor(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
