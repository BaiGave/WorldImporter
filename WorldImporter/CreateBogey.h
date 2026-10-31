// Create 6.0.8 train bogeys. small_bogey / large_bogey blockstates are only the
// anchor plate. BogeyBlockEntityRenderer draws the frame, wheels, shafts and
// drive (BogeyStyle "create:standard"). Static pose, wheelAngle 0.
#ifndef CREATEBOGEY_H
#define CREATEBOGEY_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". Appended faces are FaceType::DO_NOT_CULL.
void AppendCreateBogey(const std::string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
