// Create 6.0.8 block-entity partials that are not in the blockstate model:
// haunted/peculiar bell bodies, and mechanical crafter cog, lid, belt, arrow.
#ifndef CREATEBELLCRAFTER_H
#define CREATEBELLCRAFTER_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// x,y,z are the block position (kinetic phase and crafter neighbour lookup).
// Appends rest-pose partials onto model. Faces are FaceType::DO_NOT_CULL.
void AppendCreateBellCrafter(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
