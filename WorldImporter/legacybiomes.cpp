#include "legacybiomes.h"

#include <cstring>
#include <string>

// 1.18.x–1.19.2 原版群系。由 tests/legacybiomes/gen_legacy_biomes.py 生成。
namespace {

struct LegacyBiome {
    const char* id;
    double temperature;
    double downfall;
    const char* temperatureModifier; // "none" / "frozen"
    int skyColor;
    int fogColor;
    int waterColor;
    int waterFogColor;
    int grassColor;                  // -1 表示 JSON 中省略
    int foliageColor;                // -1 表示 JSON 中省略
    const char* grassModifier;       // "none" / "swamp" / "dark_forest"
};

// 按 id 字典序, 一行一个群系。
const LegacyBiome kLegacyBiomes[] = {
#include "legacybiomes.inc"
};

const LegacyBiome* FindLegacyBiome(const std::string& biomeId) {
    for (const LegacyBiome& biome : kLegacyBiomes) {
        if (biomeId == biome.id) return &biome;
    }
    return nullptr;
}

bool IsNone(const char* value) {
    return value == nullptr || std::strcmp(value, "none") == 0;
}

} // namespace

nlohmann::json LegacyVanillaBiomeJson(const std::string& biomeId) {
    const LegacyBiome* biome = FindLegacyBiome(biomeId);
    if (biome == nullptr) return nlohmann::json();

    // 键集合与 jar 一致: 没有的颜色和 "none" 修饰符都不写出。
    nlohmann::json effects = nlohmann::json::object();
    effects["sky_color"] = biome->skyColor;
    effects["fog_color"] = biome->fogColor;
    effects["water_color"] = biome->waterColor;
    effects["water_fog_color"] = biome->waterFogColor;
    if (biome->grassColor >= 0) effects["grass_color"] = biome->grassColor;
    if (biome->foliageColor >= 0) effects["foliage_color"] = biome->foliageColor;
    if (!IsNone(biome->grassModifier)) effects["grass_color_modifier"] = biome->grassModifier;

    nlohmann::json json = nlohmann::json::object();
    json["temperature"] = biome->temperature;
    json["downfall"] = biome->downfall;
    if (!IsNone(biome->temperatureModifier)) json["temperature_modifier"] = biome->temperatureModifier;
    json["effects"] = std::move(effects);
    return json;
}

int LegacyVanillaBiomeCount() {
    return static_cast<int>(sizeof(kLegacyBiomes) / sizeof(kLegacyBiomes[0]));
}
