#include "PointCloudExporter.h"
#include "biome.h"
#include "block.h"
#include "model.h"
#include "blockstate.h"
#include "ObjExporter.h"
#include <chrono>
#include <sstream>
#include <iostream>
#include <windows.h>  // 包含 Windows API 头文件
#include <direct.h>
#include <algorithm>  // 包含 replace 函数

using namespace std;

PointCloudExporter::PointCloudExporter(const string& objFileName, const string& jsonFileName) {
    // 获取可执行文件目录路径
    string exeDir = getExecutableDir();
    string modelsPath = exeDir + "models\\";  // models 文件夹路径

    // 确保 models 文件夹存在
    if (_mkdir(modelsPath.c_str()) != 0 && errno != EEXIST) {
        cerr << "Failed to create directory: " << modelsPath << endl;
        exit(1);
    }

    // 构造完整路径
    string fullObjPath = exeDir + objFileName;
    string fullJsonPath = exeDir + jsonFileName;

    // 打开输出文件流
    objFile.open(fullObjPath);
    if (!objFile.is_open()) {
        cerr << "Failed to open file " << fullObjPath << endl;
        exit(1);
    }

    jsonFile.open(fullJsonPath);
    if (!jsonFile.is_open()) {
        cerr << "Failed to open file " << fullJsonPath << endl;
        exit(1);
    }

    
}

void PointCloudExporter::PreprocessBlockModels(std::vector<Block> blockPalette) {
    // 获取可执行文件目录路径
    string exeDir = getExecutableDir();
    string modelsPath = exeDir + "models\\";  // models 文件夹路径

    // 确保 models 文件夹存在
    if (_mkdir(modelsPath.c_str()) != 0 && errno != EEXIST) {
        cerr << "Failed to create directory: " << modelsPath << endl;
        exit(1);
    }

    // 遍历 blockPalette 处理每个方块
    for (size_t index = 0; index < blockPalette.size(); ++index) {
        const string& blockName = blockPalette[index].GetBlockNameWithoutProperties();
        string fullBlockName = blockName;

        // 分割命名空间和方块 ID(假设 blockName 已包含状态)
        size_t colonPos = fullBlockName.find(':');
        string namespaceName = fullBlockName.substr(0, colonPos);
        string blockIdWithState = fullBlockName.substr(colonPos + 1);

        // 替换非法字符,确保文件名合法
        replace(blockIdWithState.begin(), blockIdWithState.end(), ':', '=');

        // 调用 ProcessBlockstateJson 获取模型数据
        vector<string> modelBlockIds = { blockIdWithState };
        unordered_map<string, ModelData> models = ProcessBlockstateJson(namespaceName, modelBlockIds);

        if (!models.empty()) {
            // 获取模型数据
            ModelData modelData = models[blockIdWithState];

            // 生成文件名,前面加上索引和 #
            string fileName = "#" + std::to_string(index) + "_" + blockIdWithState;

            // 调用模型生成函数(文件名前缀拼接 models/)
            string outputPath = "models//" + fileName; // 注意:Windows 路径分隔符是反斜杠
            CreateModelFiles(modelData, outputPath);
        }
    }
}

void PointCloudExporter::ExportPointCloud() {
    // 初始化坐标范围
    const int xStart = config.minX, xEnd = config.maxX;
    const int yStart = config.minY, yEnd = config.maxY;
    const int zStart = config.minZ, zEnd = config.maxZ;
    // 遍历所有方块位置
    for (int x = xStart; x <= xEnd; ++x) {
        for (int y = yStart; y <= yEnd; ++y) {
            for (int z = zStart; z <= zEnd; ++z) {
                int blockId = GetBlockId(x, y, z);
                Block b = GetBlockById(blockId);
                std::string blockName = b.GetModifiedNameWithNamespace();
                /*if (blockName=="minecraft:azalea_leaves[]")
                {
                    std::cout << blockName;
                }*/
                // 如果方块不是 "minecraft:air",则导出该方块位置为点
                if (blockName != "minecraft:air") {
                    // 将该点作为顶点写入 .obj 文件,格式:v x y z id
                    objFile << "v " << x << " " << y << " " << z << " " << blockId << endl;
                }
            }
        }
    }
    // 写入 JSON 文件
    auto blockPalette = GetGlobalBlockPalette();
    // 预先生成所有方块的模型文件
    PreprocessBlockModels(blockPalette);
    nlohmann::json blockIdMapping;

    for (size_t index = 0; index < blockPalette.size(); ++index) {
        blockIdMapping[std::to_string(index)] = blockPalette[index].name;
    }


    // 写入 JSON 文件(格式化输出)
    jsonFile << blockIdMapping.dump(4);  // 使用 4 空格进行格式化

    // 关闭文件流
    objFile.close();
    jsonFile.close();
}

// 静态方法实现
void PointCloudExporter::ExportPointCloudToFile(const std::string& objFileName, const std::string& jsonFileName) {
    // 创建 PointCloudExporter 实例
    PointCloudExporter exporter(objFileName, jsonFileName);
    
    // 调用实例方法导出点云 - 这里使用无参数的实例方法
    exporter.ExportPointCloud();
}