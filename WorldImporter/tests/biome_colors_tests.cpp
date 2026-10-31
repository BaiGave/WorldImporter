#include "biome.h"
#include "GlobalCache.h"
#include "include/stb_image.h"
#include "block.h"
#include "objExporter.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

// The application normally supplies this in main.cpp.
Config config;

namespace {
using Json = nlohmann::json;

struct NetherColor {
    const char* name;
    int rgb;
    const char* hex;
};

// Expected values shared by the official 1.21.1 JAR and 1.21.11 fixture.
constexpr std::array<NetherColor, 5> netherColors{{
    {"nether_wastes", 0x330808, "#330808"},
    {"soul_sand_valley", 0x1B4745, "#1b4745"},
    {"crimson_forest", 0x330303, "#330303"},
    {"warped_forest", 0x1A051A, "#1a051a"},
    {"basalt_deltas", 0x685F70, "#685f70"},
}};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void expectColor(int id, BiomeColorType type, int expected, const std::string& label) {
    const int actual = Biome::GetColor(id, type);
    if (actual != expected) {
        std::ostringstream error;
        error << label << ": expected 0x" << std::hex << expected << ", got 0x" << actual;
        throw std::runtime_error(error.str());
    }
}

int registerJson(Json json) {
    static int counter = 0;
    const std::string name = "test:biome_colors_" + std::to_string(counter++);
    const std::string cacheKey = "test:" + name;
    {
        std::unique_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        GlobalCache::biomes[cacheKey] = std::move(json);
        GlobalCache::biomeIndex["biomes:" + name] = cacheKey;
    }
    return Biome::GetId(name);
}

// Supply plant colors so synthetic cases need no external colormap textures.
Json biome() {
    return {{"effects", {{"foliage_color", 0x7CBD6B}, {"grass_color", 0x7CBD6B},
                         {"dry_foliage_color", 0x7CBD6B}, {"water_color", "#3f76e4"}}}};
}

void syntheticTests() {
    // Register a modern biome first, also exercising its RGB pixel below (ID 0).
    for (const auto& entry : netherColors) {
        auto modern = biome();
        modern["attributes"]["minecraft:visual/fog_color"] = entry.hex;
        const int id = registerJson(modern);
        expectColor(id, BiomeColorType::Fog, entry.rgb, std::string("modern ") + entry.name);
        expectColor(id, BiomeColorType::Water, 0x3F76E4, "modern effects water hex");

        auto legacy = biome();
        legacy["effects"]["fog_color"] = entry.rgb;
        expectColor(registerJson(legacy), BiomeColorType::Fog, entry.rgb,
                    std::string("legacy ") + entry.name);
    }

    struct Visual {
        const char* key;
        BiomeColorType type;
        int effectsDefault;
        int noEffectsDefault;
    };
    for (const auto& visual : std::array<Visual, 3>{{
             {"fog_color", BiomeColorType::Fog, 0xC0D8FF, 0xC0D8FF},
             {"sky_color", BiomeColorType::Sky, 0x84ECFF, 0x78A7FF},
             {"water_fog_color", BiomeColorType::WaterFog, 0x050533, 0x3F76E4}}}) {
        const std::string attr = std::string("minecraft:visual/") + visual.key;
        for (const Json& value : {Json(0x1B4745), Json("1787717"), Json("0x1B4745"),
                                 Json("#1b4745"), Json("#1B4745"), Json(0)}) {
            const int expected = value == Json(0) ? 0 : 0x1B4745;
            auto json = biome();
            json["effects"][visual.key] = value;
            json["attributes"][attr] = "#abcdef";
            expectColor(registerJson(json), visual.type, expected, "legacy format and priority " + attr);
            json["effects"].erase(visual.key);
            json["attributes"][attr] = value;
            expectColor(registerJson(json), visual.type, expected, "attribute format " + attr);
        }

        for (const Json& invalid : {Json(), Json("invalid"), Json("#12345"), Json("#gg0000"),
                                   Json("#12345678"), Json(true), Json::array(), Json::object()}) {
            auto json = biome();
            json["effects"][visual.key] = invalid;
            json["attributes"][attr] = "#abcdef";
            expectColor(registerJson(json), visual.type, 0xABCDEF, "invalid legacy falls through " + attr);
            json["attributes"][attr] = invalid;
            expectColor(registerJson(json), visual.type, visual.effectsDefault, "invalid both " + attr);
        }

        for (const Json& effects : {Json(), Json::object(), Json::array(), Json(7)}) {
            auto json = Json{{"effects", effects}, {"attributes", {{attr, "#abcdef"}}}};
            expectColor(registerJson(json), visual.type, 0xABCDEF, "empty/malformed effects " + attr);
        }
        expectColor(registerJson({{"attributes", {{attr, "#abcdef"}}}}), visual.type,
                    0xABCDEF, "attributes without effects " + attr);
        expectColor(registerJson(biome()), visual.type, visual.effectsDefault, "missing key " + attr);
        expectColor(registerJson(Json::object()), visual.type, visual.noEffectsDefault, "missing effects " + attr);
        expectColor(registerJson(Json()), visual.type, visual.noEffectsDefault, "missing JSON " + attr);

        for (const Json& attributes : {Json(), Json::array(), Json(42)}) {
            auto json = biome();
            json["attributes"] = attributes;
            expectColor(registerJson(json), visual.type, visual.effectsDefault, "malformed attributes " + attr);
        }

        // Only override is independent of dimension inheritance. Never use a
        // multiply/add argument as if it were the final color.
        for (const auto* modifier : {"override", "multiply", "add"}) {
            auto json = biome();
            json["attributes"][attr] = {{"modifier", modifier}, {"argument", "#abcdef"}};
            expectColor(registerJson(json), visual.type,
                        std::string(modifier) == "override" ? 0xABCDEF : visual.effectsDefault,
                        "modifier " + std::string(modifier) + " " + attr);
        }
        auto json = biome();
        json["attributes"][attr] = {{"argument", "#abcdef"}};
        expectColor(registerJson(json), visual.type, 0xABCDEF, "implicit override " + attr);
    }
    // The shared parser also covers modern hex effects colors for plants.
    auto plants = biome();
    plants["effects"]["grass_color"] = "#123ABC";
    plants["effects"]["foliage_color"] = "#456def";
    plants["effects"]["dry_foliage_color"] = "#789abc";
    const int id = registerJson(plants);
    expectColor(id, BiomeColorType::Grass, 0x123ABC, "grass hex");
    expectColor(id, BiomeColorType::Foliage, 0x456DEF, "foliage hex");
    expectColor(id, BiomeColorType::DryFoliage, 0x789ABC, "dry foliage hex");

    // RGB float arrays are legal in both effects and visual attributes (1.21.11).
    for (const auto* key : {"grass_color", "foliage_color", "dry_foliage_color", "water_color"}) {
        plants["effects"][key] = Json::array({1.0, 0.0, 0.5});
    }
    plants["attributes"]["minecraft:visual/sky_color"] = Json::array({0.0, 1.0, 0.0});
    plants["attributes"]["minecraft:visual/fog_color"] = {
        {"modifier", "override"}, {"argument", Json::array({1.0, 0.0, 0.5})}};
    const int arrayId = registerJson(plants);
    for (auto type : {BiomeColorType::Grass, BiomeColorType::Foliage,
                      BiomeColorType::DryFoliage, BiomeColorType::Water, BiomeColorType::Fog}) {
        expectColor(arrayId, type, 0xFF007F, "RGB float array");
    }
    expectColor(arrayId, BiomeColorType::Sky, 0x00FF00, "sky RGB array");
    for (const Json& invalid : {Json::array({1.0, 0.0}), Json::array({1.0, 0.0, 0.0, 1.0}),
                               Json::array({-0.1, 0.0, 0.0}), Json::array({1.1, 0.0, 0.0}),
                               Json::array({true, 0, 0}), Json::array({"1", 0, 0})}) {
        auto json = biome();
        json["effects"]["water_color"] = invalid;
        expectColor(registerJson(json), BiomeColorType::Water, 0x3F76E4, "invalid RGB array");
    }

    require(sectionCache.empty(), "synthetic blending test needs an empty section cache");
    require(Biome::GetBiomeColor(0, 63, 0, BiomeColorType::Grass) == 0xFFFFFF,
            "missing blend samples must return neutral white");
}

void mapAndTintTests(const char* executable) {
    const auto dir = std::filesystem::absolute(executable).parent_path();
    // Reinitializing an existing map must also reset widths of retained rows.
    Biome::InitializeBiomeMap(0, 0, 3, 2);
    Biome::InitializeBiomeMap(-1, -1, -1, 1);
    require(Biome::ExportToPNG("biome-colors-resize.png", BiomeColorType::Fog), "resized map export");
    int w = 0, h = 0, channels = 0;
    auto* pixels = stbi_load((dir / "biomeTex/biome-colors-resize.png").string().c_str(), &w, &h, &channels, 3);
    require(pixels != nullptr, "resized map PNG readback");
    stbi_image_free(pixels);
    require(w == 1 && h == 3, "reinitialized map must be 1x3");

    TintResult grass, fixed;
    grass.kind = TintKind::Grass;
    fixed.kind = TintKind::Fixed;
    fixed.color = 0x123456;
    RegisterOverlaySequence("biome-colors-test", {{"grass", "grass.png", grass}, {"fixed", "fixed.png", fixed}});
    for (bool enabled : {false, true}) {
        config.useBiomeColors = enabled;
        CreateTintJsonFile({{"grass", grass}, {"fixed", fixed}});
        CreateOverlayJsonFile();
        Json tint, overlays;
        std::ifstream(dir / "tint.json") >> tint;
        std::ifstream(dir / "overlay.json") >> overlays;
        require(tint["grass"].contains("kind") == enabled, "biome tint metadata must honor switch");
        const auto& layers = overlays.at("biome-colors-test");
        require(layers[0].contains("kind") == enabled, "overlay biome tint must honor switch");
        require(tint["fixed"]["kind"] == "fixed" && layers[1]["kind"] == "fixed", "fixed tint must stay enabled");
        require(tint["fixed"]["color"][0].get<float>() < 0.01f, "fixed tint must remain linear");
    }
}

void archiveTests(const char* path, bool modern) {
    config.jarPath = path;
    config.modsPath = "None";
    InitializeAllCaches();
    for (const auto& entry : netherColors) {
        const auto json = Biome::GetBiomeJson("minecraft", entry.name);
        require(json.is_object(), std::string("biome not found in archive: ") + entry.name);
        if (modern) {
            require(!json.at("effects").contains("fog_color"), "fixture must use modern schema");
            require(json.at("attributes").contains("minecraft:visual/fog_color"), "missing modern fog attribute");
        } else {
            require(json.at("effects").at("fog_color") == entry.rgb, "legacy archive fog mismatch");
        }
        const int id = Biome::GetId(std::string("minecraft:") + entry.name);
        expectColor(id, BiomeColorType::Fog, entry.rgb, entry.name);
        expectColor(id, BiomeColorType::Water, 0x3F76E4, std::string("water ") + entry.name);
        std::cout << entry.name << " fog=0x" << std::hex << entry.rgb << std::dec << '\n';
    }
}

void checkExportedPixel(const char* executable, const std::string& name) {
    // InitializeBiomeMap zero-initializes the map: ID 0 is nether_wastes in
    // both synthetic and archive modes. Exercise the real PNG exporter/decoder.
    Biome::InitializeBiomeMap(0, 0, 0, 0);
    require(Biome::ExportToPNG(name, BiomeColorType::Fog), "fog PNG export failed");
    const auto path = std::filesystem::absolute(executable).parent_path() / "biomeTex" / name;
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 3);
    require(pixels != nullptr, "cannot read exported fog PNG: " + path.string());
    const bool correct = width == 1 && height == 1 && pixels[0] == 0x33 && pixels[1] == 0x08 && pixels[2] == 0x08;
    stbi_image_free(pixels);
    require(correct, "exported fog pixel must be #330808");
}
} // namespace

int main(int argc, char** argv) {
    try {
        std::string mode = "synthetic";
        if (argc == 1) {
            syntheticTests();
            mapAndTintTests(argv[0]);
        } else {
            require(argc == 3, "usage: biome_colors_tests [--LEGACY|--ATTRIBUTES archive]");
            mode = argv[1];
            require(mode == "--LEGACY" || mode == "--ATTRIBUTES", "unknown archive schema");
            archiveTests(argv[2], mode == "--ATTRIBUTES");
        }
        checkExportedPixel(argv[0], "biome-colors-" + mode + "-fog.png");
        std::cout << "PASS: " << mode << " biome colors and fog PNG pixel\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
