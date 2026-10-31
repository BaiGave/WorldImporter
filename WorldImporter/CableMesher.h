// CableMesher.h — generic network cable / transmitter meshers
//
// Prefer neighbor-inferred connectivity (Create fluid-pipe style). BE connection
// enums (Mek connection0..5) refine PUSH/PULL when present.
// IE wire curves between connectors stay out of scope (entity graph).

#ifndef CABLEMESHER_H
#define CABLEMESHER_H

#include <string>
#include "model.h"
#include "nbtutils.h"

struct ICableMesher {
    virtual ~ICableMesher() = default;
    virtual const char* Name() const = 0;
    virtual bool AppliesTo(const std::string& ns, const std::string& blockName) const = 0;
    // Fills outModel with network geometry. Return true if handled.
    virtual bool TryGenerate(ModelData& outModel, const std::string& ns,
                             const std::string& blockName, int x, int y, int z,
                             const NbtTagPtr& beNbt) = 0;
};

void RegisterCableMesher(ICableMesher* mesher); // non-owning
void InitializeCableMeshers(); // registers built-in Mek transmitter mesher

// Try registered meshers. Returns true if one produced geometry.
bool TryGenerateCableModel(ModelData& outModel, const std::string& ns,
                           const std::string& blockName, int x, int y, int z,
                           const NbtTagPtr& beNbt);

#endif // CABLEMESHER_H
