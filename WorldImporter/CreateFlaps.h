// Create 6.0.8 funnel and belt-tunnel flaps. The blockstate model has no flaps;
// FunnelRenderer / BeltTunnelRenderer draw them. Static rest pose (flapness 0).
#ifndef CREATEFLAPS_H
#define CREATEFLAPS_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// Tunnel flaps come from BE NBT "Flaps" (list of Direction 3D data values).
// Appended faces are FaceType::DO_NOT_CULL.
void AppendCreateFlaps(const std::string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
