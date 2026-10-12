#include "fluid.h"
#include "model.h"
#include <cmath>
#include <iostream>

Config config;
static int failures = 0;
static void check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}
static bool near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
static float normalY(const ModelData& m, const Face& face) {
    auto v = [&](int i, int axis) { return m.vertices[face.vertexIndices[i] * 3 + axis]; };
    const float ax = v(1, 0) - v(0, 0), az = v(1, 2) - v(0, 2);
    const float bx = v(2, 0) - v(0, 0), bz = v(2, 2) - v(0, 2);
    return az * bx - ax * bz; // Y component of (B-A) cross (C-A).
}
int main() {
    check(near(GetFluidOwnHeight(0), 8.0f / 9.0f), "source height is 8/9");
    for (int level = 8; level <= 15; ++level)
        check(near(GetFluidOwnHeight(level), 8.0f / 9.0f), "falling fluid uses source height");
    const float source = 8.0f / 9.0f;
    check(near(CalculateFluidCornerHeight(source, -1, -1, -1), source), "solid neighbors do not raise water");
    check(near(CalculateFluidCornerHeight(source, 0, source, 0), CalculateFluidCornerHeight(source, 0, 0, source)), "corner axis symmetry");
    check(near(CalculateFluidCornerHeight(source, 0, 4.0f / 9.0f, 0), CalculateFluidCornerHeight(4.0f / 9.0f, 0, source, 0)), "shared corner is independent of current block");
    // Source contributes weight 10; the two air sides each contribute 1.
    const float isolatedCorner = source * 10.0f / 12.0f;
    check(near(CalculateFluidCornerHeight(source, 1, 0, 0), isolatedCorner), "disconnected diagonal is excluded; air sides lower corner");
    check(near(CalculateFluidCornerHeight(source, 0, 1, 0), 1), "adjacent fluid column fills corner");

    FluidModelParams params;
    params.selfHeight = source;
    auto water = GenerateFluidModel(params, "minecraft:water");
    check(near(water.vertices[13], isolatedCorner), "isolated water uses weighted corner height");
    check(water.faces.size() == 6, "sub-height water top survives ceiling culling");
    check(near(water.uvCoordinates[16], 0) && near(water.uvCoordinates[22], 0.5f), "side U uses flow sprite left half");
    check(near(water.uvCoordinates[17], 0.5f), "side bottom V uses flow sprite midpoint");
    check(near(water.uvCoordinates[19], 1.0f - (1.0f - isolatedCorner) * 0.5f), "side top V follows water height");
    check(normalY(water, water.faces[0]) < 0, "bottom face normal points down");
    check(normalY(water, water.faces[1]) > 0, "top face normal points up");
    params.topCanRender = true;
    auto inset = GenerateFluidModel(params, "minecraft:water");
    check(near(inset.vertices[13], isolatedCorner - 0.001f), "rendered top has 0.001 inset");
    params.topCanRender = false;

    using json = nlohmann::json;
    for (const auto& suffix : {std::string("_still"), std::string("_flow")}) {
        const std::string path = "block/testfluid" + suffix;
        const std::string key = "testjar:test:" + path;
        GlobalCache::mcmetaIndex["mcmetas:test:" + path] = key;
        GlobalCache::mcmetaCache[key] = {{"animation", json::object()}};
        textureDimensionCache[key] = TextureDimension(16, suffix == "_still" ? 512 : 1024);
    }
    params.selfHeight = 5.0f / 9.0f;
    params.flowX = 1.0f;
    auto animated = GenerateFluidModel(params, "test:testfluid");
    check(near(animated.materials[0].aspectRatio, 32) && near(animated.materials[1].aspectRatio, 64), "mod animation frame ratios");
    check(near(animated.uvCoordinates[17], 1.0f - 0.5f / 64.0f), "animated side bottom stays in first frame");
    check(near(animated.uvCoordinates[19], 1.0f - (1.0f - animated.vertices[31]) * 0.5f / 64.0f), "animated side top respects fluid height");
    bool uvValid = true;
    for (const auto& face : animated.faces) {
        const float ratio = animated.materials[face.materialIndex].aspectRatio;
        for (int index : face.uvIndices) {
            const float u = animated.uvCoordinates[index * 2];
            const float v = animated.uvCoordinates[index * 2 + 1];
            uvValid &= std::isfinite(u) && std::isfinite(v) && u >= 0 && u <= 1 && v >= 1 - 1 / ratio && v <= 1;
        }
    }
    check(uvValid, "flow top and sides remain inside their assigned animation frame");
    json parent = {{"elements", json::array({{{"from", {0,0,0}}, {"to", {16,16,16}}}})},
                   {"textures", {{"all", "test:block/parent"}, {"particle", "#all"}}}};
    json slab = {{"elements", json::array({{{"from", {0,0,0}}, {"to", {16,8,16}}}})},
                 {"textures", {{"all", "test:block/slab"}}}};
    auto merged = MergeModelJson(parent, slab);
    check(merged["elements"] == slab["elements"], "mod slab replaces parent cube geometry");
    FluidModelParams custom;
    custom.selfHeight = source;
    custom.keepFace[1] = true;
    fluidDefinitions["create:chocolate"] = FluidInfo{"fluid", "_still", "_flow", "", "level", {}};
    const std::string stillKey = "testjar:create:fluid/chocolate_still";
    const std::string flowKey = "testjar:create:fluid/chocolate_flow";
    GlobalCache::mcmetaIndex["mcmetas:create:fluid/chocolate_still"] = stillKey;
    GlobalCache::mcmetaIndex["mcmetas:create:fluid/chocolate_flow"] = flowKey;
    GlobalCache::mcmetaCache[stillKey] = {{"animation", nlohmann::json::object()}};
    GlobalCache::mcmetaCache[flowKey] = {{"animation", nlohmann::json::object()}};
    textureDimensionCache[stillKey] = TextureDimension(16, 512);
    textureDimensionCache[flowKey] = TextureDimension(16, 1024);
    auto chocolate = GenerateFluidModel(custom, "create:chocolate");
    const auto originalUV = chocolate.uvCoordinates;
    AssignFluidMaterials(chocolate, "create:chocolate");
    check(chocolate.uvCoordinates == originalUV, "registered fluid material assignment preserves correct UVs");
    bool customUvValid = true;
    for (const auto& face : chocolate.faces) {
        const float ratio = chocolate.materials[face.materialIndex].aspectRatio;
        for (int index : face.uvIndices) {
            const float v = chocolate.uvCoordinates[index * 2 + 1];
            customUvValid &= v >= 1.0f - 1.0f / ratio && v <= 1.0f;
        }
    }
    check(near(chocolate.materials[1].aspectRatio, 64) && customUvValid, "mod fluid UV stays inside replaced texture frame");
    check(merged["textures"]["all"] == "test:block/slab", "child texture override retained");
    check(MergeModelJson(parent, json::object())["elements"] == parent["elements"], "missing child elements inherit parent");
    check(MergeModelJson(parent, {{"elements", json::array()}})["elements"].empty(), "explicit empty elements suppress parent");
    std::cout << "Failures: " << failures << '\n';
    return failures ? 1 : 0;
}
