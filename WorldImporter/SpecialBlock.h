// SpecialBlock.h 修改
#ifndef SpecialBlock_H
#define SpecialBlock_H

#include "model.h"
#include "config.h"
#include <string>

class SpecialBlock {
public:
    static ModelData GenerateSpecialBlockModel(const std::string& blockName);

private:
    static ModelData GenerateLightBlockModel(const std::string& texturePath);

    static bool IsLightBlock(const std::string& blockName, std::string& outTexturePath);

};

#endif // SpecialBlock_H