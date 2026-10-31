#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create boiler (fluid tank controller, Window off, engines/whistles attached):
// append FluidTankRenderer.renderAsBoiler gauges on every unoccluded horizontal side.
void AppendCreateBoilerGauge(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);
