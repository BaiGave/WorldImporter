// PointCloudExporter.h
#ifndef POINT_CLOUD_EXPORTER_H
#define POINT_CLOUD_EXPORTER_H

#include <string>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include "block.h"
#include "nlohmann/json.hpp"  // 导入 nlohmann::json

class PointCloudExporter {
public:
    // 构造函数，传入导出的文件名
    PointCloudExporter(const std::string& objFileName, const std::string& jsonFileName);

    // 导出点云到 .obj 文件
    void ExportPointCloud(int xStart, int xEnd, int yStart, int yEnd, int zStart, int zEnd);


private:
    std::ofstream objFile;  // 输出 .obj 文件流
    std::ofstream jsonFile; // 输出 JSON 文件流
    std::unordered_map<int, std::string> blockPalette;  // 方块调色板
    nlohmann::json blockIdMapping;  // 用于存储 ID 对照表
};

#endif // POINT_CLOUD_EXPORTER_H
