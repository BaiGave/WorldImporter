#pragma once

#include "model.h"
#include "nbtutils.h"
#include <string>

// Create filter-slot items (FilteringRenderer.renderOnBlockEntity ->
// ValueBoxTransform.transform then ValueBoxRenderer.renderItemIntoValueBox).
// Appends the item shown in a block's filter slot. Empty / air filters are skipped.
void AppendCreateFilterItems(const std::string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model);
