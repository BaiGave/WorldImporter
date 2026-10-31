// Create 6.0.8 bezier train tracks. The blockstate model is only the end piece
// (turn=true). TrackRenderer draws the curve from the track block entity NBT
// "Connections", and only the connection with Primary=true.
#ifndef CREATECURVEDTRACK_H
#define CREATECURVEDTRACK_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:track[states]". blockEntityNbt may be null.
// Vertices stay in this block's local space (the curve may leave the block).
// Appended faces are FaceType::DO_NOT_CULL.
void AppendCreateCurvedTrack(const std::string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
