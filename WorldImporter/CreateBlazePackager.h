// Create 6.0.8 block-entity partials that are not posed by the blockstate model:
// blaze burner head, goggles, train hat, rods and flame, and packager hatch/tray.
#ifndef CREATEBLAZEPACKAGER_H
#define CREATEBLAZEPACKAGER_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// x,y,z are the block position (block-above lookup for the burner head).
// Appends the static rest pose onto model. Faces are FaceType::DO_NOT_CULL.
void AppendCreateBlazePackager(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
