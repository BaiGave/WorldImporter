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

// ---------- 共面叠加层元数据（overlay.json） ----------
// 一层叠加层：导出器侧的完整材质名、贴图路径（相对 importer 目录）、tint（线性色约定同 tint.json）
struct OverlayLayerInfo {
    std::string name;
    std::string texturePath;
    TintResult tint;
};

// 登记 base 材质 -> 叠加层序列（按绘制顺序，底层在前）。
// 同一 base 只保留首组序列；后续序列不一致时保留首组并告警一次。
void RegisterOverlaySequence(const std::string& baseName, const std::vector<OverlayLayerInfo>& overlays);

// 输出 overlay.json（base 材质全名 -> [{name, texture, kind, color?}, ...]）
void CreateOverlayJsonFile();
#endif