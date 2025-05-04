#include "EntityBlock.h"
#include "RegionModelExporter.h"  // 包含必要的头文件,确保相关函数可用


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
        << ", X: " << x << ", Y: " << y << ", Z: " << z << std::endl;

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
}

ModelData CreateCube(float minX, float minY, float minZ, float maxX, float maxY, float maxZ, const std::string& texturePath) {
    ModelData cubeModel;
    float halfSizeX = (maxX - minX) / 2.0f;
    float halfSizeY = (maxY - minY) / 2.0f;
    float halfSizeZ = (maxZ - minZ) / 2.0f;

    cubeModel.vertices = {
        // 前面
        minX, maxY, maxZ, maxX, maxY, maxZ, maxX, minY, maxZ, minX, minY, maxZ,
        // 后面
        maxX, maxY, minZ, minX, maxY, minZ, minX, minY, minZ, maxX, minY, minZ,
        // 上面
        maxX, maxY, maxZ, minX, maxY, maxZ, minX, maxY, minZ, maxX, maxY, minZ,
        // 下面
        minX, minY, maxZ, maxX, minY, maxZ, maxX, minY, minZ, minX, minY, minZ,
        // 左面
        minX, maxY, minZ, minX, maxY, maxZ, minX, minY, maxZ, minX, minY, minZ,
        // 右面
        maxX, maxY, maxZ, maxX, maxY, minZ, maxX, minY, minZ, maxX, minY, maxZ
    };

    // 设置 UV 坐标(此处是示例,实际可以根据不同的纹理进行调整)
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
    cubeModel.faces[0].faceDirection = FaceType::SOUTH;
    
    // 后面
    cubeModel.faces[1].vertexIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].uvIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].materialIndex = 0;
    cubeModel.faces[1].faceDirection = FaceType::NORTH;
    
    // 上面
    cubeModel.faces[2].vertexIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].uvIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].materialIndex = 0;
    cubeModel.faces[2].faceDirection = FaceType::UP;
    
    // 下面
    cubeModel.faces[3].vertexIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].uvIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].materialIndex = 0;
    cubeModel.faces[3].faceDirection = FaceType::DOWN;
    
    // 左面
    cubeModel.faces[4].vertexIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].uvIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].materialIndex = 0;
    cubeModel.faces[4].faceDirection = FaceType::WEST;
    
    // 右面
    cubeModel.faces[5].vertexIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].uvIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].materialIndex = 0;
    cubeModel.faces[5].faceDirection = FaceType::EAST;

    return cubeModel;
}


ModelData LittleTilesTilesEntity::GenerateModel() const {
    ModelData model;
    for (const auto& tile : tiles) {
        for (const auto& boxData : tile.boxDataList) {
            if (boxData.size() !=12) continue; // 确保 boxData 有效

            // 从 boxData 中提取位置和大小
            int minX = boxData[6];
            int minY = boxData[7];
            int minZ = boxData[8];
            int maxX = boxData[9];
            int maxY = boxData[10];
            int maxZ = boxData[11];

            // 为该 box 创建立方体模型
            ModelData cube = CreateCube(minX / 16.0f, minY / 16.0f, minZ / 16.0f, maxX / 16.0f, maxY / 16.0f, maxZ / 16.0f, tile.blockName);

            // 合并到最终模型中
            if (model.vertices.empty()) {
                model = cube;
            }
            else {
                MergeModelsDirectly(model, cube);  // 假设 MergeModelsDirectly 已定义
            }
        }
    }
    ApplyPositionOffset(model, x, y, z); // 最终整体偏移
    return model;
}
