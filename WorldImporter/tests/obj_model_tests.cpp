// Regression: forge:obj / porting_lib:obj block models (Create valve handles, tracks)
// must mesh through the normal blockstate path, including loader/model inherited
// from a parent model (red_valve_handle -> valve_handle).
#include "GlobalCache.h"
#include "SpecialBlock.h"
#include "model.h"
#include <iostream>
#include <stdexcept>

Config config;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addModel(const std::string& id, const nlohmann::json& json) {
    GlobalCache::models["testmod:" + id] = json;
    GlobalCache::modelIndex["models:" + id] = "testmod:" + id;
}

void addAsset(const std::string& id, const std::string& text) {
    GlobalCache::modelAssets["testmod:" + id] = text;
    GlobalCache::modelAssetIndex["modelassets:" + id] = "testmod:" + id;
}
}

int main() {
    try {
        addAsset("create:block/valve_handle/valve_handle.obj",
            "mtllib valve_handle.mtl\n"
            "v 0 0 0\nv 1 0 0\nv 1 0.5 1\nv 0 0.5 1\n"
            "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
            "usemtl Material\nf 1/1 2/2 3/3 4/4\n");
        addAsset("create:block/valve_handle/valve_handle.mtl", "newmtl Material\nmap_Kd #3\n");
        addModel("create:block/valve_handle", {
            {"loader", "forge:obj"}, {"flip_v", true},
            {"model", "create:models/block/valve_handle/valve_handle.obj"},
            {"textures", {{"3", "create:block/valve_handle/valve_handle_copper"}}}});
        addModel("create:block/red_valve_handle", {
            {"parent", "create:block/valve_handle"},
            {"textures", {{"3", "create:block/valve_handle/valve_handle_red"}}}});
        addModel("create:block/fabric_handle", {
            {"porting_lib:loader", "porting_lib:obj"},
            {"model", "create:models/block/valve_handle/valve_handle.obj"},
            {"textures", {{"3", "create:block/valve_handle/valve_handle_copper"}}}});

        require(SpecialBlock::IsObjModelJson(GlobalCache::models["testmod:create:block/valve_handle"]),
            "forge:obj detected");
        require(!SpecialBlock::IsObjModelJson(nlohmann::json{{"loader", "forge:composite"}}),
            "composite is not obj");

        ModelData red = ProcessModelJson("create", "block/red_valve_handle", 0, 0, false, 0, "");
        require(red.faces.size() == 2, "inherited forge:obj meshes (quad -> 2 tris), got "
            + std::to_string(red.faces.size()));
        require(!red.materials.empty() &&
            red.materials[0].name == "create:block/valve_handle/valve_handle_red",
            "child texture override wins: " + (red.materials.empty() ? std::string("<none>") : red.materials[0].name));

        ModelData rotated = ProcessModelJson("create", "block/red_valve_handle", 90, 0, false, 0, "");
        require(rotated.vertices.size() == red.vertices.size(), "rotated copy keeps geometry");

        ModelData fabric = ProcessModelJson("create", "block/fabric_handle", 0, 0, false, 0, "");
        require(fabric.faces.size() == 2, "porting_lib:obj meshes");
    }
    catch (const std::exception& error) {
        std::cerr << "obj_model_tests FAILED: " << error.what() << std::endl;
        return 1;
    }
    std::cout << "obj_model_tests passed" << std::endl;
    return 0;
}
