// PlayerSkin.h: resolve a minecraft:skull profile to an exported skin texture.
// Empty string means "keep the default Steve material".
#ifndef PLAYER_SKIN_H
#define PLAYER_SKIN_H

#include "nbtutils.h"
#include <string>

// Material texture path (textures/<ns>/<path>.png) or "" for the default skin.
std::string ResolvePlayerSkinTexture(const NbtTagPtr& blockEntityNbt);

#endif
