#include "config.h" 
#include "locutil.h"
#include <locale>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include "include/json.hpp"
Config LoadConfig(const std::string& configFile) {
    Config config;
    std::ifstream file(configFile);

    if (!file.is_open()) {
        std::cerr << "Could not open config file: " << configFile << std::endl;
        return config;  // 返回默认配置
    }

    nlohmann::json j;
    file >> j;

    // Read and populate config fields from JSON
    config.worldPath = j["worldPath"];
    config.jarPath = j["jarPath"];
    config.versionJsonPath = j["versionJsonPath"];
    config.modsPath = j["modsPath"];
    config.resourcepacksPaths = j["resourcepacksPaths"];

    config.minX = j["minX"];
    config.maxX = j["maxX"];
    config.minY = j["minY"];
    config.maxY = j["maxY"];
    config.minZ = j["minZ"];
    config.maxZ = j["maxZ"];
    config.status = j["status"];

    config.useChunkPrecision = j["useChunkPrecision"];
    config.keepBoundary = j["keepBoundary"];
    config.strictDeduplication = j["strictDeduplication"];
    config.cullCave = j["cullCave"];
    config.exportLightBlock = j["exportLightBlock"];
    config.exportLightBlockOnly = j["exportLightBlockOnly"];
    config.lightBlockSize = j["lightBlockSize"];
    config.allowDoubleFace = j["allowDoubleFace"];
    config.activeLOD = j["activeLOD"];
    config.isLODAutoCenter = j["isLODAutoCenter"];
    config.LODCenterX = j["LODCenterX"];
    config.LODCenterZ = j["LODCenterZ"];
    config.LOD0renderDistance = j["LOD0renderDistance"];
    config.LOD1renderDistance = j["LOD1renderDistance"];
    config.LOD2renderDistance = j["LOD2renderDistance"];
    config.LOD3renderDistance = j["LOD3renderDistance"];
    config.useUnderwaterLOD = j["useUnderwaterLOD"];
    config.useGreedyMesh = j["useGreedyMesh"];

    config.exportFullModel = j["exportFullModel"];
    config.partitionSize = j["partitionSize"];

    // 区块对齐处理
    if (config.useChunkPrecision) {
        config.minX = alignTo16(config.minX); config.maxX = alignTo16(config.maxX);
        config.minY = alignTo16(config.minY); config.maxY = alignTo16(config.maxY);
        config.minZ = alignTo16(config.minZ); config.maxZ = alignTo16(config.maxZ);
    }

    blockToChunk(config.minX, config.minZ, config.chunkXStart, config.chunkZStart);
    blockToChunk(config.maxX, config.maxZ, config.chunkXEnd, config.chunkZEnd);
    blockYToSectionY(config.minY, config.sectionYStart);
    blockYToSectionY(config.maxY, config.sectionYEnd);
    // 自动计算LOD中心
    if (config.isLODAutoCenter) {
        config.LODCenterX = (config.chunkXStart + config.chunkXEnd) / 2;
        config.LODCenterZ = (config.chunkZStart + config.chunkZEnd) / 2;
    }
    return config;
}
