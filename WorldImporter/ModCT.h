// ModCT.h — generic mod connected-texture hooks (industrial adapters)
//
// Prefer Continuity/OptiFine properties and CTM-mod .png.mcmeta (already in CTM.cpp).
// This module adds:
//   1) Cross-block connect groups (Mek extra.connect_to, optional JSON tables)
//   2) A thin IModCTProvider registry so Create-like sprite-shift mods do not
//      each need a private Apply* path forever.
//   3) FramedBlocks camo retexture (block / fluid / double camo_two)
//   4) Per-position camo connect aliases so framed@mek camo joins Mek CT groups
//   5) Create/Copycats+ Material camo retexture
//
// Special-case geometry (Create kinetics, Mek transmitters, IE wires) stays
// outside this module (see SpecialBlock / CableMesher).

#ifndef MODCT_H
#define MODCT_H

#include <string>
#include <vector>
#include <functional>
#include "model.h"
#include "nbtutils.h"

struct IModCTProvider {
    virtual ~IModCTProvider() = default;
    virtual const char* Name() const = 0;
    virtual bool AppliesTo(const std::string& ns, const std::string& blockName) const = 0;
    virtual bool Apply(ModelData& model, const std::string& ns,
                       const std::string& blockName, int x, int y, int z) = 0;
};

void InitializeModCT();

void RegisterModCTProvider(IModCTProvider* provider); // non-owning; must outlive export

bool TryApplyModCTProviders(ModelData& model, const std::string& ns,
                            const std::string& blockName, int x, int y, int z);

bool ModCT_BlocksConnect(const std::string& curBaseName,
                         const std::string& neighborBaseName);

bool ModCT_HasConnectGroupForTexture(const std::string& textureNs,
                                     const std::string& texturePath);

bool ModCT_TextureConnectsTo(const std::string& textureNs,
                             const std::string& texturePath,
                             const std::string& neighborBaseName);

void ModCT_AddConnectGroup(const std::vector<std::string>& blockIds);

// Per-position camo connect identity (framed block → camo block id).
void ModCT_SetPositionCamo(int x, int y, int z, const std::string& camoBlockId);
std::string ModCT_GetPositionCamo(int x, int y, int z); // empty if none
void ModCT_ClearPositionCamos();

// FramedBlocks camo retexture. blockLocalName selects double-split axis when
// camo_two is present. Registers ModCT_SetPositionCamo for Mek CT joining.
bool ApplyFramedCamoToBlockModel(ModelData& model, const NbtTagPtr& beNbt,
                                 const std::string& blockLocalName,
                                 int x, int y, int z);

// Create / Copycats+ material camo. Reads BE "Material" BlockState compound
// (Name + Properties). Retextures copycat_base materials, or rebuilds a full
// cube when the blockstate model is air/empty.
bool ApplyCopycatCamoToBlockModel(ModelData& model, const NbtTagPtr& beNbt,
                                  const std::string& ns,
                                  const std::string& blockLocalName,
                                  int x, int y, int z);

// Create copycat material warm-up: queue BE "Material" ids while chunks load,
// then ProcessBlockstate them single-threaded before blockstate caches freeze.
void ModCT_QueueCopycatWarm(const NbtTagPtr& beNbt);
void ModCT_FlushCopycatWarm();

#endif // MODCT_H
