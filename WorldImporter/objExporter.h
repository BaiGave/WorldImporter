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

// Blender 材质名（63 字节）安全化：超长时保留前 54 字节 + "~" + FNV-1a 哈希，避免
// Blender 静默截断后 tint.json / overlay.json / doubleSided.json 的键对不上。
// MTL/OBJ 与各元数据 json 的写出必须统一走这个函数。
std::string BlenderSafeName(const std::string& name);

// 登记"完整材质名"：只有被 BlenderSafeName 截断的名字才需要登记。
// 导出收尾时 WriteMaterialsJson() 落盘为 materials.json（可见名 -> 完整名），
// 插件导入后按可见名查回原名（用于分类），并存进材质节点树。
void RegisterMaterialFullName(const std::string& fullName);
void WriteMaterialsJson();

void CreateMultiModelFiles(const ModelData& data, const std::string& filename,
    std::unordered_map<std::string, std::string>& uniqueMaterialsL,
    const std::string& sharedMtlName);


// 在ObjExporter.h中添加新函数声明
void CreateSharedMtlFile(std::unordered_map<std::string, std::string> uniqueMaterials, const std::string& mtlFileName, const std::unordered_map<std::string, TintResult>& uniqueTints = {});

// 输出 tint.json（材质名 -> {on, kind, color?}），供 Blender 插件读取
void CreateTintJsonFile(const std::unordered_map<std::string, TintResult>& uniqueTints);

// tint 诊断：检查 tint 用到的群系色型是否都有对应 biomeTex 图；缺图时 Blender 侧
// 取不到颜色（发白/发灰）。mergedModel 可为空（分组导出路径），非空时把其中材质的
// tint 也计入。
void CheckBiomeTexCoverage(const std::unordered_map<std::string, TintResult>& uniqueTints,
                           const ModelData* mergedModel);

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

// overlay.json 里引用到的全部材质名（base 与各层 name），供材质剪枝时保留
std::unordered_set<std::string> GetOverlayReferencedMaterialNames();

// ---------- 双面几何元数据（doubleSided.json） ----------
// 输出 doubleSided.json：对象格式。materials 是使用了双面几何（零厚度贴片被
// SeparateCoincidentFaces 分离出正反两面）的材质基础全名数组（已剥掉 @tint 后缀，
// 与 Crafter 侧 material_base_name() 的查询口径一致）；samePairs / mirrorPairs /
// otherPairs 按材质记录三类 UV 关系的对数，mergedPairs 记录实际合并掉的 Same 对数。
// 供 Blender 侧按材质名判断是否关闭背面剔除；未使用双面几何时 materials 为空数组。
void CreateDoubleSidedJsonFile();
#endif