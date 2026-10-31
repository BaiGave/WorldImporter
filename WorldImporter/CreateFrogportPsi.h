#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create package_frogport (FrogportRenderer) and portable storage/fluid interfaces
// (PortableStorageInterfaceRenderer): append the BE-rendered partials at their rest pose.
void AppendCreateFrogportPsi(const std::string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model);
