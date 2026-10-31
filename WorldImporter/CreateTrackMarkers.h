// Create 6.0.8 train track markers. Station flag, signal lamp, and the
// track overlays are partial models, not part of the blockstate. Static pose:
// flag value 0 (progress 0), no trains, no assembly overlay.
#ifndef CREATETRACKMARKERS_H
#define CREATETRACKMARKERS_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// x,y,z is the marker block. Target-track lookup uses GetBlockId/GetBlockById.
// Appended faces are FaceType::DO_NOT_CULL.
void AppendCreateTrackMarkers(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
