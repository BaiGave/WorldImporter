// Create sliding doors (andesite, brass, copper, train, framed glass).
// When visible=false the game switches to RenderShape.ENTITYBLOCK_ANIMATED and
// SlidingDoorRenderer draws the door. The blockstate model is the vanilla swing.
#ifndef CREATESLIDINGDOOR_H
#define CREATESLIDINGDOOR_H

#include "model.h"
#include <string>

// Replaces model for the five Create sliding doors when visible=false.
// Returns true if model was replaced. Other blocks are left unchanged.
bool ApplyCreateSlidingDoor(const std::string& ns, const std::string& blockName, ModelData& model);

#endif
