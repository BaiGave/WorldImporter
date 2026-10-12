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


// 在ObjExporter.h中添加新函数声明
void CreateSharedMtlFile(std::unordered_map<std::string, std::string> uniqueMaterials, const std::string& mtlFileName, const std::unordered_map<std::string, TintResult>& uniqueTints = {});

// 输出 tint.json（材质名 -> {on, kind, color?}），供 Blender 插件读取
void CreateTintJsonFile(const std::unordered_map<std::string, TintResult>& uniqueTints);
#endif