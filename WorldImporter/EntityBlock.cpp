#include "EntityBlock.h"
#include <vector>
#include <algorithm>
#include <sstream>
using namespace std;
float centerX = 0.5f;
float centerY = 0.5f;
float centerZ = 0.5f;

ModelData EntityBlock::GenerateEntityBlockModel(const string& blockName) {
    string texturePath;
    int waterLevel;
    bool waterFalling;

    if (IsLightBlock(blockName, texturePath)&&config.exportLightBlock) {
        return GenerateLightBlockModel(texturePath);
    }
    // 其他类型方块的生成逻辑
    ModelData defaultModel;
    return defaultModel;
}


bool EntityBlock::IsLightBlock(const string& blockName, string& outTexturePath) {
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    string ns = "minecraft"; // 默认命名空间
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }

    // 过滤非minecraft命名空间
    if (ns != "minecraft") return false;

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 检查是否为光源方块
    if (blockID != "light") return false;

    // 提取光照等级
    string level = "15"; // 默认亮度
    if (bracketPos != string::npos) {
        size_t equalsPos = processed.find('=', bracketPos);
        size_t endPos = processed.find(']', equalsPos);
        if (equalsPos != string::npos && endPos != string::npos) {
            level = processed.substr(equalsPos + 1, endPos - equalsPos - 1);
        }
    }

    // 格式化为两位数
    if (level.length() == 1) level = "0" + level;

    // 构建材质路径
    outTexturePath = ns + ":block/light_block_" + level;
    return true;
}

ModelData EntityBlock::GenerateLightBlockModel(const string& texturePath) {
    ModelData cubeModel;
    float halfSize = config.lightBlockSize;

    cubeModel.vertices = {
        // 前面 (front)
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        // 后面 (back)
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        // 上面 (top)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        // 下面 (bottom)
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 左面 (left)
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 右面 (right)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize
    };

    // 设置 UV 坐标
    cubeModel.uvCoordinates = {
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f
    };

    // 设置面数据（每个面由4个顶点组成）
    cubeModel.faces = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23
    };
    cubeModel.uvFaces = cubeModel.faces;

    cubeModel.materialNames = { texturePath };
    cubeModel.texturePaths = { "None" };
    cubeModel.materialIndices = vector<int>(6, 0);

    cubeModel.faceDirections = {
        "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL","DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL","DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL","DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL"
    };

    return cubeModel;
}