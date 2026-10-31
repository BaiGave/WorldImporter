// Create 6.0.8 postbox flag and contraption-controls button + indicator.
// The blockstate models omit these; PostboxRenderer and ContraptionControlsRenderer
// draw them. Static rest pose: flag value 0, button value 0, indicator 0.
#ifndef CREATEPOSTBOXCONTROLS_H
#define CREATEPOSTBOXCONTROLS_H

#include "model.h"
#include <string>

// blockName is "create:<id>[states]". Appended faces are FaceType::DO_NOT_CULL.
void AppendCreatePostboxControls(const std::string& blockName, ModelData& model);

#endif
