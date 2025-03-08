#include "RegionModelExporter.h"
#include "coord_conversion.h"
#include "objExporter.h"
#include "biome.h"
#include "texture.h"
#include <iomanip>  // 用于 std::setw 和 std::setfill
#include <sstream>  // 用于 std::ostringstream
#include <regex>
#include <chrono>  // 新增：用于时间测量
#include <iostream>  // 新增：用于输出时间
#include "EntityBlock.h"

using namespace std;
using namespace std::chrono;  // 新增：方便使用 chrono
// 模型缓存（假设有一个全局缓存 Map）
static std::unordered_map<int, ModelData> fluidModelCache;

void deduplicateVertices(ModelData& data) {
    std::unordered_map<std::string, int> vertexMap;
    std::vector<float> newVertices;
    std::vector<int> indexMap;

    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        float x = data.vertices[i + 0];
        float y = data.vertices[i + 1];
        float z = data.vertices[i + 2];
        int roundedX = static_cast<int>(x * 10000 + 0.5);
        int roundedY = static_cast<int>(y * 10000 + 0.5);
        int roundedZ = static_cast<int>(z * 10000 + 0.5);
        std::string key = std::to_string(roundedX) + "," + std::to_string(roundedY) + "," + std::to_string(roundedZ);

        if (vertexMap.find(key) != vertexMap.end()) {
            indexMap.push_back(vertexMap[key]);
        } else {
            int newIndex = newVertices.size() / 3;
            vertexMap[key] = newIndex;
            newVertices.insert(newVertices.end(), {x, y, z});
            indexMap.push_back(newIndex);
        }
    }

    data.vertices = newVertices;

    // 更新面数据中的顶点索引
    for (auto& idx : data.faces) {
        idx = indexMap[idx];
    }
}

void deduplicateFaces(ModelData& data, bool checkMaterial = true) {
    // 键结构需要包含完整信息
    struct FaceKey {
        std::array<int, 4> sortedVerts;
        int materialIndex;
    };

    // 自定义相等比较谓词
    struct KeyEqual {
        bool checkMode;
        explicit KeyEqual(bool mode) : checkMode(mode) {}

        bool operator()(const FaceKey& a, const FaceKey& b) const {
            if (a.sortedVerts != b.sortedVerts) return false;
            return !checkMode || (a.materialIndex == b.materialIndex);
        }
    };

    // 自定义哈希器
    struct KeyHasher {
        bool checkMode;
        explicit KeyHasher(bool mode) : checkMode(mode) {}

        size_t operator()(const FaceKey& k) const {
            size_t seed = 0;
            if (checkMode) {
                seed ^= std::hash<int>()(k.materialIndex) + 0x9e3779b9;
            }
            for (int v : k.sortedVerts) {
                seed ^= std::hash<int>()(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            }
            return seed;
        }
    };

    // 初始化容器时注入检查模式和比较逻辑
    using FaceMap = std::unordered_map<FaceKey, int, KeyHasher, KeyEqual>;
    FaceMap faceCount(10, KeyHasher(checkMaterial), KeyEqual(checkMaterial));

    // 第一次遍历：生成统计
    for (size_t i = 0; i < data.faces.size(); i += 4) {
        std::array<int, 4> face = {
            data.faces[i], data.faces[i + 1],
            data.faces[i + 2], data.faces[i + 3]
        };

        std::array<int, 4> sorted = face;
        std::sort(sorted.begin(), sorted.end());

        FaceKey key{ sorted, checkMaterial ? data.materialIndices[i / 4] : -1 };
        faceCount[key]++;
    }

    // 第二次遍历：过滤数据
    std::vector<int> newFaces, newUvFaces, newMaterials;
    for (size_t i = 0; i < data.faces.size(); i += 4) {
        std::array<int, 4> face = {
            data.faces[i], data.faces[i + 1],
            data.faces[i + 2], data.faces[i + 3]
        };

        std::array<int, 4> sorted = face;
        std::sort(sorted.begin(), sorted.end());

        FaceKey key{ sorted, checkMaterial ? data.materialIndices[i / 4] : -1 };

        if (faceCount[key] == 1) {
            newFaces.insert(newFaces.end(), face.begin(), face.end());
            newUvFaces.insert(newUvFaces.end(),
                data.uvFaces.begin() + i,
                data.uvFaces.begin() + i + 4);
            newMaterials.push_back(data.materialIndices[i / 4]);
        }
    }

    data.faces.swap(newFaces);
    data.uvFaces.swap(newUvFaces);
    data.materialIndices.swap(newMaterials);
}

void ScaleModel(ModelData& model, float scale) {
    // 计算缩放中心（假设模型中心为 (0.5, 0.5, 0.5)）
    float centerX = 0.5f;
    float centerY = 0.5f;
    float centerZ = 0.5f;

    // 遍历所有顶点并进行缩放
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] = centerX + (model.vertices[i] - centerX) * scale;    // X坐标缩放
        model.vertices[i + 1] = centerY + (model.vertices[i + 1] - centerY) * scale;  // Y坐标缩放
        model.vertices[i + 2] = centerZ + (model.vertices[i + 2] - centerZ) * scale;  // Z坐标缩放
    }
}


void RegionModelExporter::ExportRegionModels(int xStart, int xEnd, int yStart, int yEnd,
    int zStart, int zEnd, const string& outputName) {
    RegisterFluidTextures();
    auto start = high_resolution_clock::now();  // 新增：开始时间点
    // 收集区域内所有唯一方块ID
    LoadChunks(xStart, xEnd, yStart, yEnd, zStart, zEnd);
    auto end = high_resolution_clock::now();  // 新增：结束时间点
    auto duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    std::cout << "LoadChunks耗时: " << duration.count() << " ms" << endl;  // 新增：输出到控制台
    UpdateSkyLightNeighborFlags();
    auto blocks = GetGlobalBlockPalette();
    // 使用 ProcessBlockstateForBlocks 处理所有方块状态模型
    ProcessBlockstateForBlocks(blocks);
    
    // 获取区域内的所有区块范围（按16x16x16划分）
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd, sectionYStart, sectionYEnd;
    blockToChunk(xStart, zStart, chunkXStart, chunkZStart);
    blockToChunk(xEnd, zEnd, chunkXEnd, chunkZEnd);
    blockYToSectionY(yStart, sectionYStart);
    blockYToSectionY(yEnd, sectionYEnd);
    start = high_resolution_clock::now();  // 新增：开始时间点
    // 遍历每个区块
    ModelData finalMergedModel;
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                // 生成当前区块的子模型
                ModelData chunkModel = GenerateChunkModel(chunkX, sectionY, chunkZ);
                // 合并到总模型
                if (finalMergedModel.vertices.empty()) {
                    finalMergedModel = chunkModel;
                }
                else {
                    MergeModelsDirectly(finalMergedModel, chunkModel);
                }
                
                
            }
           
        }
    }
    end = high_resolution_clock::now();  // 新增：结束时间点
    duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "模型合并耗时: " << duration.count() << " ms" << endl;  // 新增：输出到控制台
    start = high_resolution_clock::now();  // 新增：开始时间点
    deduplicateVertices(finalMergedModel);
    end = high_resolution_clock::now();  // 新增：结束时间点
    duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "deduplicateVertices: " << duration.count() << " ms" << endl;  // 新增：输出到控制台

    start = high_resolution_clock::now();  // 新增：开始时间点
    // 严格模式：材质+顶点都相同才剔除
    deduplicateFaces(finalMergedModel, true);

    // 宽松模式：仅顶点相同即剔除
    //deduplicateFaces(finalMergedModel, false);

    end = high_resolution_clock::now();  // 新增：结束时间点
    duration = duration_cast<milliseconds>(end - start);  // 新增：计算时间差
    cout << "deduplicateFaces: " << duration.count() << " ms" << endl;  // 新增：输出到控制台
    // 导出最终模型
    if (!finalMergedModel.vertices.empty()) {
        CreateModelFiles(finalMergedModel, outputName);
    }
}


static float getHeight(int level) {
    if (level == 0)
        return 14.166666f; // 水源

    if (level == -1)
        return 0.0f; // 空气

    if (level == -2)
        return -1.0f; // 一般方块

    if (level ==8)
        return 16.0f; // 流动水


    // 中间值的线性插值计算
    return 2.0f + (12.0f / 7.0f) * (7 - level); 
}


float getCornerHeight(float currentHeight, float NWHeight, float NHeight, float WHeight) {
    float totalWeight = 0.0f;
    float res = 0.0f;
    bool sourceBlock = false;

    if (currentHeight >= 16.0f || NWHeight >= 16.0f || NHeight >= 16.0f || WHeight >= 16.0f) {
        return 16.0f;
    }

    if (currentHeight == 14.166666f) {
        res += currentHeight * 11.0f;
        totalWeight += 11.0f;
        sourceBlock = true;
    }
    if (NWHeight == 14.166666f) {
        res += NWHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }
    if (NHeight == 14.166666f) {
        res += NHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }
    if (WHeight == 14.166666f) {
        res += WHeight * 12.0f;
        totalWeight += 12.0f;
        sourceBlock = true;
    }

    if (sourceBlock) {
        if (currentHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (NWHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (NHeight == 0.0f) {
            totalWeight += 1.0f;
        }
        if (WHeight == 0.0f) {
            totalWeight += 1.0f;
        }
    }
    else {
        if (currentHeight >= 0.0f) {
            res += currentHeight;
            totalWeight += 1.0f;
        }
        if (NWHeight >= 0.0f) {
            res += NWHeight;
            totalWeight += 1.0f;
        }
        if (NHeight >= 0.0f) {
            res += NHeight;
            totalWeight += 1.0f;
        }
        if (WHeight >= 0.0f) {
            res += WHeight;
            totalWeight += 1.0f;
        }
    }

    return (totalWeight == 0.0f) ? 0.0f : res / totalWeight;
}


ModelData GenerateFluidModel(const std::array<int, 10>& fluidLevels) {
    ModelData model;

    // 获取当前方块的液位和周围液位的高度
    int currentLevel = fluidLevels[0];
    int northLevel = fluidLevels[1];    // 北
    int southLevel = fluidLevels[2];    // 南
    int eastLevel = fluidLevels[3];     // 东
    int westLevel = fluidLevels[4];     // 西
    int northeastLevel = fluidLevels[5]; // 东北
    int northwestLevel = fluidLevels[6]; // 西北
    int southeastLevel = fluidLevels[7]; // 东南
    int southwestLevel = fluidLevels[8]; // 西南
    int belowLevel = fluidLevels[9];     // 下方

    float currentHeight = getHeight(currentLevel);
    float northHeight = getHeight(northLevel);
    float southHeight = getHeight(southLevel);
    float eastHeight = getHeight(eastLevel);
    float westHeight = getHeight(westLevel);
    float northeastHeight = getHeight(northeastLevel);
    float northwestHeight = getHeight(northwestLevel);
    float southeastHeight = getHeight(southeastLevel);
    float southwestHeight = getHeight(southwestLevel);

    // 设置 below 值
    int below = (belowLevel == -1) ? 1 : 0;

    size_t key = 0;
    for (int level : fluidLevels) {
        key = (key << 3) ^ (level + (level << 5));
    }
    // 检查缓存中是否存在该模型
    if (fluidModelCache.find(key) != fluidModelCache.end()) {

        return fluidModelCache[key]; // 返回缓存中的模型
    }

    // 如果缓存中没有该模型，则生成并存入缓存
    // 计算四个上顶点的高度
    float h_nw = getCornerHeight(currentHeight, northwestHeight, northHeight, westHeight)/16.0f;
    float h_ne = getCornerHeight(currentHeight, northeastHeight, northHeight, eastHeight)/16.0f;
    float h_se = getCornerHeight(currentHeight, southeastHeight, southHeight, eastHeight)/16.0f;
    float h_sw = getCornerHeight(currentHeight, southwestHeight, southHeight, westHeight)/16.0f;
    h_nw = ceil(h_nw * 10.0f) / 10.0f;
    h_ne = ceil(h_ne * 10.0f) / 10.0f;
    h_se = ceil(h_se * 10.0f) / 10.0f;
    h_sw = ceil(h_sw * 10.0f) / 10.0f;

    model.vertices = {
        // 底面 (bottom) - 偏移方向：Y轴负方向 (向下)
        0.0f, 0.0f, 0.0f,       // 0
        1.0f, 0.0f, 0.0f,       // 1
        1.0f, 0.0f, 1.0f,       // 2
        0.0f, 0.0f, 1.0f,       // 3

        // 顶面 (top) - 偏移方向：Y轴正方向 (向上)
        0.0f, h_nw, 0.0f, // 西北角
        1.0f, h_ne, 0.0f, // 东北角
        1.0f, h_se, 1.0f, // 东南角
        0.0f, h_sw, 1.0f, // 西南角

        // 北面 (north) - 偏移方向：Z轴负方向 (向后)
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        1.0f, h_ne, 0.0f,
        0.0f, h_nw, 0.0f,

        // 南面 (south) - 偏移方向：Z轴正方向 (向前)
        0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f,
        1.0f, h_se, 1.0f,
        0.0f, h_sw, 1.0f,

        // 西面 (west) - 偏移方向：X轴负方向 (向左)
        0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, h_sw, 1.0f,
        0.0f, h_nw, 0.0f,

        // 东面 (east) - 偏移方向：X轴正方向 (向右)
        1.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 1.0f,
        1.0f, h_se, 1.0f,
        1.0f, h_ne, 0.0f
    };

    // 面索引（每4个顶点构成一个面）
    model.faces = {
        // 下面 (bottom)
        0, 1, 2, 3,
        // 上面 (top)
        4, 5, 6, 7,
        // 北面 (north)
        8, 9, 10, 11,
        // 南面 (south)
        12, 13, 14, 15,
        // 西面 (west)
        16, 17, 18, 19,
        // 东面 (east)
        20, 21, 22, 23
    };
    model.uvFaces = model.faces;
    // 面方向（每个面四个顶点共享同一方向）

    if (currentLevel==0)
    {
        model.faceDirections = {
        "down", "down", "down", "down",    // 下面
        "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL", "DO_NOT_CULL",            // 上面
        "north", "north", "north", "north",// 北面
        "south", "south", "south", "south",// 南面
        "west", "west", "west", "west",    // 西面
        "east", "east", "east", "east"     // 东面
        };

    }
    else
    {
        model.faceDirections = {
        "down", "down", "down", "down",    // 下面
        "up", "up", "up", "up",            // 上面
        "north", "north", "north", "north",// 北面
        "south", "south", "south", "south",// 南面
        "west", "west", "west", "west",    // 西面
        "east", "east", "east", "east"     // 东面
        };
    }
    
    float v_nw = 1- (h_nw) / 32.0f;
    float v_ne = 1- (h_ne) / 32.0f;
    float v_se =  1-(h_se) / 32.0f;
    float v_sw =  1-(h_sw) / 32.0f;
    


    model.uvCoordinates = {
        // 下面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f,31.0 / 32.0f , 0.0f,31.0 / 32.0f,
        // 上面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f , 0.0f, 0.0f,
        // 北面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_ne, 0.0f, v_nw,
        // 南面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f,v_sw ,
        // 西面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_sw, 0.0f, v_nw,
        // 东面
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f,v_ne
    };
    // 材质设置
    model.materialNames = { "minecraft:block/water_still", "minecraft:block/water_flow" };
    model.texturePaths = { "textures/minecraft/block/water_still.png", "textures/minecraft/block/water_flow.png" };
    
    if (currentLevel ==0 || currentLevel == 8) {
        model.uvCoordinates = {
            // 下面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f,31.0 / 32.0f , 0.0f,31.0 / 32.0f,
            // 上面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 31.0 / 32.0f , 0.0f, 31.0 / 32.0f,
            // 北面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_ne, 0.0f, v_nw,
            // 南面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_se, 0.0f,v_sw ,
            // 西面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f, v_sw, 0.0f, v_nw,
            // 东面
            0.0f, 1.0f, 1.0f, 1.0f, 1.0f,v_se , 0.0f, v_ne
        };
        model.materialIndices = { 0, 0, 1, 1, 1, 1 };
    }
    else {
        // 计算梯度和旋转角度
        float gradientX0 = h_ne - h_nw;
        if (h_nw < 0.0f) gradientX0 = 0.0f;

        float gradientX1 = h_se - h_sw;
        if (h_sw < 0.0f) gradientX1 = 0.0f;

        float gradientZ0 = h_nw - h_sw;
        if (h_sw < 0.0f) gradientZ0 = 0.0f;

        float gradientZ1 = h_ne - h_se;
        if (h_se < 0.0f) gradientZ1 = 0.0f;

        float gradientX = (gradientX0 + gradientX1)*16;
        float gradientZ = (gradientZ0 + gradientZ1)*16;

        float gradientLength = static_cast<float>(sqrt(gradientX * gradientX + gradientZ * gradientZ));
        float angle = 0.0f;

        if (gradientLength > 0.0f) {
            gradientX /= gradientLength;
            gradientZ /= gradientLength;
            angle = static_cast<float>(atan2(gradientX, -gradientZ));
            // 将弧度转换为角度
            angle = static_cast<float>(angle * (180.0f / M_PI));

            // 确保角度在 [-180, 180] 范围内
            if (angle < -180.0f) angle += 360.0f;
            if (angle > 180.0f) angle -= 360.0f;

            // 将角度归一化到 [0, 360] 范围内
            angle = fmod(angle + 360.0f, 360.0f);

            // 将角度量化为 22.5 度的倍数
            angle = static_cast<float>(floor(angle / 22.5f + 0.5f) * 22.5f);
        }



        // 将角度转换为弧度
        float angleRad = angle * (M_PI / 180.0f);

        // 计算旋转矩阵
        float cosTheta = cos(angleRad);
        float sinTheta = sin(angleRad);

        // 旋转中心点
        constexpr float centerX = 0.5f;
        constexpr float centerY = 0.5f;

        // 遍历 UV 坐标数组
        for (size_t i = 0; i < model.uvCoordinates.size(); i += 8) {
            // 仅对上顶面的 UV 坐标进行旋转
            if (i >= 8 && i < 16) { // 上顶面对应的 UV 坐标范围
                // 提取 4 个 UV 顶点
                float u0 = model.uvCoordinates[i];
                float v0 = model.uvCoordinates[i + 1];
                float u1 = model.uvCoordinates[i + 2];
                float v1 = model.uvCoordinates[i + 3];
                float u2 = model.uvCoordinates[i + 4];
                float v2 = model.uvCoordinates[i + 5];
                float u3 = model.uvCoordinates[i + 6];
                float v3 = model.uvCoordinates[i + 7];

                // 旋转 UV 坐标
                auto rotateUV = [&](float& u, float& v) {
                    const float relU = u - centerX;
                    const float relV = v - centerY;
                    const float newU = relU * cosTheta - relV * sinTheta + centerX;
                    const float newV = relU * sinTheta + relV * cosTheta + centerY;
                    u = newU;
                    v = newV;
                    };

                rotateUV(u0, v0);
                rotateUV(u1, v1);
                rotateUV(u2, v2);
                rotateUV(u3, v3);
                // 将 x 值从 [1 到 0] 缩放到 [1 到 31/32]
                auto scaleU = [](float u) {
                    return 1.0f - (1.0f - 31.0f / 32.0f) * u;
                    };

                v0 = scaleU(v0);
                v1 = scaleU(v1);
                v2 = scaleU(v2);
                v3 = scaleU(v3);

                // 更新 UV 坐标
                model.uvCoordinates[i] = u0;
                model.uvCoordinates[i + 1] = v0;
                model.uvCoordinates[i + 2] = u1;
                model.uvCoordinates[i + 3] = v1;
                model.uvCoordinates[i + 4] = u2;
                model.uvCoordinates[i + 5] = v2;
                model.uvCoordinates[i + 6] = u3;
                model.uvCoordinates[i + 7] = v3;
            }
        }
        model.materialIndices = { 0, 1, 1, 1, 1, 1 };
    }
    

    // 存入缓存
    fluidModelCache[key] = model;
    return model;
}

std::unordered_map<std::string, std::string> ParseStateAttributes(const std::string& fluidId) {
    std::unordered_map<std::string, std::string> attributes;
    size_t bracketPos = fluidId.find('[');
    if (bracketPos != std::string::npos) {
        size_t closeBracketPos = fluidId.find(']', bracketPos);
        if (closeBracketPos != std::string::npos) {
            std::string statePart = fluidId.substr(bracketPos + 1, closeBracketPos - bracketPos - 1);
            std::stringstream ss(statePart);
            std::string attr;
            while (std::getline(ss, attr, ',')) {
                size_t equalPos = attr.find('=');
                if (equalPos != std::string::npos) {
                    std::string key = attr.substr(0, equalPos);
                    std::string value = attr.substr(equalPos + 1);
                    attributes[key] = value;
                }
            }
        }
    }
    return attributes;
}

void AssignFluidMaterials(ModelData& model, const std::string& fluidId) {
    // 提取基础 ID 和状态值（如果有多个状态值）
    std::string baseId;
    std::unordered_map<std::string, std::string> stateValues;

    size_t bracketPos = fluidId.find('[');
    if (bracketPos != std::string::npos) {
        baseId = fluidId.substr(0, bracketPos);

        std::string statePart = fluidId.substr(bracketPos + 1, fluidId.size() - bracketPos - 2); // Remove the closing ']'
        std::stringstream ss(statePart);
        std::string statePair;

        while (std::getline(ss, statePair, ',')) {
            size_t equalPos = statePair.find(':');
            if (equalPos != std::string::npos) {
                std::string key = statePair.substr(0, equalPos);
                std::string value = statePair.substr(equalPos + 1);

                stateValues[key] = value;
            }
        }
    }
    else {
        baseId = fluidId;
    }

    // 尝试查找流体定义
    auto fluidIt = fluidDefinitions.find(baseId);
    if (fluidIt == fluidDefinitions.end()) {
        // 尝试匹配 level_property
        for (const auto& entry : fluidDefinitions) {
            if (stateValues.count(entry.second.property) > 0) {
                fluidIt = fluidDefinitions.find(entry.first);
                break;
            }
        }

        if (fluidIt == fluidDefinitions.end()) {
            // 尝试匹配 liquid_blocks
            for (const auto& entry : fluidDefinitions) {
                if (entry.second.liquid_blocks.count(baseId) > 0) {
                    fluidIt = fluidDefinitions.find(entry.first);
                    break;
                }
            }
        }

        if (fluidIt == fluidDefinitions.end()) {
            // 如果仍然没找到，直接返回
            return;
        }
    }

    const FluidInfo& fluidInfo = fluidIt->second;
    std::string fluidName = fluidIt->first;
    // 清空旧数据
    model.materialNames.clear();
    model.texturePaths.clear();

    // 设置材质路径
    model.materialNames = {
        fluidInfo.folder + "/" + fluidName + fluidInfo.still_texture,
        fluidInfo.folder + "/" + fluidName + fluidInfo.flow_texture
    };

    // 构建纹理路径
    size_t colonPos = baseId.find(':');
    std::string ns = (colonPos != std::string::npos) ? baseId.substr(0, colonPos) : "";
    fluidName = (colonPos != std::string::npos) ? fluidName.substr(colonPos + 1) : fluidName;

    model.texturePaths = {
        "textures/" + ns + "/" + fluidInfo.folder + "/" + fluidName + fluidInfo.still_texture + ".png",
        "textures/" + ns + "/" + fluidInfo.folder + "/" + fluidName + fluidInfo.flow_texture + ".png"
    };
}


ModelData RegionModelExporter::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    ModelData chunkModel;
    // 计算区块内的方块范围
    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;
    // 遍历区块内的每个方块
    for (int x = blockXStart; x < blockXStart + 16; ++x) {
        for (int z = blockZStart; z < blockZStart + 16; ++z) {
            int currentY = GetHeightMapY(x, z, "WORLD_SURFACE")-64;
            for (int y = blockYStart; y < blockYStart + 16; ++y) {
                std::array<bool, 6> neighbors; // 邻居是否为空气
                std::array<int, 10> fluidLevels; // 流体液位

                int id = GetBlockIdWithNeighbors(
                    x, y, z,
                    neighbors.data(),
                    fluidLevels.data()
                );
                Block currentBlock = GetBlockById(id);
                string blockName = GetBlockNameById(id);
                
                if (blockName == "minecraft:air" || y > currentY) continue;
                if (GetSkyLight(x,y,z) == -1)continue;

                string ns = GetBlockNamespaceById(id);

                // 标准化方块名称（去掉命名空间，处理状态）
                size_t colonPos = blockName.find(':');
                if (colonPos != string::npos) {
                    blockName = blockName.substr(colonPos + 1);
                }

                ModelData blockModel;
                ModelData liquidModel;
                if (currentBlock.level > -1) {
                    blockModel = GetRandomModelFromCache(ns, blockName);

                    if (blockModel.vertices.empty()) {
                        // 如果是流体方块，生成流体模型
                        liquidModel = GenerateFluidModel(fluidLevels);
                        AssignFluidMaterials(liquidModel, currentBlock.name);
                        blockModel = liquidModel;
                    }
                    else
                    {
                        // 如果是流体方块，生成流体模型
                        liquidModel = GenerateFluidModel(fluidLevels);
                        AssignFluidMaterials(liquidModel, currentBlock.name);
                        
                        if (!blockModel.faceDirections.empty())
                        {
                            for (size_t i = 0; i < blockModel.faceDirections.size(); i += 4)
                            {
                                blockModel.faceDirections[i] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 1] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 2] = "DO_NOT_CULL";
                                blockModel.faceDirections[i + 3] = "DO_NOT_CULL";
                            }
                        }

                        blockModel = MergeModelData(blockModel, liquidModel);
                    }

                }
                else
                {
                    // 处理其他方块
                    blockModel = GetRandomModelFromCache(ns, blockName);
                }
                
                // 剔除被遮挡的面
                std::vector<int> validFaceIndices;
                const std::unordered_map<std::string, int> directionToNeighborIndex = {
                    {"down", 1},  // 假设neighbors[1]对应下方
                    {"up", 0},    // neighbors[0]对应上方
                    {"north", 4}, // neighbors[4]对应北
                    {"south", 5}, // neighbors[5]对应南
                    {"west", 2},  // neighbors[2]对应西
                    {"east", 3}   // neighbors[3]对应东
                };

                // 检查faceDirections是否已初始化
                if (blockModel.faceDirections.empty()) {
                    continue;
                }

                // 检查faces大小是否为4的倍数
                if (blockModel.faces.size() % 4 != 0) {
                    throw std::runtime_error("faces size is not a multiple of 4");
                }

                // 遍历所有面（每4个顶点索引构成一个面）
                for (size_t faceIdx = 0; faceIdx < blockModel.faces.size() / 4; ++faceIdx) {
                    // 检查faceIdx是否超出范围
                    if (faceIdx * 4 >= blockModel.faceDirections.size()) {
                        throw std::runtime_error("faceIdx out of range");
                    }

                    std::string dir = blockModel.faceDirections[faceIdx * 4]; // 取第一个顶点的方向
                    // 如果是 "DO_NOT_CULL"，保留该面
                    if (dir == "DO_NOT_CULL") {
                        validFaceIndices.push_back(faceIdx);
                    }
                    else {
                        auto it = directionToNeighborIndex.find(dir);
                        if (it != directionToNeighborIndex.end()) {
                            int neighborIdx = it->second;
                            if (!neighbors[neighborIdx]) { // 如果邻居存在（非空气），跳过该面
                                continue;
                            }
                        }
                        validFaceIndices.push_back(faceIdx);
                    }

                }

                // 重建面数据（顶点、UV、材质）
                ModelData filteredModel;
                for (int faceIdx : validFaceIndices) {
                    // 提取原面数据（4个顶点索引）
                    for (int i = 0; i < 4; ++i) {
                        filteredModel.faces.push_back(blockModel.faces[faceIdx * 4 + i]);
                        filteredModel.uvFaces.push_back(blockModel.uvFaces[faceIdx * 4 + i]);
                    }
                    // 材质索引
                    filteredModel.materialIndices.push_back(blockModel.materialIndices[faceIdx]);
                    // 方向记录（每个顶点重复方向，这里仅记录一次）
                    filteredModel.faceDirections.push_back(blockModel.faceDirections[faceIdx * 4]);
                }

                // 顶点和UV数据保持不变（后续合并时会去重）
                filteredModel.vertices = blockModel.vertices;
                filteredModel.uvCoordinates = blockModel.uvCoordinates;
                filteredModel.materialNames = blockModel.materialNames;
                filteredModel.texturePaths = blockModel.texturePaths;

                // 使用过滤后的模型
                blockModel = filteredModel;
            

                
                ApplyPositionOffset(blockModel, x, y, z);

                // 合并到主模型
                if (chunkModel.vertices.empty()) {
                    chunkModel = blockModel;
                }
                else {
                    MergeModelsDirectly(chunkModel, blockModel);
                }
            }

        }
    }

    return chunkModel;
}


void RegionModelExporter::LoadChunks(int xStart, int xEnd, int yStart, int yEnd, int zStart, int zEnd) {
    // 计算最小和最大坐标，以处理范围颠倒的情况
    int min_x = min(xStart, xEnd);
    int max_x = max(xStart, xEnd);
    int min_z = min(zStart, zEnd);
    int max_z = max(zStart, zEnd);
    int min_y = min(yStart, yEnd);
    int max_y = max(yStart, yEnd);

    // 计算分块的范围（每个分块宽度为 16 块）
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd;
    blockToChunk(min_x, min_z, chunkXStart, chunkZStart);
    blockToChunk(max_x, max_z, chunkXEnd, chunkZEnd);

    // 处理可能的负数和范围计算
    chunkXStart = floor((float)min_x / 16.0f);
    chunkXEnd = ceil((float)max_x / 16.0f);
    chunkZStart = floor((float)min_z / 16.0f);
    chunkZEnd = ceil((float)max_z / 16.0f);

    // 计算分段 Y 范围（每个分段高度为 16 块）
    int sectionYStart, sectionYEnd;
    blockYToSectionY(min_y, sectionYStart);
    blockYToSectionY(max_y, sectionYEnd);
    sectionYStart = static_cast<int>(floor((float)min_y / 16.0f));
    sectionYEnd = static_cast<int>(ceil((float)max_y / 16.0f));

    // 加载所有相关的分块和分段
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            // 加载并缓存整个 chunk 的所有子区块
            LoadAndCacheBlockData(chunkX, chunkZ);
        }
    }
}

void RegionModelExporter::ApplyPositionOffset(ModelData& model, int x, int y, int z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
    }
}