#ifndef OBJEXPORTER_H
#define OBJEXPORTER_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cmath>
#include "include/json.hpp"
#include <mutex>
#include <future>
#include "JarReader.h"
#include "config.h"
#include "texture.h"
#include "GlobalCache.h"
#include "model.h"
#pragma once
// 文件导出
void CreateModelFiles(const ModelData& data, const std::string& filename);

void CreateMultiModelFiles(const ModelData& data, const std::string& filename,
    std::unordered_map<std::string, std::string>& uniqueMaterialsL,
    const std::string& sharedMtlName);


// 在objExporter.h中添加新函数声明
void createSharedMtlFile(std::unordered_map<std::string, std::string> uniqueMaterials, const std::string& mtlFileName);
#endif