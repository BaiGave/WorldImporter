#include "biome.h"
#include "GlobalCache.h"
#include "legacybiomes.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// WorldImporterCore 里多个翻译单元引用这个全局量, 定义在 main.cpp。
Config config;

namespace {
using Json = nlohmann::json;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

double number(const Json& json, const char* key) {
    require(json.contains(key) && json.at(key).is_number(), std::string("missing number ") + key);
    return json.at(key).get<double>();
}

int color(const Json& effects, const char* key) {
    require(effects.contains(key) && effects.at(key).is_number_integer(), std::string("missing color ") + key);
    return effects.at(key).get<int>();
}

void expectExact(double actual, double expected, const std::string& label) {
    if (actual != expected) {
        std::ostringstream error;
        error << label << ": expected " << expected << ", got " << actual;
        throw std::runtime_error(error.str());
    }
}

// 把仓库里的原版色图塞进缓存, 这样平原草色可以不靠 jar 计算。
bool injectColormap(const std::string& name) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "colormap" / (name + ".png");
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty()) return false;
    const std::string cacheKey = "legacy-test:minecraft:" + name;
    std::unique_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    GlobalCache::colormaps[cacheKey] = std::move(bytes);
    GlobalCache::colormapIndex["colormaps:minecraft:" + name] = cacheKey;
    return true;
}

void checkSwamp(const Json& json) {
    expectExact(number(json, "temperature"), 0.8, "swamp temperature");
    expectExact(number(json, "downfall"), 0.9, "swamp downfall");
    require(!json.contains("temperature_modifier"), "swamp has no temperature_modifier");
    require(!json.contains("precipitation") && !json.contains("has_precipitation"),
            "precipitation is not read by biome.cpp");
    const auto& effects = json.at("effects");
    require(color(effects, "foliage_color") == 6975545, "swamp foliage");
    require(color(effects, "water_color") == 6388580, "swamp water");
    require(!effects.contains("grass_color"), "swamp omits grass_color");
    require(effects.at("grass_color_modifier") == "swamp", "swamp grass_color_modifier");
}

void checkSamples() {
    require(LegacyVanillaBiomeCount() == 63, "expected 63 legacy biomes");

    const Json swamp = LegacyVanillaBiomeJson("swamp");
    require(swamp.is_object(), "swamp missing");
    checkSwamp(swamp);
    const Json viaGetter = Biome::GetBiomeJson("minecraft", "swamp");
    require(viaGetter == swamp, "GetBiomeJson must return the legacy swamp object");

    const Json dark = LegacyVanillaBiomeJson("dark_forest");
    require(dark.at("effects").at("grass_color_modifier") == "dark_forest", "dark_forest modifier");
    require(!dark.at("effects").contains("grass_color"), "dark_forest omits grass_color");
    require(!dark.contains("temperature_modifier"), "dark_forest temperature_modifier");

    const Json frozen = LegacyVanillaBiomeJson("frozen_ocean");
    expectExact(number(frozen, "temperature"), 0.0, "frozen_ocean temperature");
    expectExact(number(frozen, "downfall"), 0.5, "frozen_ocean downfall");
    require(frozen.at("temperature_modifier") == "frozen", "frozen_ocean temperature_modifier");

    const Json deepFrozen = LegacyVanillaBiomeJson("deep_frozen_ocean");
    expectExact(number(deepFrozen, "temperature"), 0.5, "deep_frozen_ocean temperature");
    require(deepFrozen.at("temperature_modifier") == "frozen", "deep_frozen_ocean temperature_modifier");

    const Json badlands = LegacyVanillaBiomeJson("badlands");
    const auto& badlandsFx = badlands.at("effects");
    require(color(badlandsFx, "grass_color") == 9470285, "badlands grass");
    require(color(badlandsFx, "foliage_color") == 10387789, "badlands foliage");
    require(!badlandsFx.contains("grass_color_modifier"), "badlands omits grass modifier");

    require(LegacyVanillaBiomeJson("deep_dark").is_object(), "deep_dark missing");
    const Json mangrove = LegacyVanillaBiomeJson("mangrove_swamp");
    require(mangrove.is_object(), "mangrove_swamp missing");
    require(mangrove.at("effects").at("grass_color_modifier") == "swamp", "mangrove modifier");
    require(color(mangrove.at("effects"), "foliage_color") == 9285927, "mangrove foliage");

    require(LegacyVanillaBiomeJson("not_a_biome").is_null(), "unknown id must be null");
    require(LegacyVanillaBiomeJson("").is_null(), "empty id must be null");
    require(LegacyVanillaBiomeJson("minecraft:swamp").is_null(), "namespaced id is not a table key");
}

void checkPlainsColors() {
    const Json plains = LegacyVanillaBiomeJson("plains");
    expectExact(number(plains, "temperature"), 0.8, "plains temperature");
    expectExact(number(plains, "downfall"), 0.4, "plains downfall");
    require(!plains.contains("temperature_modifier"), "plains omits temperature_modifier");
    const auto& effects = plains.at("effects");
    require(!effects.contains("grass_color") && !effects.contains("foliage_color"),
            "plains plant colors come from the colormap");
    require(!effects.contains("grass_color_modifier"), "plains omits grass modifier");
    require(color(effects, "water_color") == 4159204, "plains water");
    require(color(effects, "fog_color") == 12638463, "plains fog");
    require(color(effects, "sky_color") == 7907327, "plains sky");

    // 草/树叶色图都注入后再注册, ParseBiomeColors 只会跑一次。
    const bool haveGrass = injectColormap("grass");
    injectColormap("foliage");
    const int id = Biome::GetId("minecraft:plains");
    require(Biome::GetColor(id, BiomeColorType::Water) == 4159204, "parsed plains water");
    require(Biome::GetColor(id, BiomeColorType::Fog) == 12638463, "parsed plains fog");
    require(Biome::GetColor(id, BiomeColorType::Sky) == 7907327, "parsed plains sky");

    const int grass = Biome::GetColor(id, BiomeColorType::Grass);
    if (!haveGrass) {
        std::cout << "skip plains grass 0x91BD59 (colormap missing)\n";
        return;
    }
    if (grass != 0x91BD59) {
        std::ostringstream error;
        error << "plains grass: expected 0x91bd59, got 0x" << std::hex << grass;
        throw std::runtime_error(error.str());
    }
    std::cout << "plains grass 0x91BD59\n";
}

void checkJarWins() {
    Json overrideJson = Json::object();
    overrideJson["temperature"] = 0.1;
    overrideJson["downfall"] = 0.2;
    overrideJson["effects"] = Json::object({{"water_color", 1}, {"fog_color", 2}});
    {
        std::unique_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        GlobalCache::biomes["jar:minecraft:swamp"] = overrideJson;
        GlobalCache::biomeIndex["biomes:minecraft:swamp"] = "jar:minecraft:swamp";
    }
    const Json indexed = Biome::GetBiomeJson("minecraft", "swamp");
    require(indexed == overrideJson, "indexed jar JSON must win over the built-in table");

    {
        std::unique_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        GlobalCache::jarOrder.push_back("latejar");
        GlobalCache::biomes["latejar:minecraft:river"] = overrideJson;
    }
    const Json linear = Biome::GetBiomeJson("minecraft", "river");
    require(linear == overrideJson, "linear jar scan must win over the built-in table");

    std::ostringstream captured;
    auto* previous = std::cerr.rdbuf(captured.rdbuf());
    const Json foreign = Biome::GetBiomeJson("example", "swamp");
    const Json unknown = Biome::GetBiomeJson("minecraft", "not_a_biome");
    std::cerr.rdbuf(previous);
    require(foreign.is_null(), "non-minecraft namespace must not use the vanilla table");
    require(unknown.is_null(), "unknown minecraft biome stays null");
    const std::string warnings = captured.str();
    require(warnings.find("Biome JSON not found: example:swamp") != std::string::npos,
            "foreign namespace still reports not found");
    require(warnings.find("Biome JSON not found: minecraft:not_a_biome") != std::string::npos,
            "unknown minecraft biome still reports not found");
    require(warnings.find("Biome JSON not found: minecraft:swamp") == std::string::npos,
            "a cache hit must not warn");
}
} // namespace

int main() {
    try {
        checkSamples();
        checkPlainsColors();
        checkJarWins();
        std::cout << "PASS: legacy biomes (" << LegacyVanillaBiomeCount() << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
