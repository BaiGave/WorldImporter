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

    // 安全读取配置，如果字段不存在则保留默认值
    config.worldPath = j.value("worldPath", config.worldPath);
    config.jarPath = j.value("jarPath", config.jarPath);
    config.versionJsonPath = j.value("versionJsonPath", config.versionJsonPath);
    config.modsPath = j.value("modsPath", config.modsPath);
    
    if (j.contains("resourcepacksPaths")) {
        config.resourcepacksPaths = j["resourcepacksPaths"];
    }

    config.minX = j.value("minX", config.minX);
    config.maxX = j.value("maxX", config.maxX);
    config.minY = j.value("minY", config.minY);
    config.maxY = j.value("maxY", config.maxY);
    config.minZ = j.value("minZ", config.minZ);
    config.maxZ = j.value("maxZ", config.maxZ);
    config.status = j.value("status", config.status);

    config.useChunkPrecision = j.value("useChunkPrecision", config.useChunkPrecision);
    config.keepBoundary = j.value("keepBoundary", config.keepBoundary);
    config.strictDeduplication = j.value("strictDeduplication", config.strictDeduplication);
    config.cullCave = j.value("cullCave", config.cullCave);
    config.exportLightBlock = j.value("exportLightBlock", config.exportLightBlock);
    config.exportLightBlockOnly = j.value("exportLightBlockOnly", config.exportLightBlockOnly);
    config.lightBlockSize = j.value("lightBlockSize", config.lightBlockSize);
    config.allowDoubleFace = j.value("allowDoubleFace", config.allowDoubleFace);
    config.isLODAutoCenter = j.value("isLODAutoCenter", config.isLODAutoCenter);
    config.LODCenterX = j.value("LODCenterX", config.LODCenterX);
    config.LODCenterZ = j.value("LODCenterZ", config.LODCenterZ);
    config.LOD0renderDistance = j.value("LOD0renderDistance", config.LOD0renderDistance);
    config.LOD1renderDistance = j.value("LOD1renderDistance", config.LOD1renderDistance);
    config.LOD2renderDistance = j.value("LOD2renderDistance", config.LOD2renderDistance);
    config.LOD3renderDistance = j.value("LOD3renderDistance", config.LOD3renderDistance);
    config.useUnderwaterLOD = j.value("useUnderwaterLOD", config.useUnderwaterLOD);
    config.useGreedyMesh = j.value("useGreedyMesh", config.useGreedyMesh);
    config.activeLOD = j.value("activeLOD", config.activeLOD);
    config.activeLOD2 = j.value("activeLOD2", config.activeLOD2);
    config.activeLOD3 = j.value("activeLOD3", config.activeLOD3);
    config.useBiomeColors = j.value("useBiomeColors", config.useBiomeColors);
    
    // 读取LOD1级别使用原始模型的方块列表
    /*格式：
    "lod1Blocks": [
        "minecraft:packed_ice",
        "minecraft:campfire",
        "minecraft:cherry_leaves"
    ]*/
    if (j.contains("lod1Blocks") && j["lod1Blocks"].is_array()) {
        for (const auto& block : j["lod1Blocks"]) {
            if (block.is_string()) {
                config.lod1Blocks.insert(block.get<std::string>());
            }
        }
    }

    config.exportFullModel = j.value("exportFullModel", config.exportFullModel);
    config.partitionSize = j.value("partitionSize", config.partitionSize);
    
    // 读取每批次的区块任务数量上限（如果存在）
    config.maxTasksPerBatch = j.value("maxTasksPerBatch", config.maxTasksPerBatch);


    config.selectedDimension = j.value("selectedDimension", config.selectedDimension);

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
