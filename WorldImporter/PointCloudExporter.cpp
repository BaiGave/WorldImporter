// PointCloudExporter.cpp
#include "PointCloudExporter.h"
#include "biome_mapping.h"
#include "block.h"
#include <chrono>
#include <sstream>
#include <iostream>

using namespace std;

PointCloudExporter::PointCloudExporter(const string& objFileName, const string& jsonFileName) {
    // 打开输出文件流
    objFile.open(objFileName);
    if (!objFile.is_open()) {
        cerr << "Failed to open file " << objFileName << endl;
        exit(1);
    }

    jsonFile.open(jsonFileName);
    if (!jsonFile.is_open()) {
        cerr << "Failed to open file " << jsonFileName << endl;
        exit(1);
    }

    // 写入 .obj 文件头部注释
    objFile << "# Exported from Minecraft data\n";
}

void PointCloudExporter::ExportPointCloud(int xStart, int xEnd, int yStart, int yEnd, int zStart, int zEnd) {
    // 遍历所有方块位置
    for (int x = xStart; x <= xEnd; ++x) {
        for (int y = yStart; y <= yEnd; ++y) {
            for (int z = zStart; z <= zEnd; ++z) {
                string blockName = GetBlockNameById(GetBlockId(x, y, z));
                int blockId = GetBlockId(x, y, z);

                // 如果方块不是 "minecraft:air"，则导出该方块位置为点
                if (blockName != "minecraft:air") {
                    // 将该点作为顶点写入 .obj 文件，格式： v x y z id
                    objFile << "v " << x << " " << y << " " << z << " " << blockId << endl;
                }
            }
        }
    }

    // 输出方块调色板
    blockPalette = GetGlobalBlockPalette();
    blockIdMapping = nlohmann::json::object();

    for (const auto& entry : blockPalette) {
        blockIdMapping[std::to_string(entry.first)] = entry.second;
    }

    // 写入 JSON 文件（格式化输出）
    jsonFile << blockIdMapping.dump(4);  // 使用 4 空格进行格式化输出

    // 关闭文件流
    objFile.close();
    jsonFile.close();
}
