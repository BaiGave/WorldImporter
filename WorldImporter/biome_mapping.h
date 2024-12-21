#ifndef BIOME_MAPPING_H
#define BIOME_MAPPING_H

#include "include/nlohmann/json.hpp"
#include <unordered_map>
#include <string>

using json = nlohmann::json;

extern std::unordered_map<std::string, int> biomeMapping;

// 加载群系对照表
void loadBiomeMapping(const std::string& filepath);


// 根据群系名获取ID
int getBiomeId(const std::string& biome);

#endif // BIOME_MAPPING_H
