// EntityBlock.h 修改
#ifndef ENTITY_BLOCK_H
#define ENTITY_BLOCK_H

#include "model.h"
#include <string>

class EntityBlock {
public:
    static ModelData GenerateEntityBlockModel(const std::string& blockName);

private:
    static ModelData GenerateLightBlockModel(const std::string& texturePath);

    static bool IsLightBlock(const std::string& blockName, std::string& outTexturePath);

};

#endif // ENTITY_BLOCK_H