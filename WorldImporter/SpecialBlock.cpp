#include "SpecialBlock.h"
#include "CopperGolemStatue.h"
#include <vector>
#include <algorithm>
#include <sstream>
#include "EndPortalModel.h"
#include <span>
#include "texture.h"
#include "blockstate.h"
#include "SkullModel.h"
#include <unordered_map>
#include <unordered_set>
#include <cmath>

using namespace std;
float centerX = 0.5f;
float centerY = 0.5f;
float centerZ = 0.5f;

namespace {
    unordered_map<string, string> ParseBlockStates(const string& blockName) {
        unordered_map<string, string> states;
        size_t begin = blockName.find('[');
        size_t end = blockName.rfind(']');
        if (begin == string::npos || end == string::npos || begin >= end) return states;

        stringstream stream(blockName.substr(begin + 1, end - begin - 1));
        string pair;
        while (getline(stream, pair, ',')) {
            size_t separator = pair.find('=');
            if (separator == string::npos) separator = pair.find(':');
            if (separator != string::npos) {
                states[pair.substr(0, separator)] = pair.substr(separator + 1);
            }
        }
        return states;
    }

    string BaseBlockName(const string& blockName) {
        size_t end = blockName.find('[');
        return blockName.substr(0, end);
    }

    void DisableCulling(ModelData& model) {
        for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
    }

    ModelData LoadCreateModel(const string& path, int index) {
        ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
        DisableCulling(model);
        return model;
    }

    string GetModelAsset(const string& ns, const string& path) {
        shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
        auto index = GlobalCache::modelAssetIndex.find("modelassets:" + ns + ":" + path);
        if (index == GlobalCache::modelAssetIndex.end()) return "";
        auto asset = GlobalCache::modelAssets.find(index->second);
        return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
    }

    string ResolveTexture(const nlohmann::json& modelJson, string value) {
        unordered_set<string> visited;
        while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
            string key = value.substr(1);
            if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
            value = modelJson["textures"][key].get<string>();
        }
        return value;
    }

    Material CreateMaterial(const string& texture) {
        string ns = "minecraft";
        string path = texture;
        size_t colon = texture.find(':');
        if (colon != string::npos) {
            ns = texture.substr(0, colon);
            path = texture.substr(colon + 1);
        }
        string root = "textures";
        string savedPath = "textures/" + ns + "/" + path + ".png";
        if (SaveTextureToFile(ns, path, root)) RegisterTexture(ns, path, savedPath);
        Material material(texture, savedPath, -1);
        material.type = DetectMaterialType(ns, path, material.aspectRatio);
        return material;
    }

    ModelData LoadObjFromModelJson(const nlohmann::json& modelJson);

    ModelData LoadCreateObjModel(const string& jsonPath, int index) {
        (void)index;
        nlohmann::json modelJson = GetModelJson("create", jsonPath);
        if (modelJson.is_null()) return {};
        modelJson = LoadParentModel("create", jsonPath, modelJson);
        return LoadObjFromModelJson(modelJson);
    }

    // forge:obj / neoforge:obj / porting_lib:obj: "model" points at an OBJ asset
    // ("<ns>:models/<path>.obj"); MTL map_Kd values are "#key" texture references.
    ModelData LoadObjFromModelJson(const nlohmann::json& modelJson) {
        if (!modelJson.contains("model") || !modelJson["model"].is_string()) return {};
        string objPath = modelJson["model"].get<string>();
        string objNs = "minecraft";
        size_t colon = objPath.find(':');
        if (colon != string::npos) {
            objNs = objPath.substr(0, colon);
            objPath = objPath.substr(colon + 1);
        }
        if (objPath.starts_with("models/")) objPath = objPath.substr(7);
        string obj = GetModelAsset(objNs, objPath);
        if (obj.empty()) return {};

        vector<array<float, 3>> positions;
        vector<array<float, 2>> uvs;
        unordered_map<string, string> mtlTextures;
        stringstream objStream(obj);
        string line;
        while (getline(objStream, line)) {
            if (!line.starts_with("mtllib ")) continue;
            string mtlName = line.substr(7);
            string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
            stringstream mtlStream(GetModelAsset(objNs, directory + mtlName));
            string materialName;
            while (getline(mtlStream, line)) {
                if (line.starts_with("newmtl ")) materialName = line.substr(7);
                else if (line.starts_with("map_Kd ") && !materialName.empty()) {
                    mtlTextures[materialName] = ResolveTexture(modelJson, line.substr(7));
                }
            }
        }

        ModelData result;
        unordered_map<string, int> materialIndices;
        string currentMaterial;
        objStream.clear();
        objStream.seekg(0);
        // Create JSON sets flip_v for Minecraft (V=0 at top). WI exports for Blender/
        // OpenGL (V=0 at bottom), matching the JSON model path's `1 - uv/16` convention.
        // Keep Blender-native OBJ V so cage bars / flywheel rims hit the right atlas cells.
        // (Applying flip_v here mapped blaze_cage bars onto the transparent half of
        // blaze_heater_brazier — casing looked like a flat base only.)
        while (getline(objStream, line)) {
            stringstream values(line);
            string type;
            values >> type;
            if (type == "v") {
                array<float, 3> value{};
                values >> value[0] >> value[1] >> value[2];
                positions.push_back(value);
            }
            else if (type == "vt") {
                array<float, 2> value{};
                values >> value[0] >> value[1];
                uvs.push_back(value);
            }
            else if (type == "usemtl") values >> currentMaterial;
            else if (type == "f") {
                // Forge's OBJ loader skips faces with no material (e.g. Blender "Bounding" helpers).
                if (currentMaterial.empty()) continue;
                vector<pair<int, int>> corners;
                string corner;
                while (values >> corner) {
                    size_t slash = corner.find('/');
                    int vertex = stoi(corner.substr(0, slash)) - 1;
                    size_t nextSlash = slash == string::npos ? string::npos : corner.find('/', slash + 1);
                    int uv = slash == string::npos ? -1 : stoi(corner.substr(slash + 1, nextSlash - slash - 1)) - 1;
                    corners.emplace_back(vertex, uv);
                }
                if (corners.size() < 3) continue;
                int materialIndex;
                auto materialIt = materialIndices.find(currentMaterial);
                if (materialIt == materialIndices.end()) {
                    string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                    materialIndex = static_cast<int>(result.materials.size());
                    result.materials.push_back(CreateMaterial(texture));
                    materialIndices[currentMaterial] = materialIndex;
                }
                else materialIndex = materialIt->second;

                for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                    array<pair<int, int>, 4> quad = { corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1] };
                    Face face{};
                    face.materialIndex = materialIndex;
                    face.faceDirection = FaceType::DO_NOT_CULL;
                    for (size_t i = 0; i < quad.size(); ++i) {
                        const auto& position = positions[quad[i].first];
                        face.vertexIndices[i] = static_cast<int>(result.vertices.size() / 3);
                        result.vertices.insert(result.vertices.end(), position.begin(), position.end());
                        face.uvIndices[i] = static_cast<int>(result.uvCoordinates.size() / 2);
                        if (quad[i].second >= 0 && static_cast<size_t>(quad[i].second) < uvs.size()) {
                            result.uvCoordinates.push_back(uvs[quad[i].second][0]);
                            result.uvCoordinates.push_back(uvs[quad[i].second][1]);
                        }
                        else {
                            result.uvCoordinates.push_back(0.0f);
                            result.uvCoordinates.push_back(0.0f);
                        }
                    }
                    result.faces.push_back(face);
                }
            }
        }
        return result;
    }

    void Rotate(ModelData& model, float x, float y, float z) {
        if (model.vertices.empty()) return;
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), x, y, z);
        DisableCulling(model);
    }

    void Append(ModelData& target, ModelData part) {
        if (part.vertices.empty()) return;
        if (target.vertices.empty()) target = std::move(part);
        else MergeModelsDirectly(target, part);
    }

    string NbtString(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
    }

    bool NbtBool(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::BYTE && bytesToByte(tag->payload) != 0;
    }

    void ReplaceTexture(ModelData& model, const string& oldPath, const string& newPath) {
        string saveRoot = "textures";
        if (!SaveTextureToFile("create", newPath, saveRoot)) return;
        string savedPath = "textures/create/" + newPath + ".png";
        RegisterTexture("create", newPath, savedPath);
        for (auto& material : model.materials) {
            if (material.name == "create:" + oldPath) {
                material.name = "create:" + newPath;
                material.texturePath = savedPath;
                material.type = DetectMaterialType("create", newPath, material.aspectRatio);
            }
        }
    }

    float FacingY(const string& facing) {
        if (facing == "west") return 90.0f;
        if (facing == "north") return 180.0f;
        if (facing == "east") return 270.0f;
        return 0.0f;
    }

    void RotateKineticAxis(ModelData& model, const string& axis) {
        if (axis == "x") Rotate(model, 90.0f, 90.0f, 0.0f);
        else if (axis == "z") Rotate(model, 90.0f, 180.0f, 0.0f);
    }

    void RotateFromSouth(ModelData& model, const string& direction) {
        if (direction == "north") Rotate(model, 0.0f, 180.0f, 0.0f);
        else if (direction == "east") Rotate(model, 0.0f, 90.0f, 0.0f);
        else if (direction == "west") Rotate(model, 0.0f, 270.0f, 0.0f);
        else if (direction == "up") Rotate(model, 270.0f, 0.0f, 0.0f);
        else if (direction == "down") Rotate(model, 90.0f, 0.0f, 0.0f);
    }

    ModelData LoadBlockstateModel(const string& blockName) {
        size_t colon = blockName.find(':');
        string localName = colon == string::npos ? blockName : blockName.substr(colon + 1);
        return GetRandomModelFromCache("create", localName);
    }

    // WaterWheelRenderer swaps the oak template sprites for the BE "Material" planks
    // (default spruce) and the matching log (x_log / x_stem / x_block).
    void ApplyWaterWheelMaterial(ModelData& model, const NbtTagPtr& nbt) {
        string planks = "minecraft:spruce_planks";
        NbtTagPtr material = nbt && nbt->type == TagType::COMPOUND ? getChildByName(nbt, "Material") : nullptr;
        if (material && material->type == TagType::COMPOUND) {
            string name = NbtString(material, "Name");
            if (!name.empty()) planks = name;
        }
        size_t colon = planks.find(':');
        string ns = colon == string::npos ? "minecraft" : planks.substr(0, colon);
        string path = colon == string::npos ? planks : planks.substr(colon + 1);
        string wood;
        if (path.size() > 7 && path.ends_with("_planks"))
            wood = (path.starts_with("archwood") ? "blue_" : "") + path.substr(0, path.size() - 7);
        else if (path.find("wood/planks/") != string::npos) wood = path.substr(path.find("wood/planks/") + 12);
        if (wood.empty() || (ns == "minecraft" && wood == "oak")) return;

        auto swap = [&](const string& from, const string& texNs, const string& texPath) {
            string root = "textures";
            if (!SaveTextureToFile(texNs, texPath, root)) return false;
            string saved = "textures/" + texNs + "/" + texPath + ".png";
            RegisterTexture(texNs, texPath, saved);
            for (auto& m : model.materials) {
                if (m.texturePath != "textures/minecraft/" + from + ".png") continue;
                m.name = texNs + ":" + texPath;
                m.texturePath = saved;
                m.type = DetectMaterialType(texNs, texPath, m.aspectRatio);
            }
            return true;
        };
        swap("block/oak_planks", ns, "block/" + path);
        for (const string& log : { wood + "_log", wood + "_stem", wood + "_block", "wood/log/" + wood }) {
            if (swap("block/oak_log", ns, "block/" + log)) {
                swap("block/oak_log_top", ns, "block/" + log + "_top");
                break;
            }
        }
    }

    ModelData GenerateCreateWaterWheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z, const NbtTagPtr& nbt) {
        ModelData result;
        string axis = "y";
        if (id == "water_wheel") {
            result = LoadBlockstateModel(blockName);
            string facing = states.contains("facing") ? states.at("facing") : "up";
            axis = facing == "east" || facing == "west" ? "x" :
                facing == "north" || facing == "south" ? "z" : "y";
            ModelData wheel = LoadCreateObjModel("block/water_wheel/wheel", 120);
            ApplyWaterWheelMaterial(wheel, nbt);
            RotateKineticAxis(wheel, axis);
            Append(result, std::move(wheel));
        }
        else {
            axis = states.contains("axis") ? states.at("axis") : "y";
            bool extension = states.contains("extension") && states.at("extension") == "true";
            result = LoadCreateObjModel(extension ? "block/large_water_wheel/block_extension" :
                "block/large_water_wheel/block", 121);
            ApplyWaterWheelMaterial(result, nbt);
            RotateKineticAxis(result, axis);
        }
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : 0.0f;
        if (axis == "x") Rotate(result, phase, 0.0f, 0.0f);
        else if (axis == "z") Rotate(result, 0.0f, 0.0f, phase);
        else Rotate(result, 0.0f, phase, 0.0f);
        return result;
    }

    ModelData GenerateCreateShaftedBlock(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        ModelData shaft = LoadCreateModel("block/shaft", 132);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        return result;
    }

    void RotateAroundAxis(ModelData& model, const string& axis, float angle) {
        if (axis == "x") Rotate(model, angle, 0.0f, 0.0f);
        else if (axis == "z") Rotate(model, 0.0f, 0.0f, angle);
        else Rotate(model, 0.0f, angle, 0.0f);
    }

    ModelData GenerateCreateKinetic(const string& id,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData model = LoadCreateModel("block/" + id, 100);
        string axis = "y";
        auto axisIt = states.find("axis");
        if (axisIt != states.end()) axis = axisIt->second;
        RotateKineticAxis(model, axis);

        // Create offsets adjacent gears so their teeth mesh in a static export.
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : (id == "large_cogwheel" ? 11.25f : 0.0f);
        if (id != "shaft") RotateAroundAxis(model, axis, phase);
        return model;
    }

    ModelData GenerateCreateEncasedCogwheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        bool isLarge = id.find("large_cogwheel") != string::npos;
        string cogwheelId = isLarge ? "large_cogwheel" : "cogwheel";
        ModelData cogwheel = GenerateCreateKinetic(cogwheelId, states, x, y, z);
        Append(result, std::move(cogwheel));
        return result;
    }

    // Right-handed rotation about the block centre, same as Minecraft's
    // PoseStack rotateX/Y/ZDegrees (Axis.XP/YP/ZP) used by Create renderers.
    void McRotate(ModelData& model, char axis, float degrees) {
        if (model.vertices.empty() || fmodf(degrees, 360.0f) == 0.0f) return;
        const float rad = degrees * (float)M_PI / 180.0f;
        const float c = roundf(cosf(rad) * 1e6f) / 1e6f, s = roundf(sinf(rad) * 1e6f) / 1e6f;
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            float x = model.vertices[i] - 0.5f, y = model.vertices[i + 1] - 0.5f, z = model.vertices[i + 2] - 0.5f;
            float nx = x, ny = y, nz = z;
            if (axis == 'x') { ny = y * c - z * s; nz = y * s + z * c; }
            else if (axis == 'y') { nx = x * c + z * s; nz = -x * s + z * c; }
            else { nx = x * c - y * s; ny = x * s + y * c; }
            model.vertices[i] = nx + 0.5f;
            model.vertices[i + 1] = ny + 0.5f;
            model.vertices[i + 2] = nz + 0.5f;
        }
        DisableCulling(model);
    }

    // AngleHelper.horizontalAngle: toYRot, negated on the X axis.
    float HorizontalAngle(const string& facing) {
        if (facing == "west") return -90.0f;
        if (facing == "north") return 180.0f;
        if (facing == "east") return -270.0f;
        return 0.0f;
    }

    void ReplaceTexturePath(ModelData& model, const string& oldPath, const string& newPath) {
        string saveRoot = "textures";
        if (!SaveTextureToFile("create", newPath, saveRoot)) return;
        string savedPath = "textures/create/" + newPath + ".png";
        RegisterTexture("create", newPath, savedPath);
        const string oldSaved = "textures/create/" + oldPath + ".png";
        for (auto& material : model.materials) {
            if (material.texturePath != oldSaved && material.name != "create:" + oldPath) continue;
            material.name = "create:" + newPath;
            material.texturePath = savedPath;
            material.type = DetectMaterialType("create", newPath, material.aspectRatio);
        }
    }

    // Port of BeltRenderer (belt + pulley partials) and BeltModel (casing from the
    // blockstate model, andesite sprite shift, covers).
    ModelData GenerateCreateBelt(const string& blockName, const unordered_map<string, string>& states,
        const NbtTagPtr& nbt) {
        const string facing = states.contains("facing") ? states.at("facing") : "south";
        const string slope = states.contains("slope") ? states.at("slope") : "horizontal";
        const string part = states.contains("part") ? states.at("part") : "middle";
        const bool casing = states.contains("casing") && states.at("casing") == "true";
        const bool downward = slope == "downward", upward = slope == "upward";
        const bool diagonal = downward || upward;
        const bool sideways = slope == "sideways", vertical = slope == "vertical";
        const bool alongX = facing == "east" || facing == "west";
        const bool positive = facing == "east" || facing == "south";

        bool start = part == "start", end = part == "end";
        if (downward || (vertical && positive)) swap(start, end);
        string piece = start ? "start" : end ? "end" : "middle";

        ModelData result;
        Append(result, LoadCreateModel(diagonal ? "block/belt/diagonal_" + piece : "block/belt/" + piece, 110));
        if (!diagonal) Append(result, LoadCreateModel("block/belt/" + piece + "_bottom", 111));
        // center().rotateY(a).rotateZ(b).rotateX(c).uncenter(): X first, then Z, then Y.
        McRotate(result, 'x', (!diagonal && slope != "horizontal") ? 90.0f : 0.0f);
        McRotate(result, 'z', sideways ? 90.0f : 0.0f);
        McRotate(result, 'y', HorizontalAngle(facing) + (upward ? 180.0f : 0.0f) + (sideways ? 270.0f : 0.0f));

        string dye = NbtString(nbt, "Dye");
        transform(dye.begin(), dye.end(), dye.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (!dye.empty()) {
            string baseTexture = diagonal ? "block/belt_diagonal" : "block/belt";
            string dyedTexture = "block/belt/" + dye + (diagonal ? "_diagonal_scroll" : "_scroll");
            ReplaceTexture(result, baseTexture, dyedTexture);
        }

        if (part != "middle") {
            // dir = sideways ? UP : facing.getClockWise(); center, [Y90 | X90], X90, uncenter.
            ModelData pulley = LoadCreateModel("block/belt_pulley", 112);
            McRotate(pulley, 'x', 90.0f);
            if (sideways) McRotate(pulley, 'x', 90.0f);
            else if (!alongX) McRotate(pulley, 'y', 90.0f); // clockwise of N/S is on the X axis
            Append(result, std::move(pulley));
        }

        if (casing) {
            ModelData casingModel = LoadBlockstateModel(blockName);
            string casingType = NbtString(nbt, "Casing");
            transform(casingType.begin(), casingType.end(), casingType.begin(), [](unsigned char c) { return static_cast<char>(toupper(c)); });
            if (casingType == "ANDESITE")
                ReplaceTexturePath(casingModel, "block/belt/brass_belt_casing", "block/belt/andesite_belt_casing");
            Append(result, std::move(casingModel));
            if (NbtBool(nbt, "Covered")) {
                string cover = "block/belt_cover/" + string(casingType == "ANDESITE" ? "andesite" : "brass")
                    + "_belt_cover_" + (alongX ? "x" : "z");
                Append(result, LoadCreateModel(cover, 114));
            }
        }
        return result;
    }

    // Create GearboxRenderer / SplitShaftRenderer place SHAFT_HALF partials via Flywheel.
    // shaft_half.json extends toward +Z (south); RotateFromSouth aims it.
    ModelData MakeShaftHalfFacing(const string& direction) {
        ModelData half = LoadCreateModel("block/shaft_half", 133);
        RotateFromSouth(half, direction);
        return half;
    }

    // Gearbox: casing from blockstate + half-shafts on every direction NOT on the box axis.
    ModelData GenerateCreateGearbox(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        static const char* dirs[] = {"down","up","north","south","west","east"};
        static const char* dirAxis[] = {"y","y","z","z","x","x"};
        for (int i = 0; i < 6; ++i) {
            if (axis == dirAxis[i]) continue;
            Append(result, MakeShaftHalfFacing(dirs[i]));
        }
        return result;
    }

    // Clutch / gearshift / sequenced_gearshift: casing + half-shafts ON the rotation axis.
    ModelData GenerateCreateSplitShaft(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        if (axis == "x") {
            Append(result, MakeShaftHalfFacing("west"));
            Append(result, MakeShaftHalfFacing("east"));
        } else if (axis == "z") {
            Append(result, MakeShaftHalfFacing("north"));
            Append(result, MakeShaftHalfFacing("south"));
        } else {
            Append(result, MakeShaftHalfFacing("down"));
            Append(result, MakeShaftHalfFacing("up"));
        }
        return result;
    }

    // Fluid valve: casing (blockstate) + Kinetic shaft on rotation axis + FLUID_VALVE_POINTER.
    // Sticker: casing + STICKER_HEAD (offset from extended). Backtank: shaft_input + cogs at rest.
    // Steam whistle: casing + size mouth at offset 0.
    string DirectionalAxisRotation(const string& facing, bool alongFirst) {
        if (facing == "east" || facing == "west") return alongFirst ? "y" : "z";
        if (facing == "up" || facing == "down") return alongFirst ? "x" : "z";
        return alongFirst ? "x" : "y";
    }

    ModelData GenerateCreateFluidValve(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "up";
        bool alongFirst = states.contains("axis_along_first") && states.at("axis_along_first") == "true";
        string axis = DirectionalAxisRotation(facing, alongFirst);
        ModelData shaft = LoadCreateModel("block/shaft", 290);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        string facingAxis = (facing == "east" || facing == "west") ? "x" :
            (facing == "up" || facing == "down") ? "y" : "z";
        string pipeAxis = "xyz";
        pipeAxis.erase(pipeAxis.find(facingAxis), 1);
        pipeAxis.erase(pipeAxis.find(axis), 1); // remaining axis
        int pointerOffset = ((pipeAxis != "y" && axis == "x") || pipeAxis == "y") ? 90 : 0;
        bool enabled = states.contains("enabled") && states.at("enabled") == "true";
        float pointerRot = enabled ? -90.0f : 0.0f;
        float pitch = facing == "up" ? 0.0f : facing == "down" ? 180.0f : 90.0f;
        float yaw = -FacingY(facing); // AngleHelper.horizontalAngle (X-axis facings negated)
        ModelData pointer = LoadCreateModel("block/fluid_valve/pointer", 291);
        Rotate(pointer, 0.0f, pointerOffset + pointerRot, 0.0f);
        Rotate(pointer, pitch, 0.0f, 0.0f);
        Rotate(pointer, 0.0f, yaw, 0.0f);
        Append(result, std::move(pointer));
        return result;
    }

    ModelData GenerateCreateSticker(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "up";
        bool extended = states.contains("extended") && states.at("extended") == "true";
        float offset = extended ? 4.0f / 16.0f : 0.0f; // offset*offset at 0 or 1
        float pitch = facing == "up" ? 0.0f : facing == "down" ? 180.0f : 90.0f;
        ModelData head = LoadCreateModel("block/sticker/head", 292);
        if (offset != 0.0f) {
            for (size_t i = 1; i < head.vertices.size(); i += 3) head.vertices[i] += offset;
        }
        Rotate(head, pitch, 0.0f, 0.0f);
        Rotate(head, 0.0f, -FacingY(facing), 0.0f);
        Append(result, std::move(head));
        return result;
    }

    ModelData GenerateCreateBacktank(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "north";
        string prefix = id == "netherite_backtank" ? "block/netherite_backtank" : "block/copper_backtank";
        ModelData shaft = LoadCreateModel(prefix + "/block_shaft_input", 293);
        // Shaft stub on top; KineticBlockEntityRenderer orients with rotateToFace(horizontal facing).
        Rotate(shaft, 0.0f, -FacingY(facing), 0.0f);
        Append(result, std::move(shaft));
        ModelData cogs = LoadCreateModel(prefix + "/block_cogs", 294);
        // Rest angle 0: pivot translates cancel; only Ry(180+horizontalAngle).
        Rotate(cogs, 0.0f, 180.0f - FacingY(facing), 0.0f);
        Append(result, std::move(cogs));
        return result;
    }

    ModelData GenerateCreateSteamWhistle(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "north";
        string size = states.contains("size") ? states.at("size") : "medium";
        string mouthPath = size == "large" ? "block/steam_whistle/large_mouth" :
            size == "small" ? "block/steam_whistle/small_mouth" : "block/steam_whistle/medium_mouth";
        ModelData mouth = LoadCreateModel(mouthPath, 295);
        // offset 0: center → Ry(hA) → uncenter → T(0,0,0)
        Rotate(mouth, 0.0f, -FacingY(facing), 0.0f);
        Append(result, std::move(mouth));
        return result;
    }

    // Flywheel / crushing_wheel / blaze cage use porting_lib:obj (same as water wheels).
    // Blaze also gets inert/idle blaze + rods (+ static flame) from heat property "blaze".
    ModelData GenerateCreateObjKinetic(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        string jsonPath;
        if (id == "flywheel") jsonPath = "block/flywheel/block";
        else if (id == "crushing_wheel") jsonPath = "block/crushing_wheel/block";
        else if (id == "blaze_burner") jsonPath = "block/blaze_burner/block";
        else return {};
        ModelData result = LoadCreateObjModel(jsonPath, 140);
        if (result.vertices.empty()) return result;
        if (id == "blaze_burner") {
            // Cage / brazier only. Head, rods, flame, goggles and train hat are
            // appended by AppendCreateBlazePackager in the static renderer pose.
            (void)states;
            return result;
        }
        string axis = states.contains("axis") ? states.at("axis") : "y";
        RotateKineticAxis(result, axis);
        return result;
    }

    void TranslateModel(ModelData& model, float dx, float dy, float dz) {
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            model.vertices[i] += dx;
            model.vertices[i + 1] += dy;
            model.vertices[i + 2] += dz;
        }
    }

    // Rotate around world origin (0,0,0), unlike ApplyRotationToVertices which uses 0.5.
    void RotateModelOrigin(ModelData& model, float rx, float ry, float rz) {
        if (model.vertices.empty()) return;
        const float radX = rx * (float)M_PI / 180.0f;
        const float radY = ry * (float)M_PI / 180.0f;
        const float radZ = rz * (float)M_PI / 180.0f;
        const float cosX = cosf(radX), sinX = sinf(radX);
        const float cosY = cosf(radY), sinY = sinf(radY);
        const float cosZ = cosf(radZ), sinZ = sinf(radZ);
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            float x = model.vertices[i], y = model.vertices[i + 1], z = model.vertices[i + 2];
            if (rx != 0.0f) {
                float ny = y * cosX - z * sinX;
                float nz = y * sinX + z * cosX;
                y = ny; z = nz;
            }
            if (ry != 0.0f) {
                float nx = x * cosY + z * sinY;
                float nz = -x * sinY + z * cosY;
                x = nx; z = nz;
            }
            if (rz != 0.0f) {
                float nx = x * cosZ - y * sinZ;
                float ny = x * sinZ + y * cosZ;
                x = nx; y = ny;
            }
            model.vertices[i] = x;
            model.vertices[i + 1] = y;
            model.vertices[i + 2] = z;
        }
        DisableCulling(model);
    }

    // Models authored pointing NORTH (-Z), e.g. encased_fan propeller.
    void RotateFromNorth(ModelData& model, const string& direction) {
        if (direction == "south") Rotate(model, 0.0f, 180.0f, 0.0f);
        else if (direction == "east") Rotate(model, 0.0f, 270.0f, 0.0f);
        else if (direction == "west") Rotate(model, 0.0f, 90.0f, 0.0f);
        else if (direction == "up") Rotate(model, 90.0f, 0.0f, 0.0f);
        else if (direction == "down") Rotate(model, 270.0f, 0.0f, 0.0f);
        // north: identity
    }

    // Fluid-capability neighbors (approx; Create uses FluidStorage.SIDED).
    // Hose pulley is explicitly excluded from DRAIN in FluidTransportBehaviour.
    bool IsCreateFluidCapable(const string& id) {
        return id == "create:fluid_tank" || id == "create:creative_fluid_tank" ||
            id == "create:item_drain" || id == "create:spout" ||
            id == "create:portable_fluid_interface" || id == "create:basin" ||
            id == "create:blender" || id == "create:steam_engine" ||
            id == "create:hose_pulley"; // listed so callers can special-case
    }

    // steam_engine: housing (blockstate) + SteamEngineRenderer piston / linkage / shaft
    // connector, only when a powered_shaft sits 2 blocks along the facing (getShaft()).
    // Static: shaft angle 0 -> piston = -sqrt(.875^2 - .375^2), linkage tilt acos(-piston/.875).
    ModelData GenerateCreateSteamEngine(const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        string face = states.contains("face") ? states.at("face") : "floor";
        string facing = face == "floor" ? "up" : face == "ceiling" ? "down" :
            (states.contains("facing") ? states.at("facing") : "north");
        int dx = facing == "east" ? 1 : facing == "west" ? -1 : 0;
        int dy = facing == "up" ? 1 : facing == "down" ? -1 : 0;
        int dz = facing == "south" ? 1 : facing == "north" ? -1 : 0;
        Block shaftBlock = GetBlockById(GetBlockId(x + 2 * dx, y + 2 * dy, z + 2 * dz));
        if (BaseBlockName(shaftBlock.name) != "create:powered_shaft") return result;
        auto shaftStates = ParseBlockStates(shaftBlock.name);
        string axis = shaftStates.contains("axis") ? shaftStates.at("axis") : "y";
        bool vertical = dy != 0;
        string facingAxis = vertical ? "y" : dx != 0 ? "x" : "z";
        if (axis == facingAxis) return result;
        bool roll90 = (!vertical && axis == "y") || (vertical && axis == "z");
        // AngleHelper horizontalAngle / verticalAngle(+90) / roll, applied about the centre.
        const float yaw = vertical ? 0.0f : -FacingY(facing);
        const float pitch = facing == "up" ? 0.0f : facing == "down" ? 180.0f : 90.0f;
        auto place = [&](ModelData& part) {
            if (roll90) Rotate(part, 0.0f, -90.0f, 0.0f);
            Rotate(part, pitch, 0.0f, 0.0f);
            Rotate(part, 0.0f, yaw, 0.0f);
            Append(result, std::move(part));
        };
        const float piston = -sqrtf(0.875f * 0.875f - 0.375f * 0.375f);
        const float linkAngle = acosf(-piston / 0.875f) * 180.0f / (float)M_PI;
        ModelData pistonPart = LoadCreateModel("block/steam_engine/piston", 270);
        TranslateModel(pistonPart, 0.0f, piston + 1.25f, 0.0f);
        place(pistonPart);
        ModelData linkage = LoadCreateModel("block/steam_engine/linkage", 271);
        TranslateModel(linkage, 0.0f, -0.25f, -0.5f);
        RotateModelOrigin(linkage, linkAngle, 0.0f, 0.0f);
        TranslateModel(linkage, 0.0f, 0.25f + piston + 2.25f, 0.5f);
        place(linkage);
        ModelData connector = LoadCreateModel("block/steam_engine/shaft_connector", 272);
        Rotate(connector, -90.0f, 0.0f, 0.0f);
        TranslateModel(connector, 0.0f, 2.0f, 0.0f);
        place(connector);
        return result;
    }

    // FluidPipe: multipart body is in the blockstate; rims/connectors come from
    // PipeAttachmentModel (BE AttachmentTypes). Approximate from open faces + neighbors.
    // DRAIN vs RIM: fluid-capable neighbor (not hose_pulley) → drain; pipe-like → connection;
    // else → rim (+ connector). Matches FluidTransportBehaviour.getRenderedRimAttachment.
    ModelData GenerateCreateFluidPipe(const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        static const char* dirs[] = {"down", "up", "north", "south", "west", "east"};
        static const int dxyz[6][3] = {{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
        auto isPipeLike = [](const string& id) -> bool {
            return id == "create:fluid_pipe" || id == "create:glass_fluid_pipe" ||
                id == "create:encased_fluid_pipe" || id == "create:smart_fluid_pipe" ||
                id == "create:mechanical_pump" || id == "create:fluid_valve";
        };
        // Which faces are open: explicit bool props, or both ends of glass pipe axis.
        bool open[6] = {};
        bool anyExplicit = false;
        for (int i = 0; i < 6; ++i) {
            auto it = states.find(dirs[i]);
            if (it != states.end()) {
                anyExplicit = true;
                open[i] = it->second == "true";
            }
        }
        if (!anyExplicit) {
            string axis = states.contains("axis") ? states.at("axis") : "";
            if (axis == "y") { open[0] = open[1] = true; }
            else if (axis == "z") { open[2] = open[3] = true; }
            else if (axis == "x") { open[4] = open[5] = true; }
        }
        for (int i = 0; i < 6; ++i) {
            if (!open[i]) continue;
            Block nb = GetBlockById(GetBlockId(x + dxyz[i][0], y + dxyz[i][1], z + dxyz[i][2]));
            string nbId = nb.name;
            size_t br = nbId.find('[');
            if (br != string::npos) nbId = nbId.substr(0, br);
            if (isPipeLike(nbId)) {
                Append(result, LoadCreateModel(string("block/fluid_pipe/connection/") + dirs[i], 150 + i));
            } else if (IsCreateFluidCapable(nbId) && nbId != "create:hose_pulley") {
                // AttachmentTypes.DRAIN = rim_connector + drain
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim_connector/") + dirs[i], 160 + i));
                Append(result, LoadCreateModel(string("block/fluid_pipe/drain/") + dirs[i], 170 + i));
            } else {
                // AttachmentTypes.RIM = rim_connector + rim
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim_connector/") + dirs[i], 160 + i));
                Append(result, LoadCreateModel(string("block/fluid_pipe/rim/") + dirs[i], 175 + i));
            }
        }
        // Pipe casing when >2 connections off one axis (FluidPipeBlock.shouldDrawCasing).
        // Only for the copper-casing variant of fluid_pipe (not glass/encased).
        string base = BaseBlockName(blockName);
        if (base == "create:fluid_pipe") {
            for (int axis = 0; axis < 3; ++axis) {
                int connections = 0;
                for (int i = 0; i < 6; ++i) {
                    int a = (i <= 1) ? 1 : (i <= 3) ? 2 : 0;
                    if (a == axis) continue;
                    if (open[i]) connections++;
                }
                if (connections > 2) {
                    Append(result, LoadCreateModel("block/fluid_pipe/casing", 180));
                    break;
                }
            }
        }
        return result;
    }

    void TranslateModel(ModelData& model, float dx, float dy, float dz);

    // Toolbox body is in the blockstate; lid (+ drawer) are Flywheel partials.
    ModelData GenerateCreateToolbox(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string base = BaseBlockName(blockName);
        // create:brown_toolbox → brown
        string color = "brown";
        size_t colon = base.find(':');
        string local = colon == string::npos ? base : base.substr(colon + 1);
        const string suffix = "_toolbox";
        if (local.size() > suffix.size() &&
            local.compare(local.size() - suffix.size(), suffix.size(), suffix) == 0) {
            color = local.substr(0, local.size() - suffix.size());
        }
        // ToolboxRenderer: lid + two drawers (second 1/8 higher), each rotated
        // rotateYDegrees(-facing.opposite.toYRot()) == blockstate y of the body.
        string facing = states.contains("facing") ? states.at("facing") : "north";
        float yRot = fmodf(FacingY(facing) + 180.0f, 360.0f);
        ModelData lid = LoadCreateModel("block/toolbox/lid/" + color, 190);
        if (yRot != 0.0f) Rotate(lid, 0.0f, yRot, 0.0f);
        Append(result, std::move(lid));
        for (int offset = 0; offset < 2; ++offset) {
            ModelData drawer = LoadCreateModel("block/toolbox/drawer", 191);
            if (drawer.vertices.empty()) break;
            // Drawer JSON hardcodes brown; remap to this toolbox's dye sheet.
            ReplaceTexture(drawer, "block/toolbox/brown", "block/toolbox/" + color);
            TranslateModel(drawer, 0.0f, offset / 8.0f, 0.0f);
            if (yRot != 0.0f) Rotate(drawer, 0.0f, yRot, 0.0f);
            Append(result, std::move(drawer));
        }
        (void)states;
        return result;
    }

    // Harvester / roller: casing (blockstate) + BE-rendered parts at rest (angle 0).
    // HarvesterRenderer: blade rotateCentered(horizontalAngle(facing)).
    // RollerRenderer: frame rotateCentered(horizontalAngle+180); wheel (obj) Ry90 about
    // origin, T(0,-.5,.5), rotateCentered(horizontalAngle), T(normal*1.0625), T(0,-.25,0).
    ModelData GenerateCreateHarvesterRoller(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "south";
        const float angle = -FacingY(facing); // AngleHelper.horizontalAngle
        if (id == "mechanical_harvester") {
            ModelData blade = LoadCreateModel("block/mechanical_harvester/blade", 260);
            Rotate(blade, 0.0f, angle, 0.0f);
            Append(result, std::move(blade));
            return result;
        }
        ModelData frame = LoadCreateModel("block/mechanical_roller/frame", 261);
        Rotate(frame, 0.0f, angle + 180.0f, 0.0f);
        Append(result, std::move(frame));
        ModelData wheel = LoadCreateObjModel("block/mechanical_roller/wheel", 262);
        if (!wheel.vertices.empty()) {
            RotateModelOrigin(wheel, 0.0f, 90.0f, 0.0f);
            TranslateModel(wheel, 0.0f, -0.5f, 0.5f);
            Rotate(wheel, 0.0f, angle, 0.0f);
            float nx = facing == "east" ? 1.0f : facing == "west" ? -1.0f : 0.0f;
            float nz = facing == "south" ? 1.0f : facing == "north" ? -1.0f : 0.0f;
            TranslateModel(wheel, nx * 1.0625f, -0.25f, nz * 1.0625f);
            Append(result, std::move(wheel));
        }
        return result;
    }

    // mechanical_mixer: casing JSON + shaftless cog + pole + head at rest (offset 0, angle 0).
    ModelData GenerateCreateMixer(const string& blockName) {
        ModelData result = LoadBlockstateModel(blockName);
        Append(result, LoadCreateModel("block/cogwheel_shaftless", 200));
        Append(result, LoadCreateModel("block/mechanical_mixer/pole", 201));
        Append(result, LoadCreateModel("block/mechanical_mixer/head", 202));
        return result;
    }

    // mechanical_press: casing (blockstate) + shaft on facing axis + head facing horizontal.
    ModelData GenerateCreatePress(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "north";
        string axis = (facing == "east" || facing == "west") ? "x" : "z";
        ModelData shaft = LoadCreateModel("block/shaft", 210);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        ModelData head = LoadCreateModel("block/mechanical_press/head", 211);
        Rotate(head, 0.0f, FacingY(facing), 0.0f);
        Append(result, std::move(head));
        return result;
    }

    // encased_fan: casing (blockstate) + SHAFT_HALF + propeller on opposite face, rest angle 0.
    ModelData GenerateCreateEncasedFan(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "south";
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},
            {"west","east"},{"up","down"},{"down","up"}
        };
        string back = opposite.contains(facing) ? opposite.at(facing) : "north";
        Append(result, MakeShaftHalfFacing(back));
        ModelData prop = LoadCreateModel("block/encased_fan/propeller", 220);
        // Propeller JSON faces NORTH (-Z); aim at the back (intake) side like EncasedFanRenderer.
        RotateFromNorth(prop, back);
        Append(result, std::move(prop));
        return result;
    }

    // Centred right-handed rotation (PoseStack rotateX/Y/ZDegrees, Direction.EAST/UP/SOUTH).
    void PoseRotate(ModelData& model, char axis, float degrees) {
        if (model.vertices.empty() || fmodf(degrees, 360.0f) == 0.0f) return;
        const float rad = degrees * (float)M_PI / 180.0f;
        const float c = roundf(cosf(rad) * 1e6f) / 1e6f, s = roundf(sinf(rad) * 1e6f) / 1e6f;
        for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
            float vx = model.vertices[i] - 0.5f, vy = model.vertices[i + 1] - 0.5f, vz = model.vertices[i + 2] - 0.5f;
            float nx = vx, ny = vy, nz = vz;
            if (axis == 'x') { ny = vy * c - vz * s; nz = vy * s + vz * c; }
            else if (axis == 'y') { nx = vx * c + vz * s; nz = -vx * s + vz * c; }
            else { nx = vx * c - vy * s; ny = vx * s + vy * c; }
            model.vertices[i] = nx + 0.5f; model.vertices[i + 1] = ny + 0.5f; model.vertices[i + 2] = nz + 0.5f;
        }
        DisableCulling(model);
    }

    // AngleHelper.horizontalAngle / verticalAngle.
    float PoseHorizontalAngle(const string& d) {
        return d == "west" ? -90.0f : d == "north" ? 180.0f : d == "east" ? -270.0f : 0.0f;
    }
    float PoseVerticalAngle(const string& d) { return d == "up" ? -90.0f : d == "down" ? 90.0f : 0.0f; }
    char DirectionAxis(const string& d) {
        return d == "east" || d == "west" ? 'x' : d == "up" || d == "down" ? 'y' : 'z';
    }
    string OppositeDirection(const string& d) {
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},{"west","east"},{"up","down"},{"down","up"}};
        return opposite.contains(d) ? opposite.at(d) : d;
    }

    // CachedBuffers.partialFacing / rotateToFace: center, rotateY(horizontal), rotateX(vertical).
    ModelData LoadPartialFacing(const string& path, const string& facing, int index) {
        ModelData part = LoadCreateModel(path, index);
        PoseRotate(part, 'x', PoseVerticalAngle(facing));
        PoseRotate(part, 'y', PoseHorizontalAngle(facing));
        return part;
    }

    // KineticBlockEntityVisual.rotationOffset (non large cog).
    float KineticRestAngle(char axis, int x, int y, int z) {
        int sum = (axis == 'x' ? 0 : x) + (axis == 'y' ? 0 : y) + (axis == 'z' ? 0 : z);
        return sum % 2 == 0 ? 22.5f : 0.0f;
    }

    // mechanical_drill: casing + DRILL_HEAD (DrillRenderer, partialFacing + kinetic rest angle).
    ModelData GenerateCreateDrill(const string& blockName, const unordered_map<string, string>& states,
        int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "north";
        ModelData head = LoadPartialFacing("block/mechanical_drill/head", facing, 280);
        const char axis = DirectionAxis(facing);
        PoseRotate(head, axis, KineticRestAngle(axis, x, y, z));
        Append(result, std::move(head));
        return result;
    }

    // mechanical_saw: casing + inactive blade (SawRenderer.renderBlade) + shaft (renderShaft).
    ModelData GenerateCreateSaw(const string& blockName, const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "up";
        const bool alongFirst = states.contains("axis_along_first") && states.at("axis_along_first") == "true";
        const bool horizontal = DirectionAxis(facing) != 'y';
        ModelData blade = LoadPartialFacing(horizontal ? "block/mechanical_saw/blade_horizontal_inactive" :
            "block/mechanical_saw/blade_vertical_inactive", facing, 281);
        if (!horizontal && alongFirst) PoseRotate(blade, 'y', 90.0f);
        Append(result, std::move(blade));
        if (horizontal) Append(result, LoadPartialFacing("block/shaft_half", OppositeDirection(facing), 284));
        else {
            ModelData shaft = LoadCreateModel("block/shaft", 282);
            RotateKineticAxis(shaft, alongFirst ? "x" : "z"); // DirectionalAxisKineticBlock, facing Y
            Append(result, std::move(shaft));
        }
        return result;
    }

    // mechanical / windmill / clockwork bearing: casing + SHAFT_HALF (back) + top (BearingRenderer).
    ModelData GenerateCreateBearing(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "up";
        Append(result, LoadPartialFacing("block/shaft_half", OppositeDirection(facing), 284));
        ModelData top = LoadCreateModel(id == "windmill_bearing" ? "block/bearing/top_wooden" : "block/bearing/top", 283);
        PoseRotate(top, 'x', -90.0f - PoseVerticalAngle(facing));
        if (DirectionAxis(facing) != 'y') PoseRotate(top, 'y', PoseHorizontalAngle(OppositeDirection(facing)));
        Append(result, std::move(top));
        return result;
    }

    // deployer: casing (blockstate) + shaft + pole + hand_pointing at offset 0.
    ModelData GenerateCreateDeployer(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "south";
        bool axisAlongFirst = states.contains("axis_along_first") &&
            states.at("axis_along_first") == "true";
        // Rotation axis — DirectionalAxisKineticBlock.getRotationAxis:
        //   facing X → alongFirst ? Y : Z
        //   facing Y → alongFirst ? X : Z
        //   facing Z → alongFirst ? X : Y
        string axis = "y";
        if (facing == "east" || facing == "west")
            axis = axisAlongFirst ? "y" : "z";
        else if (facing == "up" || facing == "down")
            axis = axisAlongFirst ? "x" : "z";
        else // north/south
            axis = axisAlongFirst ? "x" : "y";
        ModelData shaft = LoadCreateModel("block/shaft", 230);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));

        // DeployerRenderer.transform: rotateCentered Y, then X(EAST), then Z(SOUTH).
        auto deployOrient = [&](ModelData& part, bool axisDirectionMatters) {
            float yRot = FacingY(facing); // 0 for up/down (AngleHelper.horizontalAngle)
            float xRot = facing == "up" ? 270.0f : facing == "down" ? 90.0f : 0.0f;
            bool facingAxisZ = (facing == "north" || facing == "south");
            float zRot = 0.0f;
            if (axisDirectionMatters && (axisAlongFirst ^ facingAxisZ)) zRot = 90.0f;
            if (yRot != 0.0f) Rotate(part, 0.0f, yRot, 0.0f);
            if (xRot != 0.0f) Rotate(part, xRot, 0.0f, 0.0f);
            if (zRot != 0.0f) Rotate(part, 0.0f, 0.0f, zRot);
        };
        ModelData pole = LoadCreateModel("block/deployer/pole", 231);
        deployOrient(pole, true);
        Append(result, std::move(pole));
        ModelData hand = LoadCreateModel("block/deployer/hand_pointing", 232);
        deployOrient(hand, false);
        Append(result, std::move(hand));
        return result;
    }

    // mechanical_pump: casing + MECHANICAL_PUMP_COG (PumpRenderer: partialFacing + kinetic rest angle).
    // millstone: casing + MILLSTONE_COG (MillstoneRenderer: unrotated partial, axis Y).
    ModelData GenerateCreateCogPartial(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = "y";
        ModelData cog;
        if (id == "mechanical_pump") {
            const string facing = states.contains("facing") ? states.at("facing") : "north";
            axis = facing == "east" || facing == "west" ? "x" : facing == "up" || facing == "down" ? "y" : "z";
            cog = LoadCreateModel("block/mechanical_pump/cog", 290);
            // CachedBuffers.rotateToFace: rotateY(horizontalAngle) after rotateX(verticalAngle)
            float vertical = facing == "up" ? -90.0f : facing == "down" ? 90.0f : 0.0f;
            float horizontal = facing == "west" ? -90.0f : facing == "north" ? 180.0f : facing == "east" ? -270.0f : 0.0f;
            Rotate(cog, vertical, horizontal, 0.0f);
        }
        else cog = LoadCreateModel("block/millstone/inner", 291);
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        if (parity % 2 == 0) RotateAroundAxis(cog, axis, 22.5f);
        DisableCulling(cog);
        Append(result, std::move(cog));
        return result;
    }

    // Apply ArmRenderer transform stack (resting NO_TARGET pose) to one arm partial.
    // Create: center → [ceiling Rx180] → base(Ty 4/16, Ry) → lower(Ty 2/16, Rx+135) →
    // upper(Tz -14/16, Rx-90) → head(Tz -15/16, Rx-45) → claw grips.
    // Renderer feeds lowerArmAngle-135 / upperArmAngle-90; NO_TARGET raw is 135/45/0/0 →
    // passed angles base=0, lower=0, upper=-45, head=0.
    void ApplyArmRestPose(ModelData& part, int stage, bool ceiling, float clawFlip = 0.0f) {
        // stage: 0=base, 1=lower, 2=upper, 3=clawBase, 4=clawGrip
        const float baseAngle = 0.0f;
        const float lowerPassed = 0.0f;   // 135 - 135
        const float upperPassed = -45.0f; // 45 - 90
        const float headPassed = 0.0f;

        // Apply innermost first (vertex = M_outer * ... * M_inner * v).
        if (stage >= 4) {
            // transformClawHalf: translate(0, -flip * 1/16, -6/16) for empty hand
            TranslateModel(part, 0.0f, -clawFlip * (1.0f / 16.0f), -6.0f / 16.0f);
        }
        if (stage >= 3) {
            // transformHead
            RotateModelOrigin(part, headPassed - 45.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 0.0f, -15.0f / 16.0f);
        }
        if (stage >= 2) {
            RotateModelOrigin(part, upperPassed - 90.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 0.0f, -14.0f / 16.0f);
        }
        if (stage >= 1) {
            RotateModelOrigin(part, lowerPassed + 135.0f, 0.0f, 0.0f);
            TranslateModel(part, 0.0f, 2.0f / 16.0f, 0.0f);
        }
        // transformBase
        RotateModelOrigin(part, 0.0f, baseAngle, 0.0f);
        TranslateModel(part, 0.0f, 4.0f / 16.0f, 0.0f);
        if (ceiling) RotateModelOrigin(part, 180.0f, 0.0f, 0.0f);
        TranslateModel(part, 0.5f, 0.5f, 0.5f);
    }

    // speedometer / stressometer (GaugeRenderer): ShaftRenderer shaft + dial (rest, progress 0)
    // and head on every horizontal face that GaugeBlock.shouldRenderHeadOnFace allows.
    ModelData GenerateCreateGauge(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "north";
        const bool alongFirst = states.contains("axis_along_first") && states.at("axis_along_first") == "true";
        const string facingAxis = facing == "east" || facing == "west" ? "x" : facing == "up" || facing == "down" ? "y" : "z";
        const string axis = facingAxis == "x" ? (alongFirst ? "y" : "z") :
            facingAxis == "y" ? (alongFirst ? "x" : "z") : (alongFirst ? "x" : "y");
        ModelData shaft = LoadCreateModel("block/shaft", 295);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},{"west","east"},{"up","down"},{"down","up"}};
        static const struct { const char* name; const char* axis; float yRot; } faces[] = {
            {"south", "z", 0.0f}, {"west", "x", 90.0f}, {"north", "z", 180.0f}, {"east", "x", 270.0f}};
        for (const auto& face : faces) {
            if (face.name == opposite.at(facing) || axis == face.axis) continue;
            if (axis == "y" && face.name != facing) continue;
            for (const char* part : { "block/gauge/dial", id == "speedometer" ? "block/gauge/speedometer/head" :
                    "block/gauge/stressometer/head" }) {
                ModelData piece = LoadCreateModel(part, 296);
                Rotate(piece, 0.0f, -face.yRot - 90.0f, 0.0f); // rotateBufferTowards
                Append(result, std::move(piece));
            }
        }
        return result;
    }

    // analog_lever (AnalogLeverRenderer): handle tilted by State/15*90 deg + indicator.
    ModelData GenerateCreateAnalogLever(const string& blockName,
        const unordered_map<string, string>& states, const NbtTagPtr& nbt) {
        ModelData result = LoadBlockstateModel(blockName);
        const string face = states.contains("face") ? states.at("face") : "floor";
        const string facing = states.contains("facing") ? states.at("facing") : "north";
        const float rX = face == "floor" ? 0.0f : face == "wall" ? 90.0f : 180.0f;
        const float rY = facing == "west" ? -90.0f : facing == "north" ? 180.0f : facing == "east" ? -270.0f : 0.0f;
        int state = 0;
        if (nbt && nbt->type == TagType::COMPOUND) {
            auto tag = getChildByName(nbt, "State");
            if (tag && tag->type == TagType::INT) state = bytesToInt(tag->payload);
        }
        ModelData handle = LoadCreateModel("block/analog_lever/handle", 297);
        TranslateModel(handle, -0.5f, -0.0625f, -0.5f);
        RotateModelOrigin(handle, state / 15.0f * 90.0f, 0.0f, 0.0f);
        TranslateModel(handle, 0.5f, 0.0625f, 0.5f);
        ModelData indicator = LoadCreateModel("block/analog_lever/indicator", 298);
        for (ModelData* part : { &handle, &indicator }) {
            Rotate(*part, rX, 0.0f, 0.0f);
            Rotate(*part, 0.0f, rY, 0.0f);
            Append(result, std::move(*part));
        }
        DisableCulling(result);
        return result;
    }

    // cuckoo_clock (CuckooClockRenderer): back SHAFT_HALF, hands and closed doors at rest.
    ModelData GenerateCreateCuckooClock(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "north";
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},{"west","east"}};
        static const unordered_map<string, string> counterClockWise = {
            {"north","west"},{"west","south"},{"south","east"},{"east","north"}};
        Append(result, MakeShaftHalfFacing(opposite.contains(facing) ? opposite.at(facing) : "south"));
        // Partials face west; renderer uses horizontalAngle(facing.getCounterClockWise()).
        const string cw = counterClockWise.contains(facing) ? counterClockWise.at(facing) : "west";
        const float yRot = cw == "west" ? -90.0f : cw == "north" ? 180.0f : cw == "east" ? -270.0f : 0.0f;
        for (const char* part : { "block/cuckoo_clock/hour_hand", "block/cuckoo_clock/minute_hand",
                "block/cuckoo_clock/left_door", "block/cuckoo_clock/right_door" }) {
            ModelData piece = LoadCreateModel(part, 299);
            Rotate(piece, 0.0f, yRot, 0.0f);
            Append(result, std::move(piece));
        }
        return result;
    }

    // mechanical_arm: base plate (blockstate) + ARM_COG + full arm at resting pose.
    ModelData GenerateCreateMechanicalArm(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        bool ceiling = states.contains("ceiling") && states.at("ceiling") == "true";
        Append(result, LoadCreateModel("block/mechanical_arm/cog", 240));

        ModelData base = LoadCreateModel("block/mechanical_arm/base", 241);
        ApplyArmRestPose(base, 0, ceiling);
        Append(result, std::move(base));

        ModelData lower = LoadCreateModel("block/mechanical_arm/lower_body", 242);
        ApplyArmRestPose(lower, 1, ceiling);
        Append(result, std::move(lower));

        ModelData upper = LoadCreateModel("block/mechanical_arm/upper_body", 243);
        ApplyArmRestPose(upper, 2, ceiling);
        Append(result, std::move(upper));

        ModelData claw = LoadCreateModel("block/mechanical_arm/claw_base", 244);
        ApplyArmRestPose(claw, 3, ceiling);
        Append(result, std::move(claw));

        for (int flip : {1, -1}) {
            ModelData grip = LoadCreateModel(
                flip > 0 ? "block/mechanical_arm/lower_claw_grip"
                         : "block/mechanical_arm/upper_claw_grip", 245);
            ApplyArmRestPose(grip, 4, ceiling, static_cast<float>(flip));
            Append(result, std::move(grip));
        }
        return result;
    }

    // gantry_carriage: blockstate casing + KineticBlockEntityRenderer shaft along the rotation
    // axis + GantryCarriageRenderer GANTRY_COGS (gantry_carriage/wheels) meshing with the shaft.
    ModelData GenerateCreateGantryCarriage(const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        auto rotateMc = [](ModelData& model, char axis, float degrees) {
            if (model.vertices.empty() || fmodf(degrees, 360.0f) == 0.0f) return;
            const float rad = degrees * (float)M_PI / 180.0f;
            const float c = roundf(cosf(rad) * 1e6f) / 1e6f, s = roundf(sinf(rad) * 1e6f) / 1e6f;
            for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
                float vx = model.vertices[i] - 0.5f, vy = model.vertices[i + 1] - 0.5f, vz = model.vertices[i + 2] - 0.5f;
                float nx = vx, ny = vy, nz = vz;
                if (axis == 'x') { ny = vy * c - vz * s; nz = vy * s + vz * c; }
                else if (axis == 'y') { nx = vx * c + vz * s; nz = -vx * s + vz * c; }
                model.vertices[i] = nx + 0.5f; model.vertices[i + 1] = ny + 0.5f; model.vertices[i + 2] = nz + 0.5f;
            }
        };
        const string facing = states.contains("facing") ? states.at("facing") : "up";
        const bool alongFirst = states.contains("axis_along_first") && states.at("axis_along_first") == "true";
        const char facingAxis = facing == "east" || facing == "west" ? 'x' : facing == "up" || facing == "down" ? 'y' : 'z';
        // DirectionalAxisKineticBlock.getRotationAxis
        char rotationAxis = facingAxis == 'x' ? (alongFirst ? 'y' : 'z') :
            facingAxis == 'y' ? (alongFirst ? 'x' : 'z') : (alongFirst ? 'x' : 'y');
        char gantryAxis = 'x';
        for (char a : { 'x', 'y', 'z' }) if (a != rotationAxis && a != facingAxis) gantryAxis = a;

        ModelData result = LoadBlockstateModel(blockName);
        Append(result, GenerateCreateKinetic("shaft", { {"axis", string(1, rotationAxis)} }, x, y, z));

        // Rest angle: KineticBlockEntityVisual.rotationOffset at the visual position.
        const bool positive = facing == "east" || facing == "up" || facing == "south";
        int vx = x, vy = y, vz = z;
        if (!positive) { if (facingAxis == 'x') vx++; else if (facingAxis == 'y') vy++; else vz++; }
        int sum = (rotationAxis == 'x' ? 0 : vx) + (rotationAxis == 'y' ? 0 : vy) + (rotationAxis == 'z' ? 0 : vz);
        float angle = sum % 2 == 0 ? 22.5f : 0.0f;
        if (gantryAxis == 'x' && facing == "up") angle = -angle;
        if (gantryAxis == 'y' && (facing == "north" || facing == "east")) angle = -angle;

        ModelData cogs = LoadCreateModel("block/gantry_carriage/wheels", 270);
        auto shiftY = [&](float dy) { for (size_t i = 1; i < cogs.vertices.size(); i += 3) cogs.vertices[i] += dy; };
        // center, rotY(h), rotX(v), rotY(a), translate(0,-9/16,0), rotX(-angle), translate(0,9/16,0), uncenter
        shiftY(0.5625f);
        rotateMc(cogs, 'x', -angle);
        shiftY(-0.5625f);
        rotateMc(cogs, 'y', (alongFirst ^ (facingAxis == 'x')) ? 0.0f : 90.0f);
        rotateMc(cogs, 'x', facing == "up" ? 0.0f : facing == "down" ? 180.0f : 90.0f);
        float horizontal = facing == "west" ? -90.0f : facing == "north" ? 180.0f : facing == "east" ? -270.0f : 0.0f;
        rotateMc(cogs, 'y', horizontal);
        DisableCulling(cogs);
        Append(result, std::move(cogs));
        return result;
    }


    // hand_crank: short axis stub (blockstate) + handle facing opposite, rest angle 0.
    ModelData GenerateCreateHandCrank(const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string facing = states.contains("facing") ? states.at("facing") : "up";
        static const unordered_map<string, string> opposite = {
            {"north","south"},{"south","north"},{"east","west"},
            {"west","east"},{"up","down"},{"down","up"}
        };
        string back = opposite.contains(facing) ? opposite.at(facing) : "down";
        ModelData handle = LoadCreateModel("block/hand_crank/handle", 250);
        // Handle JSON points UP by default (axis along +Y from bottom); HandCrank uses
        // partialFacing(..., facing.opposite). RotateFromSouth-style for UP-default:
        // map via blockstate-like x/y rotations from facing=up identity.
        if (back == "down") Rotate(handle, 180.0f, 0.0f, 0.0f);
        else if (back == "east") Rotate(handle, 90.0f, 90.0f, 0.0f);
        else if (back == "west") Rotate(handle, 90.0f, 270.0f, 0.0f);
        else if (back == "north") Rotate(handle, 90.0f, 0.0f, 0.0f);
        else if (back == "south") Rotate(handle, 90.0f, 180.0f, 0.0f);
        // up: identity
        Append(result, std::move(handle));
        return result;
    }

    // factory_gauge: block.json has no elements; FactoryPanelModel adds one panel partial per
    // active slot (NBT key = slot name), FactoryPanelRenderer adds a bulb when FilterAmount > 0.
    // Both: translate(slot * 0.5), rotateY(180), rotateX(xRot + 90), rotateY(yRot), centred.
    ModelData GenerateCreateFactoryGauge(const unordered_map<string, string>& states, const NbtTagPtr& nbt) {
        ModelData result;
        if (!nbt || nbt->type != TagType::COMPOUND) return result;
        auto rotateMc = [](ModelData& model, char axis, float degrees) {
            if (model.vertices.empty() || fmodf(degrees, 360.0f) == 0.0f) return;
            const float rad = degrees * (float)M_PI / 180.0f;
            const float c = roundf(cosf(rad) * 1e6f) / 1e6f, s = roundf(sinf(rad) * 1e6f) / 1e6f;
            for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
                float vx = model.vertices[i] - 0.5f, vy = model.vertices[i + 1] - 0.5f, vz = model.vertices[i + 2] - 0.5f;
                float nx = vx, ny = vy, nz = vz;
                if (axis == 'x') { ny = vy * c - vz * s; nz = vy * s + vz * c; }
                else if (axis == 'y') { nx = vx * c + vz * s; nz = -vx * s + vz * c; }
                model.vertices[i] = nx + 0.5f; model.vertices[i + 1] = ny + 0.5f; model.vertices[i + 2] = nz + 0.5f;
            }
        };
        const string face = states.contains("face") ? states.at("face") : "floor";
        const string facing = states.contains("facing") ? states.at("facing") : "south";
        const float xRot = face == "ceiling" ? 90.0f : face == "floor" ? -90.0f : 0.0f;
        // AngleHelper.horizontalAngle: toYRot, negated on the X axis.
        float yRot = facing == "west" ? -90.0f : facing == "north" ? 180.0f : facing == "east" ? -270.0f : 0.0f;
        if (face == "ceiling") yRot += 180.0f;
        const bool restocker = getChildByName(nbt, "Promises") != nullptr; // only saved for restockers
        auto listSize = [](const NbtTagPtr& tag) {
            return tag && tag->type == TagType::LIST ? tag->children.size() : size_t(0);
        };
        static const struct { const char* name; int xOffset, yOffset; } slots[] = {
            {"top_left", 1, 1}, {"top_right", 0, 1}, {"bottom_left", 1, 0}, {"bottom_right", 0, 0}};
        for (const auto& slot : slots) {
            NbtTagPtr panel = getChildByName(nbt, slot.name);
            if (!panel || panel->type != TagType::COMPOUND || panel->children.empty()) continue;
            auto amountTag = getChildByName(panel, "FilterAmount");
            int count = amountTag && amountTag->type == TagType::INT ? bytesToInt(amountTag->payload) : 0;
            string partial = string("block/factory_gauge/panel") + (restocker ? "_restocker" : "") + (count == 0 ? "" : "_with_bulb");
            ModelData piece = LoadCreateModel(partial, 260);
            if (count > 0) {
                bool linked = listSize(getChildByName(panel, "TargetedBy")) > 0 || restocker;
                bool missingAddress = linked && NbtString(panel, "RecipeAddress").empty();
                Append(piece, LoadCreateModel(NbtBool(panel, "RedstonePowered") || missingAddress ?
                    "block/factory_gauge/bulb_red" : "block/factory_gauge/bulb_light", 261));
            }
            for (size_t i = 0; i + 2 < piece.vertices.size(); i += 3) {
                piece.vertices[i] += slot.xOffset * 0.5f;
                piece.vertices[i + 2] += slot.yOffset * 0.5f;
            }
            rotateMc(piece, 'y', 180.0f);
            rotateMc(piece, 'x', xRot + 90.0f);
            rotateMc(piece, 'y', yRot);
            Append(result, std::move(piece));
        }
        DisableCulling(result);
        return result;
    }

}

bool SpecialBlock::IsObjModelJson(const nlohmann::json& modelJson) {
    auto isObj = [&](const char* key) {
        if (!modelJson.contains(key) || !modelJson[key].is_string()) return false;
        const string loader = modelJson[key].get<string>();
        return loader.size() >= 4 && loader.compare(loader.size() - 4, 4, ":obj") == 0;
    };
    return (isObj("loader") || isObj("porting_lib:loader")) && modelJson.contains("model");
}

ModelData SpecialBlock::GenerateObjModel(const nlohmann::json& modelJson) {
    return LoadObjFromModelJson(modelJson);
}

bool SpecialBlock::TryGenerateCreateBlockModel(const string& blockName,
    int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel) {
    string baseName = BaseBlockName(blockName);
    if (baseName == "create:shaft" || baseName == "create:cogwheel" ||
        baseName == "create:large_cogwheel") {
        outModel = GenerateCreateKinetic(baseName.substr(7), ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:belt") {
        outModel = GenerateCreateBelt(blockName, ParseBlockStates(blockName), blockEntityNbt);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:water_wheel" || baseName == "create:large_water_wheel") {
        outModel = GenerateCreateWaterWheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z, blockEntityNbt);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_shaft" ||
        baseName == "create:brass_encased_shaft" ||
        baseName == "create:metal_girder_encased_shaft" ||
        baseName == "create:encased_chain_drive" ||
        baseName == "create:adjustable_chain_gearshift" ||
        baseName == "create:rotation_speed_controller") {
        outModel = GenerateCreateShaftedBlock(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_cogwheel" ||
        baseName == "create:brass_encased_cogwheel" ||
        baseName == "create:andesite_encased_large_cogwheel" ||
        baseName == "create:brass_encased_large_cogwheel") {
        outModel = GenerateCreateEncasedCogwheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    // Gearbox casing is in the blockstate; protruding shafts are Flywheel SHAFT_HALF partials.
    if (baseName == "create:gearbox") {
        outModel = GenerateCreateGearbox(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    // piston-shaft-6.0.8: MechanicalPistonRenderer always draws create:shaft on
    // DirectionalAxisKineticBlock.getRotationAxis, for retracted, extended and moving
    // (the kinetic BE exists in every state). Reuse GenerateCreateShaftedBlock; the
    // shaft is 90°-symmetric and that path does not apply the 22.5° position offset.
    if (baseName == "create:mechanical_piston" ||
        baseName == "create:sticky_mechanical_piston") {
        auto states = ParseBlockStates(blockName);
        const string facing = states.contains("facing") ? states.at("facing") : "north";
        const bool alongFirst = states.contains("axis_along_first") &&
            states.at("axis_along_first") == "true";
        // facing X → alongFirst ? Y : Z; facing Y → X : Z; facing Z → X : Y.
        string axis = "y";
        if (facing == "east" || facing == "west")
            axis = alongFirst ? "y" : "z";
        else if (facing == "up" || facing == "down")
            axis = alongFirst ? "x" : "z";
        else
            axis = alongFirst ? "x" : "y";
        states["axis"] = axis;
        outModel = GenerateCreateShaftedBlock("mechanical_piston", blockName, states);
        return !outModel.vertices.empty();
    }
    // Clutch / gearshift / sequenced_gearshift: SplitShaftRenderer places SHAFT_HALF on the axis.
    if (baseName == "create:clutch" || baseName == "create:gearshift" ||
        baseName == "create:sequenced_gearshift") {
        outModel = GenerateCreateSplitShaft(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:fluid_valve") {
        outModel = GenerateCreateFluidValve(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:sticker") {
        outModel = GenerateCreateSticker(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:copper_backtank" || baseName == "create:netherite_backtank") {
        outModel = GenerateCreateBacktank(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:steam_whistle") {
        outModel = GenerateCreateSteamWhistle(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:flywheel" || baseName == "create:crushing_wheel" ||
        baseName == "create:blaze_burner") {
        outModel = GenerateCreateObjKinetic(baseName.substr(7), blockName,
            ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:steam_engine") {
        outModel = GenerateCreateSteamEngine(blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:fluid_pipe" || baseName == "create:glass_fluid_pipe" ||
        baseName == "create:encased_fluid_pipe") {
        outModel = GenerateCreateFluidPipe(blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_harvester" || baseName == "create:mechanical_roller") {
        outModel = GenerateCreateHarvesterRoller(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_mixer") {
        outModel = GenerateCreateMixer(blockName);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:speedometer" || baseName == "create:stressometer") {
        outModel = GenerateCreateGauge(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:analog_lever") {
        outModel = GenerateCreateAnalogLever(blockName, ParseBlockStates(blockName), blockEntityNbt);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:cuckoo_clock" || baseName == "create:mysterious_cuckoo_clock") {
        outModel = GenerateCreateCuckooClock(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_press") {
        outModel = GenerateCreatePress(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:encased_fan") {
        outModel = GenerateCreateEncasedFan(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_drill") {
        outModel = GenerateCreateDrill(blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_saw") {
        outModel = GenerateCreateSaw(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_bearing" || baseName == "create:windmill_bearing" ||
        baseName == "create:clockwork_bearing") {
        outModel = GenerateCreateBearing(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:deployer") {
        outModel = GenerateCreateDeployer(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_pump" || baseName == "create:millstone") {
        outModel = GenerateCreateCogPartial(baseName.substr(7), blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:mechanical_arm") {
        outModel = GenerateCreateMechanicalArm(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:gantry_carriage") {
        outModel = GenerateCreateGantryCarriage(blockName, ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:hand_crank") {
        outModel = GenerateCreateHandCrank(blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:factory_gauge") {
        outModel = GenerateCreateFactoryGauge(ParseBlockStates(blockName), blockEntityNbt);
        return !outModel.vertices.empty();
    }
    {
        // create:<color>_toolbox
        size_t colon = baseName.find(':');
        string local = colon == string::npos ? baseName : baseName.substr(colon + 1);
        if (local.size() > 8 && local.compare(local.size() - 8, 8, "_toolbox") == 0) {
            outModel = GenerateCreateToolbox(blockName, ParseBlockStates(blockName));
            return !outModel.vertices.empty();
        }
    }
    // nixie_tube: porting_lib:composite — flattened in ProcessModelData; no SpecialBlock needed.
    // Colored variants share the same composite parent.
    return false;
}

ModelData SpecialBlock::GenerateSpecialBlockModel(const string& blockName) {
    // 末地传送门/折跃门(着色器渲染, 方块模型没有 elements)
    {
        ModelData ep = GenerateEndPortalModel(blockName);
        if (!ep.faces.empty()) return ep;
    }
    string texturePath;
    int waterLevel;
    bool waterFalling;

    if (IsLightBlock(blockName, texturePath)&&config.exportLightBlock) {
        return GenerateLightBlockModel(texturePath);
    }

    // 头颅/头(实体渲染方块, 方块模型没有 elements)
    {
        ModelData skull = GenerateSkullModel(blockName);
        if (!skull.faces.empty()) return skull;
    }

    // 检查床方块 (block ID 以 _bed 结尾)
    {
        string testName = blockName;
        size_t colonPos = testName.find(':');
        if (colonPos != string::npos) {
            testName = testName.substr(colonPos + 1);
        }
        size_t bracketPos = testName.find('[');
        string blockID = testName.substr(0, bracketPos);
        if (blockID.size() > 4 &&
            blockID.compare(blockID.size() - 4, 4, "_bed") == 0) {
            return GenerateBedModel(blockName);
        }
    }

    // 箱子(实体渲染方块, 方块模型没有 elements)
    {
        ModelData chest = GenerateChestModel(blockName);
        if (!chest.faces.empty()) return chest;
    }

    // 原版实体渲染方块(饰纹陶罐/潮涌核心/钟/潜影盒)
    {
        ModelData be = GenerateBlockEntityModel(blockName);
        if (!be.faces.empty()) return be;
    }

    // 告示牌(旧版本没有方块模型, 26.x 自带模型不会走到这里)
    {
        ModelData sign = GenerateSignModel(blockName);
        if (!sign.faces.empty()) return sign;
    }

    // 铜傀儡雕像(方块实体渲染)
    {
        ModelData statue = GenerateCopperGolemStatueModel(blockName);
        if (!statue.faces.empty()) return statue;
    }

    // 其他类型方块的生成逻辑
    ModelData defaultModel;
    return defaultModel;
}


bool SpecialBlock::IsLightBlock(const string& blockName, string& outTexturePath) {
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    string ns = "minecraft"; // 默认命名空间
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }

    // 过滤非minecraft命名空间
    if (ns != "minecraft") return false;

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 检查是否为光源方块
    if (blockID != "light") return false;

    // 提取光照等级
    string level = "15"; // 默认亮度
    if (bracketPos != string::npos) {
        size_t equalsPos = processed.find('=', bracketPos);
        size_t endPos = processed.find(']', equalsPos);
        if (equalsPos != string::npos && endPos != string::npos) {
            level = processed.substr(equalsPos + 1, endPos - equalsPos - 1);
        }
    }

    // 格式化为两位数
    if (level.length() == 1) level = "0" + level;

    // 构建材质路径
    outTexturePath = ns + ":block/light_block_" + level;
    return true;
}

ModelData SpecialBlock::GenerateLightBlockModel(const string& texturePath) {
    ModelData cubeModel;
    float halfSize = config.lightBlockSize;

    cubeModel.vertices = {
        // 前面 (front)
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        // 后面 (back)
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        // 上面 (top)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        // 下面 (bottom)
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 左面 (left)
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 右面 (right)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize
    };

    // 设置 UV 坐标
    cubeModel.uvCoordinates = {
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f
    };

    // 创建材质（旧版显示名：minecraft:block/light_block_<level>）
    Material material;
    material.name = texturePath;
    material.texturePath = "None";
    material.tintIndex = -1;  // 设置默认tint索引
    cubeModel.materials = { material };

    // 使用Face结构体创建六个面
    cubeModel.faces.resize(6);
    
    // 前面
    cubeModel.faces[0].vertexIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].uvIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].materialIndex = 0;
    cubeModel.faces[0].faceDirection = FaceType::DO_NOT_CULL;
    
    // 后面
    cubeModel.faces[1].vertexIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].uvIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].materialIndex = 0;
    cubeModel.faces[1].faceDirection = FaceType::DO_NOT_CULL;
    
    // 上面
    cubeModel.faces[2].vertexIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].uvIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].materialIndex = 0;
    cubeModel.faces[2].faceDirection = FaceType::DO_NOT_CULL;
    
    // 下面
    cubeModel.faces[3].vertexIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].uvIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].materialIndex = 0;
    cubeModel.faces[3].faceDirection = FaceType::DO_NOT_CULL;
    
    // 左面
    cubeModel.faces[4].vertexIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].uvIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].materialIndex = 0;
    cubeModel.faces[4].faceDirection = FaceType::DO_NOT_CULL;
    
    // 右面
    cubeModel.faces[5].vertexIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].uvIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].materialIndex = 0;
    cubeModel.faces[5].faceDirection = FaceType::DO_NOT_CULL;

    return cubeModel;
}

ModelData SpecialBlock::GenerateBedModel(const string& blockName) {
    // 解析方块名称: "minecraft:red_bed[facing=east,part=head]"
    string ns = "minecraft";
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }
    if (ns != "minecraft") {
        return ModelData();
    }

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 从方块ID中提取颜色 (移除 "_bed" 后缀)
    string color = blockID;
    size_t bedPos = color.rfind("_bed");
    if (bedPos != string::npos) {
        color = color.substr(0, bedPos);
    }

    // 解析状态属性
    string part = "head";
    string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart);
        string pair;
        while (getline(ss, pair, ',')) {
            size_t eqPos = pair.find('=');
            if (eqPos != string::npos) {
                string key = pair.substr(0, eqPos);
                string value = pair.substr(eqPos + 1);
                if (key == "part") part = value;
                else if (key == "facing") facing = value;
            }
        }
    }

    // 材质名以方块 id 为主（如 minecraft:red_bed）；贴图仍用实体床贴图
    string textureName = ns + ":" + blockID;
    string textureFileDir = "textures";
    string savedTexturePath;
    if (!SaveTextureToFile(ns, "entity/bed/" + color, textureFileDir)) {
        return ModelData(); // 纹理不存在,返回空模型
    }
    savedTexturePath = "textures/" + ns + "/entity/bed/" + color + ".png";
    RegisterTexture(ns, "entity/bed/" + color, savedTexturePath);

    ModelData model;

    // 辅助lambda: 添加一个面 (顶点使用床本地坐标系 -0.5~0.5, 自动转换到 0~1)
    auto addFace = [&](float v0x, float v0y, float v0z,
                        float v1x, float v1y, float v1z,
                        float v2x, float v2y, float v2z,
                        float v3x, float v3y, float v3z,
                        float u0, float v0,
                        float u1, float v1,
                        float u2, float v2,
                        float u3, float v3) {
        int vertexStart = (int)model.vertices.size() / 3;
        int uvStart = (int)model.uvCoordinates.size() / 2;

        // 从床本地坐标系 (-0.5~0.5) 转换到方块坐标系 (0~1)
        model.vertices.push_back(v0x + 0.5f);
        model.vertices.push_back(v0y + 0.5f);
        model.vertices.push_back(v0z + 0.5f);
        model.vertices.push_back(v1x + 0.5f);
        model.vertices.push_back(v1y + 0.5f);
        model.vertices.push_back(v1z + 0.5f);
        model.vertices.push_back(v2x + 0.5f);
        model.vertices.push_back(v2y + 0.5f);
        model.vertices.push_back(v2z + 0.5f);
        model.vertices.push_back(v3x + 0.5f);
        model.vertices.push_back(v3y + 0.5f);
        model.vertices.push_back(v3z + 0.5f);

        model.uvCoordinates.push_back(u0);
        model.uvCoordinates.push_back(v0);
        model.uvCoordinates.push_back(u1);
        model.uvCoordinates.push_back(v1);
        model.uvCoordinates.push_back(u2);
        model.uvCoordinates.push_back(v2);
        model.uvCoordinates.push_back(u3);
        model.uvCoordinates.push_back(v3);

        Face face;
        face.vertexIndices = {vertexStart, vertexStart + 1, vertexStart + 2, vertexStart + 3};
        face.uvIndices = {uvStart, uvStart + 1, uvStart + 2, uvStart + 3};
        face.materialIndex = 0;
        face.faceDirection = DO_NOT_CULL;
        model.faces.push_back(face);
    };

    // 设置材质
    Material material;
    material.name = textureName;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    if (part == "head")
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f, 22/64.0f, 58/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 42/64.0f, 28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 44/64.0f, 58/64.0f);
        // 前面 (床头板正面, z=-0.5), jmc2obj 的 UV 是非顺序分配 (uv[2],uv[3],uv[0],uv[1])
        addFace( 0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,  -0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f, -0.5f,
                6/64.0f, 64/64.0f, 22/64.0f, 64/64.0f, 22/64.0f, 58/64.0f, 6/64.0f, 58/64.0f);
        // 左面 (床头板左侧, x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 58/64.0f,  0/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f);
        // 右面 (床头板右侧, x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 22/64.0f, 58/64.0f, 22/64.0f, 42/64.0f);

        // --- 左床腿 (床头端, z=-0.5~-0.3125, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,    -0.5f, -0.3125f, -0.3125f,  -0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace(-0.5f, -0.3125f, -0.3125f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床头端, z=-0.5~-0.3125, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.5f, -0.3125f, -0.3125f,   0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f, -0.5f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.3125f, -0.3125f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace( 0.5f, -0.3125f, -0.3125f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }
    else // foot
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f, 22/64.0f, 36/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 20/64.0f, 28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 44/64.0f, 36/64.0f);
        // 左面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 36/64.0f,  0/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f);
        // 右面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 22/64.0f, 36/64.0f, 22/64.0f, 20/64.0f);
        // 后面 (床尾板, z=0.5)
        addFace( 0.5f, -0.3125f, 0.5f,  -0.5f, -0.3125f, 0.5f,  -0.5f, 0.0625f, 0.5f,   0.5f, 0.0625f, 0.5f,
                38/64.0f, 42/64.0f, 22/64.0f, 42/64.0f, 22/64.0f, 36/64.0f, 38/64.0f, 36/64.0f);

        // --- 左床腿 (床尾端, z=0.3125~0.5, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f,  0.5f,    -0.5f, -0.3125f, 0.3125f,  -0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace(-0.5f, -0.3125f,  0.5f,  -0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace(-0.5f, -0.3125f, 0.3125f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床尾端, z=0.3125~0.5, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,     0.5f, -0.3125f, 0.3125f,   0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f,  0.5f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.3125f, -0.3125f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace( 0.5f, -0.3125f, 0.3125f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }

    // 根据朝向应用旋转 (使用方块坐标系 0~1, 以 0.5,0.5 为中心)
    int rotY = 0;
    if (facing == "south") rotY = 180;
    else if (facing == "west") rotY = 270;
    else if (facing == "east") rotY = 90;

    if (rotY != 0) {
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    }

    return model;
}

// 箱子: 按原版 ChestModel (1.15+ 贴图布局, 64x64) 生成 bottom/lid/lock 三个盒子。
// 原版箱子渲染不翻转 y, 盒子坐标即方块像素坐标; 默认朝南, 按 facing 旋转。
ModelData SpecialBlock::GenerateChestModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);
    string id = blockID;
    if (id.rfind("waxed_", 0) == 0) id = id.substr(6);

    string tex;
    if (id == "chest") tex = "normal";
    else if (id == "trapped_chest") tex = "trapped";
    else if (id == "ender_chest") tex = "ender";
    else if (id == "copper_chest") tex = "copper";
    else if (id == "exposed_copper_chest") tex = "copper_exposed";
    else if (id == "weathered_copper_chest") tex = "copper_weathered";
    else if (id == "oxidized_copper_chest") tex = "copper_oxidized";
    else return ModelData();

    string facing = "north", type = "single";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart); string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "facing") facing = v; else if (k == "type") type = v;
        }
    }
    if (id == "ender_chest") type = "single";
    if (type == "left" || type == "right") tex += "_" + type;

    string texPath = "entity/chest/" + tex;
    string textureFileDir = "textures";
    if (!SaveTextureToFile(ns, texPath, textureFileDir)) return ModelData();
    string savedTexturePath = "textures/" + ns + "/" + texPath + ".png";
    RegisterTexture(ns, texPath, savedTexturePath);

    ModelData model;
    Material material;
    // 左右半箱贴图不同, 材质名必须区分, 否则按名字去重后都会用到同一张贴图
    material.name = ns + ":" + blockID;
    if (type == "left" || type == "right") material.name += "#" + type + "~" + tex;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    const float T = 64.0f;
    // 原版 ModelPart.Cube: 顶点 0..7 与 6 个面的 UV 分配
    auto addBox = [&](float x, float y, float z, float dx, float dy, float dz, float u, float v) {
        float x2 = x + dx, y2 = y + dy, z2 = z + dz;
        float P[8][3] = { {x,y,z},{x2,y,z},{x2,y2,z},{x,y2,z},{x,y,z2},{x2,y,z2},{x2,y2,z2},{x,y2,z2} };
        float f4 = u + dz, f5 = u + dz + dx, f6 = u + dz + dx + dx, f10 = u + dz + dx + dz, f11 = u + dz + dx + dz + dx;
        float f7 = v, f8 = v + dz, f9 = v + dz + dy;
        // {四个顶点, u1, v1, u2, v2}; 原版 Polygon: v[0]->(u2,v1) v[1]->(u1,v1) v[2]->(u1,v2) v[3]->(u2,v2)
        struct Q { int a, b, c, d; float u1, v1, u2, v2; };
        Q qs[6] = {
            {5, 1, 2, 6, f5, f8, f10, f9},  // east  (+x)
            {0, 4, 7, 3, u,  f8, f4,  f9},  // west  (-x)
            {5, 4, 0, 1, f4, f7, f5,  f8},  // down  (-y)
            {2, 3, 7, 6, f5, f8, f6,  f7},  // up    (+y)
            {1, 0, 3, 2, f4, f8, f5,  f9},  // north (-z)
            {4, 5, 6, 7, f10, f8, f11, f9}, // south (+z)
        };
        for (const Q& q : qs) {
            int vs = (int)model.vertices.size() / 3, us = (int)model.uvCoordinates.size() / 2;
            int idx[4] = { q.a, q.b, q.c, q.d };
            float uv[4][2] = { {q.u2, q.v1}, {q.u1, q.v1}, {q.u1, q.v2}, {q.u2, q.v2} };
            // 原版顶点绕序在 y 向上的方块空间里是反的, 逆序写入使法线朝外
            for (int i = 3; i >= 0; --i) {
                for (int k = 0; k < 3; ++k) model.vertices.push_back(P[idx[i]][k] / 16.0f);
                model.uvCoordinates.push_back(uv[i][0] / T);
                model.uvCoordinates.push_back(1.0f - uv[i][1] / T);
            }
            Face face;
            face.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            face.uvIndices = { us, us + 1, us + 2, us + 3 };
            face.materialIndex = 0;
            face.faceDirection = DO_NOT_CULL;
            model.faces.push_back(face);
        }
    };

    if (type == "left") {
        addBox(0, 0, 1, 15, 10, 14, 0, 19);
        addBox(0, 9, 1, 15, 5, 14, 0, 0);
        addBox(0, 7, 15, 1, 4, 1, 0, 0);
    } else if (type == "right") {
        addBox(1, 0, 1, 15, 10, 14, 0, 19);
        addBox(1, 9, 1, 15, 5, 14, 0, 0);
        addBox(15, 7, 15, 1, 4, 1, 0, 0);
    } else {
        addBox(1, 0, 1, 14, 10, 14, 0, 19);
        addBox(1, 9, 1, 14, 5, 14, 0, 0);
        addBox(7, 7, 15, 2, 4, 1, 0, 0);
    }

    int rotY = 0;  // 模型默认朝南
    if (facing == "west") rotY = 90;
    else if (facing == "north") rotY = 180;
    else if (facing == "east") rotY = 270;
    if (rotY != 0) ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    return model;
}

// 告示牌: 按原版 SignRenderer + SignModel 生成(实体贴图 entity/signs/<木材>.png, 64x32)。
// 实体模型空间 y、z 朝下/朝北, 渲染时 scale(2/3, -2/3, -2/3) 后再旋转平移到方块里。
ModelData SpecialBlock::GenerateSignModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);
    if (blockID.find("hanging") != string::npos) return ModelData();
    bool wall = false;
    string wood;
    auto endsWith = [&](const string& suf) { return blockID.size() > suf.size() && blockID.compare(blockID.size() - suf.size(), suf.size(), suf) == 0; };
    if (endsWith("_wall_sign")) { wall = true; wood = blockID.substr(0, blockID.size() - 10); }
    else if (endsWith("_sign")) { wood = blockID.substr(0, blockID.size() - 5); }
    else return ModelData();

    int rotation = 0; string facing = "south";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart); string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "rotation") { try { rotation = stoi(v); } catch (...) {} }
            else if (k == "facing") facing = v;
        }
    }

    string texPath = "entity/signs/" + wood;
    string textureFileDir = "textures";
    if (!SaveTextureToFile(ns, texPath, textureFileDir)) return ModelData();
    string savedTexturePath = "textures/" + ns + "/" + texPath + ".png";
    RegisterTexture(ns, texPath, savedTexturePath);

    ModelData model;
    Material material;
    material.name = ns + ":" + blockID;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    float yRot;
    if (wall) {
        float f = 0;
        if (facing == "west") f = 90; else if (facing == "north") f = 180; else if (facing == "east") f = 270;
        yRot = -f;
    } else {
        yRot = -rotation * 22.5f;
    }
    const float rad = yRot * 3.14159265358979f / 180.0f, c = cos(rad), sn = sin(rad), k = 2.0f / 3.0f / 16.0f;
    auto xf = [&](float px, float py, float pz, float out[3]) {
        float x = px * k, y = -py * k, z = -pz * k;
        if (wall) { y -= 0.3125f; z -= 0.4375f; }
        out[0] = x * c + z * sn + 0.5f;
        out[1] = y + 0.5f;
        out[2] = -x * sn + z * c + 0.5f;
    };
    const float TW = 64.0f, TH = 32.0f;
    auto addBox = [&](float x, float y, float z, float dx, float dy, float dz, float u, float v) {
        float x2 = x + dx, y2 = y + dy, z2 = z + dz;
        float P[8][3] = { {x,y,z},{x2,y,z},{x2,y2,z},{x,y2,z},{x,y,z2},{x2,y,z2},{x2,y2,z2},{x,y2,z2} };
        float W[8][3];
        for (int i = 0; i < 8; ++i) xf(P[i][0], P[i][1], P[i][2], W[i]);
        float C[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; ++i) for (int a = 0; a < 3; ++a) C[a] += W[i][a] / 8.0f;
        float f4 = u + dz, f5 = u + dz + dx, f6 = u + dz + dx + dx, f10 = u + dz + dx + dz, f11 = u + dz + dx + dz + dx;
        float f7 = v, f8 = v + dz, f9 = v + dz + dy;
        struct Q { int a, b, c, d; float u1, v1, u2, v2; };
        Q qs[6] = {
            {5, 1, 2, 6, f5, f8, f10, f9}, {0, 4, 7, 3, u, f8, f4, f9},
            {5, 4, 0, 1, f4, f7, f5, f8},  {2, 3, 7, 6, f5, f8, f6, f7},
            {1, 0, 3, 2, f4, f8, f5, f9},  {4, 5, 6, 7, f10, f8, f11, f9},
        };
        for (const Q& q : qs) {
            int idx[4] = { q.a, q.b, q.c, q.d };
            float uv[4][2] = { {q.u2, q.v1}, {q.u1, q.v1}, {q.u1, q.v2}, {q.u2, q.v2} };
            // 绕序: 让法线朝外(远离盒子中心)
            float e1[3], e2[3], n[3], fc[3] = { 0, 0, 0 };
            for (int a = 0; a < 3; ++a) {
                e1[a] = W[idx[1]][a] - W[idx[0]][a]; e2[a] = W[idx[2]][a] - W[idx[0]][a];
                for (int i = 0; i < 4; ++i) fc[a] += W[idx[i]][a] / 4.0f;
            }
            n[0] = e1[1] * e2[2] - e1[2] * e2[1]; n[1] = e1[2] * e2[0] - e1[0] * e2[2]; n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float d = n[0] * (fc[0] - C[0]) + n[1] * (fc[1] - C[1]) + n[2] * (fc[2] - C[2]);
            int order[4] = { 0, 1, 2, 3 };
            if (d < 0) { order[0] = 3; order[1] = 2; order[2] = 1; order[3] = 0; }
            int vs = (int)model.vertices.size() / 3, us = (int)model.uvCoordinates.size() / 2;
            for (int j = 0; j < 4; ++j) {
                int i = order[j];
                for (int a = 0; a < 3; ++a) model.vertices.push_back(W[idx[i]][a]);
                model.uvCoordinates.push_back(uv[i][0] / TW);
                model.uvCoordinates.push_back(1.0f - uv[i][1] / TH);
            }
            Face face;
            face.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            face.uvIndices = { us, us + 1, us + 2, us + 3 };
            face.materialIndex = 0;
            face.faceDirection = DO_NOT_CULL;
            model.faces.push_back(face);
        }
    };
    addBox(-12, -14, -1, 24, 12, 2, 0, 0);       // 牌面
    if (!wall) addBox(-1, -2, -1, 2, 14, 2, 0, 14); // 立杆
    return model;
}

// ---------------------------------------------------------------------------
// 原版实体渲染方块(方块模型没有 elements, 由 BlockEntityRenderer 绘制):
// 饰纹陶罐 / 潮涌核心 / 钟 / 潜影盒。几何按原版 ModelPart.Cube + PartPose 移植,
// 变换按各自 Renderer 的 PoseStack 链计算, 单位: 方块(0..1)。
namespace {
    struct Aff {  // 3x4 仿射矩阵
        float m[3][4] = { {1,0,0,0},{0,1,0,0},{0,0,1,0} };
        Aff operator*(const Aff& o) const {
            Aff r;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) {
                float s = (j == 3) ? m[i][3] : 0.0f;
                for (int k = 0; k < 3; ++k) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
            return r;
        }
        void Apply(const float p[3], float out[3], bool dir = false) const {
            for (int i = 0; i < 3; ++i)
                out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + (dir ? 0.0f : m[i][3]);
        }
    };
    Aff AffT(float x, float y, float z) { Aff a; a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z; return a; }
    Aff AffS(float x, float y, float z) { Aff a; a.m[0][0] = x; a.m[1][1] = y; a.m[2][2] = z; return a; }
    Aff AffRX(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c; return a; }
    Aff AffRY(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c; return a; }
    Aff AffRZ(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c; return a; }
    // ModelPart.translateAndRotate: translate(xyz/16) * rotationZYX(z, y, x)
    Aff PartPose(float x, float y, float z, float rx = 0, float ry = 0, float rz = 0) {
        return AffT(x / 16.0f, y / 16.0f, z / 16.0f) * AffRZ(rz) * AffRY(ry) * AffRX(rx);
    }
    enum : unsigned { F_DOWN = 1, F_UP = 2, F_WEST = 4, F_NORTH = 8, F_EAST = 16, F_SOUTH = 32, F_ALL = 63 };

    // 原版 ModelPart.Cube 构造(顶点 / UV / 面), pose 为该部件到方块空间的变换(输入像素坐标/16)
    void AddEntityCube(ModelData& model, int mat, const Aff& pose, int texU, int texV,
                       float x, float y, float z, float w, float h, float d, float grow,
                       float texW, float texH, unsigned faces = F_ALL) {
        float x0 = x - grow, y0 = y - grow, z0 = z - grow, x1 = x + w + grow, y1 = y + h + grow, z1 = z + d + grow;
        const float P[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},   // t0..t3
                                {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} }; // l0..l3
        float u0 = (float)texU, u1 = texU + d, u2 = texU + d + w, u22 = texU + d + w + w, u3 = texU + d + w + d, u4 = texU + d + w + d + w;
        float v0 = (float)texV, v1 = texV + d, v2 = texV + d + h;
        struct Q { unsigned bit; int a, b, c, e; float ua, va, ub, vb; float n[3]; };
        const Q qs[6] = {
            {F_DOWN,  5, 4, 0, 1, u1, v0, u2, v1,  {0,-1,0}},
            {F_UP,    2, 3, 7, 6, u2, v1, u22, v0, {0,1,0}},
            {F_WEST,  0, 4, 7, 3, u0, v1, u1, v2,  {-1,0,0}},
            {F_NORTH, 1, 0, 3, 2, u1, v1, u2, v2,  {0,0,-1}},
            {F_EAST,  5, 1, 2, 6, u2, v1, u3, v2,  {1,0,0}},
            {F_SOUTH, 4, 5, 6, 7, u3, v1, u4, v2,  {0,0,1}},
        };
        for (const Q& q : qs) {
            if (!(faces & q.bit)) continue;
            int idx[4] = { q.a, q.b, q.c, q.e };
            // Polygon: v0->(u1,v0) v1->(u0,v0) v2->(u0,v1) v3->(u1,v1)
            float uv[4][2] = { {q.ub, q.va}, {q.ua, q.va}, {q.ua, q.vb}, {q.ub, q.vb} };
            float W[4][3];
            for (int i = 0; i < 4; ++i) {
                float p[3] = { P[idx[i]][0] / 16.0f, P[idx[i]][1] / 16.0f, P[idx[i]][2] / 16.0f };
                pose.Apply(p, W[i]);
            }
            // 零面积面(零厚度平面的侧面)跳过
            float e1[3], e2[3], cr[3];
            for (int k = 0; k < 3; ++k) { e1[k] = W[1][k] - W[0][k]; e2[k] = W[2][k] - W[0][k]; }
            cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
            if (fabsf(cr[0]) + fabsf(cr[1]) + fabsf(cr[2]) < 1e-9f) {
                for (int k = 0; k < 3; ++k) e2[k] = W[3][k] - W[0][k];
                cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
                if (fabsf(cr[0]) + fabsf(cr[1]) + fabsf(cr[2]) < 1e-9f) continue;
            }
            // 绕序与变换后的面法线一致(变换可能含镜像)
            float n[3]; pose.Apply(q.n, n, true);
            bool flip = cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < 0.0f;
            int vs = (int)model.vertices.size() / 3, us = (int)model.uvCoordinates.size() / 2;
            for (int j = 0; j < 4; ++j) {
                int i = flip ? 3 - j : j;
                for (int k = 0; k < 3; ++k) model.vertices.push_back(W[i][k]);
                model.uvCoordinates.push_back(uv[i][0] / texW);
                model.uvCoordinates.push_back(1.0f - uv[i][1] / texH);
            }
            Face face;
            face.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            face.uvIndices = { us, us + 1, us + 2, us + 3 };
            face.materialIndex = mat;
            face.faceDirection = DO_NOT_CULL;
            model.faces.push_back(face);
        }
    }

    // 注册实体贴图并返回材质下标; 失败返回 -1
    int AddEntityMaterial(ModelData& model, const string& ns, const string& blockID, const string& texPath, const string& key) {
        string dir = "textures";
        if (!SaveTextureToFile(ns, texPath, dir)) return -1;
        string saved = "textures/" + ns + "/" + texPath + ".png";
        RegisterTexture(ns, texPath, saved);
        Material m;
        string base = texPath.substr(texPath.rfind('/') + 1);
        m.name = ns + ":" + blockID + (key.empty() ? "" : "#" + key + "~" + base);
        m.texturePath = saved;
        m.tintIndex = -1;
        model.materials.push_back(m);
        return (int)model.materials.size() - 1;
    }

    float DirToYRot(const string& f) {  // Direction.toYRot
        if (f == "south") return 0.0f;
        if (f == "west") return 90.0f;
        if (f == "north") return 180.0f;
        if (f == "east") return 270.0f;
        return 0.0f;
    }
    const float kPi = 3.14159265358979f;
    // DecoratedPotRenderer: 侧面按 PotDecorations.ordered() = back, left, right, front 取图案
    ModelData DecoratedPotModel(const string& ns, const string& blockID, const string& facing, const std::vector<string>& sherds) {
        ModelData model;
        // rotateAround(Y, 180 - facing.toYRot(), 0.5, 0.5, 0.5)
        float deg = 180.0f - DirToYRot(facing);
        Aff root = AffT(0.5f, 0.5f, 0.5f) * AffRY(deg * kPi / 180.0f) * AffT(-0.5f, -0.5f, -0.5f);
        int base = AddEntityMaterial(model, ns, blockID, "entity/decorated_pot/decorated_pot_base", "base");
        int side = AddEntityMaterial(model, ns, blockID, "entity/decorated_pot/decorated_pot_side", "side");
        if (base < 0 || side < 0) return ModelData();
        Aff neck = root * PartPose(0, 37, 16, kPi, 0, 0);
        AddEntityCube(model, base, neck, 0, 0, 4, 17, 4, 8, 3, 8, -0.1f, 32, 32);
        AddEntityCube(model, base, neck, 0, 5, 5, 20, 5, 6, 1, 6, 0.2f, 32, 32);
        AddEntityCube(model, base, root * PartPose(1, 16, 1), -14, 13, 0, 0, 0, 14, 0, 14, 0, 32, 32);
        AddEntityCube(model, base, root * PartPose(1, 0, 1), -14, 13, 0, 0, 0, 14, 0, 14, 0, 32, 32);
        const float sides[4][6] = { {15, 16, 1, 0, 0, kPi}, {1, 16, 1, 0, -kPi / 2, kPi},
                                    {15, 16, 15, 0, kPi / 2, kPi}, {1, 16, 15, kPi, 0, 0} };
        for (int i = 0; i < 4; ++i) {
            int mat = side;
            // DecoratedPotPatterns: <name>_pottery_sherd -> entity/decorated_pot/<name>_pottery_pattern, 其他(砖)用默认侧面
            string item = i < (int)sherds.size() ? sherds[i] : "";
            size_t c = item.find(':');
            string ins = c == string::npos ? "minecraft" : item.substr(0, c), id = c == string::npos ? item : item.substr(c + 1);
            const string suf = "_pottery_sherd";
            if (id.size() > suf.size() && id.compare(id.size() - suf.size(), suf.size(), suf) == 0) {
                int m = AddEntityMaterial(model, ins, blockID,
                    "entity/decorated_pot/" + id.substr(0, id.size() - suf.size()) + "_pottery_pattern", "side");
                if (m >= 0) mat = m;
            }
            const auto& s = sides[i];
            AddEntityCube(model, mat, root * PartPose(s[0], s[1], s[2], s[3], s[4], s[5]), 1, 0, 0, 0, 0, 14, 16, 0, 0, 16, 16, F_NORTH);
        }
        return model;
    }
}

std::vector<string> SpecialBlock::ReadPotSherds(const NbtTagPtr& blockEntityNbt) {
    // 方块实体 sherds(back, left, right, front), 缺省为砖:
    // 1.20-1.21 为物品 id 列表; 26.1+ 为 {back/left/right/front: 物品} 的 compound
    std::vector<string> sherds;
    NbtTagPtr tag = blockEntityNbt ? getChildByName(blockEntityNbt, "sherds") : nullptr;
    auto itemId = [](const NbtTagPtr& t) -> string {
        if (!t) return "";
        if (t->type == TagType::STRING) return getStringTag(t);
        if (t->type == TagType::COMPOUND) {
            NbtTagPtr id = getChildByName(t, "id");
            if (id && id->type == TagType::STRING) return getStringTag(id);
        }
        return "";
    };
    if (tag && tag->type == TagType::LIST)
        for (const auto& t : tag->children) sherds.push_back(itemId(t));
    else if (tag && tag->type == TagType::COMPOUND)
        for (const char* k : { "back", "left", "right", "front" }) sherds.push_back(itemId(getChildByName(tag, k)));
    return sherds;
}

ModelData SpecialBlock::GenerateDecoratedPotModel(const string& blockName, const NbtTagPtr& blockEntityNbt) {
    std::vector<string> sherds = ReadPotSherds(blockEntityNbt);
    bool any = false;
    for (const auto& id : sherds) if (id.find("_pottery_sherd") != string::npos) any = true;
    if (!any) return ModelData();
    auto states = ParseBlockStates(blockName);
    auto it = states.find("facing");
    return DecoratedPotModel("minecraft", "decorated_pot", it == states.end() ? "north" : it->second, sherds);
}

ModelData SpecialBlock::GenerateBlockEntityModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    string blockID = processed.substr(0, processed.find('['));
    auto states = ParseBlockStates(blockName);
    auto state = [&](const string& k, const string& def) { auto it = states.find(k); return it == states.end() ? def : it->second; };
    ModelData model;

    if (blockID == "decorated_pot") return DecoratedPotModel(ns, blockID, state("facing", "north"), {});
    if (blockID == "conduit") {
        // ConduitRenderer(未激活): translate(0.5,0.5,0.5) 外壳 6x6x6
        int mat = AddEntityMaterial(model, ns, blockID, "entity/conduit/base", "");
        if (mat < 0) return ModelData();
        AddEntityCube(model, mat, AffT(0.5f, 0.5f, 0.5f), 0, 0, -3, -3, -3, 6, 6, 6, 0, 32, 16);
        return model;
    }
    if (blockID == "bell") {
        // BellRenderer: 钟体(方块模型只有支架), bell_body offset(8,12,8) / bell_base 相对 (-8,-12,-8)
        int mat = AddEntityMaterial(model, ns, blockID, "entity/bell/bell_body", "body");
        if (mat < 0) return ModelData();
        Aff body = PartPose(8, 12, 8);
        AddEntityCube(model, mat, body, 0, 0, -3, -6, -3, 6, 7, 6, 0, 32, 32);
        AddEntityCube(model, mat, body * PartPose(-8, -12, -8), 0, 13, 4, 4, 4, 8, 2, 8, 0, 32, 32);
        return model;
    }
    if (blockID == "shulker_box" || (blockID.size() > 12 && blockID.compare(blockID.size() - 12, 12, "_shulker_box") == 0)) {
        // ShulkerBoxRenderer: T(.5,.5,.5) S(.9995) R(facing) S(1,-1,-1) T(0,-1,0); lid/base offset(0,24,0)
        string color = blockID == "shulker_box" ? "" : "_" + blockID.substr(0, blockID.size() - 12);
        int mat = AddEntityMaterial(model, ns, blockID, "entity/shulker/shulker" + color, "");
        if (mat < 0) return ModelData();
        string f = state("facing", "up");
        Aff rot;  // Direction.getRotation()
        if (f == "down") rot = AffRX(kPi);
        else if (f == "north") rot = AffRX(kPi / 2) * AffRZ(kPi);
        else if (f == "south") rot = AffRX(kPi / 2);
        else if (f == "west") rot = AffRX(kPi / 2) * AffRZ(kPi / 2);
        else if (f == "east") rot = AffRX(kPi / 2) * AffRZ(-kPi / 2);
        Aff root = AffT(0.5f, 0.5f, 0.5f) * AffS(0.9995f, 0.9995f, 0.9995f) * rot * AffS(1, -1, -1) * AffT(0, -1, 0);
        AddEntityCube(model, mat, root * PartPose(0, 24, 0), 0, 0, -8, -16, -8, 16, 12, 16, 0, 64, 64);
        // 底座与盖子侧面共面, 盖子侧面下沿是镂空的; 底座略微内缩避免 z-fighting
        AddEntityCube(model, mat, root * PartPose(0, 24, 0), 0, 28, -8, -8, -8, 16, 8, 16, -0.02f, 64, 64);
        return model;
    }
    return ModelData();
}
