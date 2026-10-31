#pragma once

#include <string>
#include "include/json.hpp"

// 1.18.x–1.19.2 客户端 jar 不含 data/minecraft/worldgen/biome。
// 未知 id 返回 null。命中时返回与 jar 群系 JSON 同形的对象(省略默认项)。
nlohmann::json LegacyVanillaBiomeJson(const std::string& biomeId);

// 内置表条目数。1.19.2 原版报告为 63。
int LegacyVanillaBiomeCount();
