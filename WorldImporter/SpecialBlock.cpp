#include "SpecialBlock.h"
#include <vector>
#include <algorithm>
#include <sstream>
#include <span>
#include "texture.h"

using namespace std;
float centerX = 0.5f;
float centerY = 0.5f;
float centerZ = 0.5f;

ModelData SpecialBlock::GenerateSpecialBlockModel(const string& blockName) {
    string texturePath;
    int waterLevel;
    bool waterFalling;

    if (IsLightBlock(blockName, texturePath)&&config.exportLightBlock) {
        return GenerateLightBlockModel(texturePath);
    }

    // 检查床方块 (block ID 以 _bed 结尾)
    {
        string testName = blockName;
        size_t colonPos = testName.find(':');
        if (colonPos != string::npos) {
            testName = testName.substr(colonPos + 1);
        }
        size_t bracketPos = testName.find('[');
        string blockID = testName.substr(0, bracketPos);
        if (blockID.size() > 4 &&
            blockID.compare(blockID.size() - 4, 4, "_bed") == 0) {
            return GenerateBedModel(blockName);
        }
    }

    // 其他类型方块的生成逻辑
    ModelData defaultModel;
    return defaultModel;
}


bool SpecialBlock::IsLightBlock(const string& blockName, string& outTexturePath) {
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

ModelData SpecialBlock::GenerateLightBlockModel(const string& texturePath) {
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

    // 创建材质
    Material material;
    material.name = texturePath;
    material.texturePath = "None";
    material.tintIndex = -1;  // 设置默认tint索引
    cubeModel.materials = { material };

    // 使用Face结构体创建六个面
    cubeModel.faces.resize(6);
    
    // 前面
    cubeModel.faces[0].vertexIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].uvIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].materialIndex = 0;
    cubeModel.faces[0].faceDirection = FaceType::DO_NOT_CULL;
    
    // 后面
    cubeModel.faces[1].vertexIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].uvIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].materialIndex = 0;
    cubeModel.faces[1].faceDirection = FaceType::DO_NOT_CULL;
    
    // 上面
    cubeModel.faces[2].vertexIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].uvIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].materialIndex = 0;
    cubeModel.faces[2].faceDirection = FaceType::DO_NOT_CULL;
    
    // 下面
    cubeModel.faces[3].vertexIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].uvIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].materialIndex = 0;
    cubeModel.faces[3].faceDirection = FaceType::DO_NOT_CULL;
    
    // 左面
    cubeModel.faces[4].vertexIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].uvIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].materialIndex = 0;
    cubeModel.faces[4].faceDirection = FaceType::DO_NOT_CULL;
    
    // 右面
    cubeModel.faces[5].vertexIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].uvIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].materialIndex = 0;
    cubeModel.faces[5].faceDirection = FaceType::DO_NOT_CULL;

    return cubeModel;
}

ModelData SpecialBlock::GenerateBedModel(const string& blockName) {
    // 解析方块名称: "minecraft:red_bed[facing=east,part=head]"
    string ns = "minecraft";
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }
    if (ns != "minecraft") {
        return ModelData();
    }

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 从方块ID中提取颜色 (移除 "_bed" 后缀)
    string color = blockID;
    size_t bedPos = color.rfind("_bed");
    if (bedPos != string::npos) {
        color = color.substr(0, bedPos);
    }

    // 解析状态属性
    string part = "head";
    string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart);
        string pair;
        while (getline(ss, pair, ',')) {
            size_t eqPos = pair.find('=');
            if (eqPos != string::npos) {
                string key = pair.substr(0, eqPos);
                string value = pair.substr(eqPos + 1);
                if (key == "part") part = value;
                else if (key == "facing") facing = value;
            }
        }
    }

    // 构建材质路径
    string textureName = ns + ":entity/bed/" + color;
    string textureFileDir = "textures";
    string savedTexturePath;
    if (!SaveTextureToFile(ns, "entity/bed/" + color, textureFileDir)) {
        return ModelData(); // 纹理不存在,返回空模型
    }
    savedTexturePath = "textures/" + ns + "/entity/bed/" + color + ".png";
    RegisterTexture(ns, "entity/bed/" + color, savedTexturePath);

    ModelData model;

    // 辅助lambda: 添加一个面 (顶点使用床本地坐标系 -0.5~0.5, 自动转换到 0~1)
    auto addFace = [&](float v0x, float v0y, float v0z,
                        float v1x, float v1y, float v1z,
                        float v2x, float v2y, float v2z,
                        float v3x, float v3y, float v3z,
                        float u0, float v0,
                        float u1, float v1,
                        float u2, float v2,
                        float u3, float v3) {
        int vertexStart = (int)model.vertices.size() / 3;
        int uvStart = (int)model.uvCoordinates.size() / 2;

        // 从床本地坐标系 (-0.5~0.5) 转换到方块坐标系 (0~1)
        model.vertices.push_back(v0x + 0.5f);
        model.vertices.push_back(v0y + 0.5f);
        model.vertices.push_back(v0z + 0.5f);
        model.vertices.push_back(v1x + 0.5f);
        model.vertices.push_back(v1y + 0.5f);
        model.vertices.push_back(v1z + 0.5f);
        model.vertices.push_back(v2x + 0.5f);
        model.vertices.push_back(v2y + 0.5f);
        model.vertices.push_back(v2z + 0.5f);
        model.vertices.push_back(v3x + 0.5f);
        model.vertices.push_back(v3y + 0.5f);
        model.vertices.push_back(v3z + 0.5f);

        model.uvCoordinates.push_back(u0);
        model.uvCoordinates.push_back(v0);
        model.uvCoordinates.push_back(u1);
        model.uvCoordinates.push_back(v1);
        model.uvCoordinates.push_back(u2);
        model.uvCoordinates.push_back(v2);
        model.uvCoordinates.push_back(u3);
        model.uvCoordinates.push_back(v3);

        Face face;
        face.vertexIndices = {vertexStart, vertexStart + 1, vertexStart + 2, vertexStart + 3};
        face.uvIndices = {uvStart, uvStart + 1, uvStart + 2, uvStart + 3};
        face.materialIndex = 0;
        face.faceDirection = DO_NOT_CULL;
        model.faces.push_back(face);
    };

    // 设置材质
    Material material;
    material.name = textureName;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    if (part == "head")
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f, 22/64.0f, 58/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 42/64.0f, 28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 44/64.0f, 58/64.0f);
        // 前面 (床头板正面, z=-0.5), jmc2obj 的 UV 是非顺序分配 (uv[2],uv[3],uv[0],uv[1])
        addFace( 0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,  -0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f, -0.5f,
                6/64.0f, 64/64.0f, 22/64.0f, 64/64.0f, 22/64.0f, 58/64.0f, 6/64.0f, 58/64.0f);
        // 左面 (床头板左侧, x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 58/64.0f,  0/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f);
        // 右面 (床头板右侧, x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 22/64.0f, 58/64.0f, 22/64.0f, 42/64.0f);

        // --- 左床腿 (床头端, z=-0.5~-0.3125, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,    -0.5f, -0.3125f, -0.3125f,  -0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace(-0.5f, -0.3125f, -0.3125f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床头端, z=-0.5~-0.3125, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.5f, -0.3125f, -0.3125f,   0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f, -0.5f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.3125f, -0.3125f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace( 0.5f, -0.3125f, -0.3125f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }
    else // foot
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f, 22/64.0f, 36/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 20/64.0f, 28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 44/64.0f, 36/64.0f);
        // 左面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 36/64.0f,  0/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f);
        // 右面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 22/64.0f, 36/64.0f, 22/64.0f, 20/64.0f);
        // 后面 (床尾板, z=0.5)
        addFace( 0.5f, -0.3125f, 0.5f,  -0.5f, -0.3125f, 0.5f,  -0.5f, 0.0625f, 0.5f,   0.5f, 0.0625f, 0.5f,
                38/64.0f, 42/64.0f, 22/64.0f, 42/64.0f, 22/64.0f, 36/64.0f, 38/64.0f, 36/64.0f);

        // --- 左床腿 (床尾端, z=0.3125~0.5, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f,  0.5f,    -0.5f, -0.3125f, 0.3125f,  -0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace(-0.5f, -0.3125f,  0.5f,  -0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace(-0.5f, -0.3125f, 0.3125f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床尾端, z=0.3125~0.5, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,     0.5f, -0.3125f, 0.3125f,   0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f,  0.5f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.3125f, -0.3125f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace( 0.5f, -0.3125f, 0.3125f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }

    // 根据朝向应用旋转 (使用方块坐标系 0~1, 以 0.5,0.5 为中心)
    int rotY = 0;
    if (facing == "south") rotY = 180;
    else if (facing == "west") rotY = 270;
    else if (facing == "east") rotY = 90;

    if (rotY != 0) {
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    }

    return model;
}