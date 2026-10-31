// Create 6.0.8 block-entity partials that are not in the blockstate model:
// spout nozzle, schematicannon connector/pipe, and pulley shaft/coil/rope.
#ifndef CREATESPOUTCANNONPULLEY_H
#define CREATESPOUTCANNONPULLEY_H

#include "model.h"
#include "nbtutils.h"
#include <string>

// blockName is "create:<id>[states]". blockEntityNbt may be null.
// Appends rest-pose partials onto model. Faces are FaceType::DO_NOT_CULL.
void AppendCreateSpoutCannonPulley(const std::string& blockName,
    const NbtTagPtr& blockEntityNbt, ModelData& model);

#endif
