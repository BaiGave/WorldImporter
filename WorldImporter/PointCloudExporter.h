// PointCloudExporter.h
#ifndef POINT_CLOUD_EXPORTER_H
#define POINT_CLOUD_EXPORTER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <iostream>
#include <fstream>
#include "block.h"
#include "include/json.hpp"

class PointCloudExporter {
public:
    // 构造函数,传入导出的文件名
    PointCloudExporter(const std::string& objFileName, const std::string& jsonFileName);

    // 预先生成所有方块的模型文件
    void PreprocessBlockModels(std::vector<Block> blockPalette);

    // 导出点云到 .obj 文件 - 实例方法,无参数版本
    void ExportPointCloud();
    
    // 静态方法,直接导出点云 - 包含文件名参数
    static void ExportPointCloudToFile(const std::string& objFileName = "output.obj", const std::string& jsonFileName = "block_id_mapping.json");

private:
    std::ofstream objFile;  // 输出 .obj 文件流
    std::ofstream jsonFile; // 输出 JSON 文件流
    std::vector<std::string> blockPalette;  // 方块调色板
    nlohmann::json blockIdMapping;  // 用于存储 ID 对照表
};

#endif // POINT_CLOUD_EXPORTER_H