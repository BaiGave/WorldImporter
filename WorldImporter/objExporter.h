#ifndef OBJEXPORTER_H
#define OBJEXPORTER_H

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cmath>
#include <nlohmann/json.hpp>  // 用于解析 JSON
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

#endif