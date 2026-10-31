// SpecialBlock.h 修改
#ifndef SpecialBlock_H
#define SpecialBlock_H

#include "model.h"
#include "config.h"
#include "nbtutils.h"
#include <string>
#include <vector>

class SpecialBlock {
public:
    static ModelData GenerateSpecialBlockModel(const std::string& blockName);
    // Forge/NeoForge/porting_lib OBJ loader models (loader "*:obj" + "model").
    static bool IsObjModelJson(const nlohmann::json& modelJson);
    static ModelData GenerateObjModel(const nlohmann::json& modelJson);
    static bool TryGenerateCreateBlockModel(const std::string& blockName,
        int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel);
    // 原版实体渲染方块(饰纹陶罐/潮涌核心/钟体/潜影盒), 不支持时返回空模型
    static ModelData GenerateBlockEntityModel(const std::string& blockName);
    // 带图案的饰纹陶罐(方块实体 sherds), 没有图案时返回空模型
    static ModelData GenerateDecoratedPotModel(const std::string& blockName, const NbtTagPtr& blockEntityNbt);
    // sherds 物品 id, 顺序 back, left, right, front; 缺项为空串, 无该标签返回空
    static std::vector<std::string> ReadPotSherds(const NbtTagPtr& blockEntityNbt);

private:
    static ModelData GenerateLightBlockModel(const std::string& texturePath);
    static ModelData GenerateBedModel(const std::string& blockName);
    static ModelData GenerateChestModel(const std::string& blockName);
    static ModelData GenerateSignModel(const std::string& blockName);

    static bool IsLightBlock(const std::string& blockName, std::string& outTexturePath);

};

#endif // SpecialBlock_H
