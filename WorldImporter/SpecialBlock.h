// SpecialBlock.h 修改
#ifndef SpecialBlock_H
#define SpecialBlock_H

#include "model.h"
#include "config.h"
#include "nbtutils.h"
#include <string>

class SpecialBlock {
public:
    static ModelData GenerateSpecialBlockModel(const std::string& blockName);
    static bool TryGenerateCreateBlockModel(const std::string& blockName,
        int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel);

private:
    static ModelData GenerateLightBlockModel(const std::string& texturePath);
    static ModelData GenerateBedModel(const std::string& blockName);

    static bool IsLightBlock(const std::string& blockName, std::string& outTexturePath);

};

#endif // SpecialBlock_H
