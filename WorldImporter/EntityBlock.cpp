#include "EntityBlock.h"
#include "RegionModelExporter.h"  // 包含必要的头文件,确保相关函数可用
#include "blockstate.h"         // 为了调用 ProcessBlockstate
#include <span>                  // 为了 std::span (C++20)
#include <iostream>            // 为了 std::cout, std::cerr (如果尚未包含)
#include <map>
#include <vector>
#include <string>


void EntityBlock::PrintDetails() const {
    std::cout << "EntityBlock - ID: " << id << ", X: " << x << ", Y: " << y << ", Z: " << z << std::endl;
}

void YuushyaShowBlockEntity::PrintDetails() const {
    std::cout << "YuushyaShowBlockEntity - ID: " << id << ", X: " << x << ", Y: " << y << ", Z: " << z << std::endl;
    std::cout << "ControlSlot: " << controlSlot << ", KeepPacked: " << keepPacked << std::endl;
    for (const auto& block : blocks) {
        std::cout << "  BlockState Name: " << ", BlockID: " << block.blockid << std::endl;
        std::cout << "  ShowPos: ";
        for (const auto& pos : block.showPos) std::cout << pos << " ";
        std::cout << "\n  ShowRotation: ";
        for (const auto& rot : block.showRotation) std::cout << rot << " ";
        std::cout << "\n  ShowScales: ";
        for (const auto& scale : block.showScales) std::cout << scale << " ";
        std::cout << "\n  IsShown: " << block.isShown << ", Slot: " << block.slot << std::endl;
    }
}

ModelData YuushyaShowBlockEntity::GenerateModel() const {
    ModelData mainModel;
    for (const auto& block : blocks) {
        int id = block.blockid;
        // 位置偏移(除以16转换到模型空间)
        double tx = block.showPos[0] / 16.0f;
        double ty = block.showPos[1] / 16.0f;
        double tz = block.showPos[2] / 16.0f;

        // 旋转参数(直接使用原始值)
        float rx = block.showRotation[0]; // 假设已是角度值
        float ry = block.showRotation[1];
        float rz = block.showRotation[2];

        // 缩放参数
        float sx = block.showScales[0];
        float sy = block.showScales[1];
        float sz = block.showScales[2];

        if (!block.isShown) continue;

        Block b = GetBlockById(id);
        std::string blockName = b.GetModifiedNameWithNamespace();
        std::string ns = b.GetNamespace();
        // 获取模型数据
        size_t colonPos = blockName.find(':');
        if (colonPos != std::string::npos) blockName = blockName.substr(colonPos + 1);
        ModelData blockModel = GetRandomModelFromCache(ns, blockName);

        // 如果缓存未命中,尝试处理 blockstate 并重新获取模型
        if (blockModel.vertices.empty() && !blockName.empty()) {
            ProcessBlockstate(ns, {blockName}); // blockName 应包含方块状态, ns 是命名空间
            blockModel = GetRandomModelFromCache(ns, blockName); // 再次尝试获取
        }

        // 将所有面设置为DO_NOT_CULL,确保不会被贪心合并算法错误剔除
        for (auto& face : blockModel.faces) {
            face.faceDirection = DO_NOT_CULL;
        }

        // 应用变换顺序:缩放 -> 旋转 -> 平移
        ApplyRotationToVertices(std::span<float>(blockModel.vertices.data(), blockModel.vertices.size()), rx, ry, rz);

        ApplyDoublePositionOffset(blockModel, tx, ty, tz);
        ApplyScaleToVertices(std::span<float>(blockModel.vertices.data(), blockModel.vertices.size()), sx, sy, sz);

        // 合并模型
        if (mainModel.vertices.empty()) mainModel = blockModel;
        else MergeModelsDirectly(mainModel, blockModel);
    }
    ApplyPositionOffset(mainModel, x, y, z); // 最终整体偏移
    return mainModel;
}

void LittleTilesTilesEntity::PrintDetails() const {
    std::cout << "LittleTilesTilesEntity - ID: " << id
        << ", X: " << x << ", Y: " << y << ", Z: " << z
        << ", Grid: " << grid << std::endl;

    for (const auto& tile : tiles) {
        std::cout << " Tile - BlockName: " << tile.blockName << std::endl;

        // 打印颜色数组
        std::cout << "  Color: ";
        for (int c : tile.color) {
            std::cout << c << " ";
        }
        std::cout << std::endl;

        // 打印所有 boxData
        if (!tile.boxDataList.empty()) {
            std::cout << "  Boxes:" << std::endl;
            for (size_t i = 0; i < tile.boxDataList.size(); ++i) {
                std::cout << "   Box " << i << ": ";
                for (int v : tile.boxDataList[i]) {
                    std::cout << v << " ";
                }
                std::cout << std::endl;
            }
        }
        else {
            std::cout << "  (No boxes)" << std::endl;
        }
    }

    if (!children.empty()) {
        std::cout << " Children:" << std::endl;
        for (size_t i = 0; i < children.size(); ++i) {
            const auto& child = children[i];
            std::cout << "  Child " << i << " - Coord: ";
            for (int c : child.coord) std::cout << c << " ";
            std::cout << std::endl;
            for (const auto& tile : child.tiles) {
                std::cout << "    Tile - BlockName: " << tile.blockName << std::endl;
            }
        }
    }
}

// 检查面是否被完全覆盖,如果是则应被剔除
static bool IsFaceCovered(LittleFaceState state) {
    return state == LittleFaceState::INSIDE_COVERED || state == LittleFaceState::OUTSIDE_COVERED;
}

ModelData CreateCube(float minX, float minY, float minZ, float maxX, float maxY, float maxZ, const ModelData& templateModel, std::span<const int> faceStates) {
    ModelData cubeModel;

    // 1. 复制材质
    cubeModel.materials = templateModel.materials;

    // 2. 构建面-材质映射
    std::map<FaceType, int> faceMaterialMap;
    int anySideMaterial = -1;
    for (const auto& face : templateModel.faces) {
        if (faceMaterialMap.find(face.faceDirection) == faceMaterialMap.end()) {
            faceMaterialMap[face.faceDirection] = face.materialIndex;
        }
        if (anySideMaterial == -1 && (face.faceDirection == NORTH || face.faceDirection == SOUTH || face.faceDirection == EAST || face.faceDirection == WEST)) {
            anySideMaterial = face.materialIndex;
        }
    }

    auto getMaterialForFace = [&](FaceType dir) -> int {
        auto it = faceMaterialMap.find(dir);
        if (it != faceMaterialMap.end()) {
            return it->second;
        }
        // 侧面的备用方案
        if (dir == NORTH || dir == SOUTH || dir == EAST || dir == WEST) {
            if (anySideMaterial != -1) return anySideMaterial;
        }
        // 顶部/底部的备用方案
        auto it_up = faceMaterialMap.find(UP);
        if (it_up != faceMaterialMap.end()) return it_up->second;
        auto it_down = faceMaterialMap.find(DOWN);
        if (it_down != faceMaterialMap.end()) return it_down->second;

        if (anySideMaterial != -1) return anySideMaterial; // 再次尝试侧面
        if (!faceMaterialMap.empty()) return faceMaterialMap.begin()->second; // 尝试任何一个
        return 0; // 最后手段
    };

    // 3. 为6个独立的面创建顶点 (每个面4个顶点)
    cubeModel.vertices = {
        // 上 (Y max). 从顶部看逆时针: (minX,maxY,minZ), (maxX,maxY,minZ), (maxX,maxY,maxZ), (minX,maxY,maxZ)
        minX, maxY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   minX, maxY, maxZ,
        // 下 (Y min). 从底部看逆时针: (minX,minY,maxZ), (maxX,minY,maxZ), (maxX,minY,minZ), (minX,minY,minZ)
        minX, minY, maxZ,   maxX, minY, maxZ,   maxX, minY, minZ,   minX, minY, minZ,
        // 东 (X max). 从东面看逆时针: (maxX,minY,minZ), (maxX,maxY,minZ), (maxX,maxY,maxZ), (maxX,minY,maxZ)
        maxX, minY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   maxX, minY, maxZ,
        // 西 (X min). 从西面看逆时針: (minX,minY,maxZ), (minX,maxY,maxZ), (minX,maxY,minZ), (minX,minY,minZ)
        minX, minY, maxZ,   minX, maxY, maxZ,   minX, maxY, minZ,   minX, minY, minZ,
        // 北 (Z min). 从北面看逆时针: (minX,minY,minZ), (maxX,minY,minZ), (maxX,maxY,minZ), (minX,maxY,minZ)
        minX, minY, minZ,   maxX, minY, minZ,   maxX, maxY, minZ,   minX, maxY, minZ,
        // 南 (Z max). 从南面看逆时针: (maxX,minY,maxZ), (minX,minY,maxZ), (minX,maxY,maxZ), (maxX,maxY,maxZ)
        maxX, minY, maxZ,   minX, minY, maxZ,   minX, maxY, maxZ,   maxX, maxY, maxZ
    };

    // 4. UV坐标 (每个顶点一对)
    cubeModel.uvCoordinates = {
        // 上 (XZ平面)
        minX, minZ,   maxX, minZ,   maxX, maxZ,   minX, maxZ,
        // 下 (XZ平面)
        minX, maxZ,   maxX, maxZ,   maxX, minZ,   minX, minZ,
        // 东 (ZY平面)
        minZ, minY,   minZ, maxY,   maxZ, maxY,   maxZ, minY,
        // 西 (ZY平面)
        maxZ, minY,   maxZ, maxY,   minZ, maxY,   minZ, minY,
        // 北 (XY平面)
        minX, minY,   maxX, minY,   maxX, maxY,   minX, maxY,
        // 南 (XY平面)
        maxX, minY,   minX, minY,   minX, maxY,   maxX, maxY
    };

    // 5. 面 - 根据faceStates决定是否生成
    struct FaceInfo {
        FaceType type;
        std::array<int, 4> vertIndices;
        std::array<int, 4> uvIndices;
    };

    const std::vector<FaceInfo> faceInfos = {
        {UP,    {0, 1, 2, 3},    {0, 1, 2, 3}},
        {DOWN,  {4, 5, 6, 7},    {4, 5, 6, 7}},
        {EAST,  {8, 9, 10, 11},  {8, 9, 10, 11}},
        {WEST,  {12, 13, 14, 15}, {12, 13, 14, 15}},
        {NORTH, {16, 17, 18, 19}, {16, 17, 18, 19}},
        {SOUTH, {20, 21, 22, 23}, {20, 21, 22, 23}}
    };

    // 根据 EntityBlock.h 中的注释, faceStates 的顺序是 EAST, WEST, UP, DOWN, SOUTH, NORTH
    std::map<FaceType, LittleFaceState> stateMap;
    if (faceStates.size() >= 6) {
        stateMap[FaceType::UP] = static_cast<LittleFaceState>(faceStates[0]);
        stateMap[FaceType::DOWN] = static_cast<LittleFaceState>(faceStates[1]);
        stateMap[FaceType::SOUTH] = static_cast<LittleFaceState>(faceStates[2]);
        stateMap[FaceType::NORTH] = static_cast<LittleFaceState>(faceStates[3]);
        stateMap[FaceType::EAST] = static_cast<LittleFaceState>(faceStates[4]);
        stateMap[FaceType::WEST] = static_cast<LittleFaceState>(faceStates[5]);
    }


    for (const auto& info : faceInfos) {
        LittleFaceState state = LittleFaceState::UNLOADED;
        if (stateMap.count(info.type)) {
            state = stateMap.at(info.type);
        }
        if (!IsFaceCovered(state)) {
            cubeModel.faces.push_back({ info.vertIndices, info.uvIndices, getMaterialForFace(info.type), info.type });
        }
    }

    return cubeModel;
}

// --- 新增辅助函数 ---
// 根据给定的tiles和grid生成模型,不应用最终的位置偏移
static ModelData GenerateModelFromTiles(const std::vector<LittleTilesTileEntry>& tiles, int grid) {
    ModelData model;
    float grid_f = static_cast<float>(grid);

    for (const auto& tile : tiles) {
        // 获取方块类型的模板模型
        std::string fullBlockName = tile.blockName;
        std::string ns = "minecraft"; // 默认命名空间
        std::string blockName;
        size_t colonPos = fullBlockName.find(':');
        if (colonPos != std::string::npos) {
            ns = fullBlockName.substr(0, colonPos);
            blockName = fullBlockName.substr(colonPos + 1);
        }
        else {
            blockName = fullBlockName;
        }

        ModelData templateModel = GetRandomModelFromCache(ns, blockName);
        if (templateModel.vertices.empty() && !blockName.empty()) {
            ProcessBlockstate(ns, { blockName });
            templateModel = GetRandomModelFromCache(ns, blockName);
        }

        if (templateModel.materials.empty()) {
            // 如果没有材质,创建一个虚拟材质以防止崩溃
            Material dummyMaterial;
            dummyMaterial.name = "dummy";
            dummyMaterial.texturePath = "None";
            templateModel.materials.push_back(dummyMaterial);
        }

        for (const auto& boxData : tile.boxDataList) {
            if (boxData.size() != 12) continue; // 现在需要12个元素

            // 从 boxData 中提取位置和大小
            int minX = boxData[6];
            int minY = boxData[7];
            int minZ = boxData[8];
            int maxX = boxData[9];
            int maxY = boxData[10];
            int maxZ = boxData[11];

            // 为该 box 创建立方体模型,并传入面状态
            ModelData cube = CreateCube(minX / grid_f, minY / grid_f, minZ / grid_f, maxX / grid_f, maxY / grid_f, maxZ / grid_f, templateModel, std::span<const int>(boxData.data(), 6));

            // 合并到最终模型中
            if (model.vertices.empty()) {
                model = cube;
            }
            else {
                MergeModelsDirectly(model, cube);  // 假设 MergeModelsDirectly 已定义
            }
        }
    }
    return model;
}

ModelData LittleTilesTilesEntity::GenerateModel() const {
    // 1. 生成父结构的本地模型
    ModelData finalModel = GenerateModelFromTiles(this->tiles, this->grid);

    // 2. 遍历子结构,生成它们的模型并合并
    for (const auto& child : this->children) {
        // 假设子结构继承父结构的 grid 设置
        ModelData childModel = GenerateModelFromTiles(child.tiles, this->grid);

        // 应用子结构的相对坐标偏移(根据grid缩放)
        if (child.coord.size() >= 3) {
            float grid_f = static_cast<float>(this->grid);
            double offsetX = static_cast<double>(child.coord[0]) / grid_f;
            double offsetY = static_cast<double>(child.coord[1]) / grid_f;
            double offsetZ = static_cast<double>(child.coord[2]) / grid_f;
            ApplyDoublePositionOffset(childModel, offsetX, offsetY, offsetZ);
        }

        // 合并到最终模型中
        if (finalModel.vertices.empty()) {
            finalModel = childModel;
        }
        else {
            MergeModelsDirectly(finalModel, childModel);
        }
    }

    // 3. 对最终合并的模型应用实体自身的绝对坐标偏移
    ApplyPositionOffset(finalModel, x, y, z);
    return finalModel;
}
