#include "CreateBlazePackager.h"
#include "block.h"
#include "GlobalCache.h"
#include "texture.h"

#include <array>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {

// ProcessModelJson cache indices. Paths differ, so an index may be reused.
constexpr int kIdxFlame = 340;
constexpr int kIdxInert = 341;
constexpr int kIdxIdle = 342;
constexpr int kIdxActive = 343;
constexpr int kIdxSuper = 344;
constexpr int kIdxSuperActive = 345;
constexpr int kIdxGoggles = 346;
constexpr int kIdxGogglesSmall = 347;
constexpr int kIdxRods = 348;
constexpr int kIdxRods2 = 349;
constexpr int kIdxHatch = 340;
constexpr int kIdxTray = 341;
constexpr int kIdxTrayDefrag = 342;
constexpr int kIdxTrainHat = 343;

constexpr float kPi = 3.14159265358979323846f;

void MarkNoCull(ModelData& model) {
    for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
}

void Append(ModelData& target, ModelData part) {
    if (part.vertices.empty()) return;
    MarkNoCull(part);
    MergeModelsDirectly(target, part);
}

string LocalId(const string& blockName) {
    size_t bracket = blockName.find('[');
    string base = bracket == string::npos ? blockName : blockName.substr(0, bracket);
    size_t colon = base.rfind(':');
    return colon == string::npos ? base : base.substr(colon + 1);
}

string BaseId(const string& blockName) {
    size_t bracket = blockName.find('[');
    return bracket == string::npos ? blockName : blockName.substr(0, bracket);
}

string StateValue(const string& blockName, const string& key) {
    size_t begin = blockName.find('[');
    size_t end = blockName.rfind(']');
    if (begin == string::npos || end == string::npos || end <= begin) return "";
    string body = blockName.substr(begin + 1, end - begin - 1);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        string pair = body.substr(pos, comma == string::npos ? string::npos : comma - pos);
        size_t sep = pair.find('=');
        if (sep == string::npos) sep = pair.find(':');
        if (sep != string::npos && pair.substr(0, sep) == key) return pair.substr(sep + 1);
        if (comma == string::npos) break;
        pos = comma + 1;
    }
    return "";
}

bool HasTag(const NbtTagPtr& nbt, const string& name) {
    return nbt && getChildByName(nbt, name);
}

// Right-handed degrees. Exact at multiples of 90 so ends stay on the block grid.
void SinCosDeg(float deg, float& s, float& c) {
    float wrapped = fmod(deg, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    if (wrapped == 0.0f) { s = 0.0f; c = 1.0f; return; }
    if (wrapped == 90.0f) { s = 1.0f; c = 0.0f; return; }
    if (wrapped == 180.0f) { s = 0.0f; c = -1.0f; return; }
    if (wrapped == 270.0f) { s = -1.0f; c = 0.0f; return; }
    float rad = deg * (kPi / 180.0f);
    s = sin(rad);
    c = cos(rad);
}

void Translate(ModelData& model, float x, float y, float z) {
    if (x == 0.0f && y == 0.0f && z == 0.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        v[i] += x;
        v[i + 1] += y;
        v[i + 2] += z;
    }
}

// Ry: x' = x c + z s, z' = -x s + z c. Matches Axis.YP / rotateCentered(UP).
void RotateY(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float x = v[i];
        float z = v[i + 2];
        v[i] = x * c + z * s;
        v[i + 2] = -x * s + z * c;
    }
}

// Rx: y' = y c - z s, z' = y s + z c. Axis.XP, east is +X.
void RotateX(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float y = v[i + 1];
        float z = v[i + 2];
        v[i + 1] = y * c - z * s;
        v[i + 2] = y * s + z * c;
    }
}

void RotateAbout(ModelData& model, float px, float py, float pz, char axis, float deg) {
    Translate(model, -px, -py, -pz);
    if (axis == 'x') RotateX(model, deg);
    else RotateY(model, deg);
    Translate(model, px, py, pz);
}

void RotateCentered(ModelData& model, char axis, float deg) {
    RotateAbout(model, 0.5f, 0.5f, 0.5f, axis, deg);
}

// Scale about the block centre. center(); scale(); uncenter().
void ScaleAboutCenter(ModelData& model, float s) {
    if (s == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        v[i] = (v[i] - 0.5f) * s + 0.5f;
        v[i + 1] = (v[i + 1] - 0.5f) * s + 0.5f;
        v[i + 2] = (v[i + 2] - 0.5f) * s + 0.5f;
    }
}

// AngleHelper.horizontalAngle: south 0, west -90, north 180, east -270, up/down 0.
float HorizontalAngle(const string& facing) {
    if (facing == "west") return -90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return -270.0f;
    return 0.0f;
}

// AngleHelper.verticalAngle: up -90, down 90, else 0.
float VerticalAngle(const string& facing) {
    if (facing == "up") return -90.0f;
    if (facing == "down") return 90.0f;
    return 0.0f;
}

// Direction.toYRot: south 0, west 90, north 180, east 270, up/down 0.
float ToYRot(const string& facing) {
    if (facing == "west") return 90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return 270.0f;
    return 0.0f;
}

// Java float remainder keeps the sign of the dividend.
float JavaMod(float value, float divisor) {
    return fmod(value, divisor);
}

string Opposite(const string& facing) {
    if (facing == "north") return "south";
    if (facing == "south") return "north";
    if (facing == "east") return "west";
    if (facing == "west") return "east";
    if (facing == "up") return "down";
    if (facing == "down") return "up";
    return "north";
}

void DirectionNormal(const string& facing, float& x, float& y, float& z) {
    x = y = z = 0.0f;
    if (facing == "down") y = -1.0f;
    else if (facing == "up") y = 1.0f;
    else if (facing == "north") z = -1.0f;
    else if (facing == "south") z = 1.0f;
    else if (facing == "west") x = -1.0f;
    else if (facing == "east") x = 1.0f;
}

string ResolveTexture(const nlohmann::json& modelJson, string value) {
    unordered_set<string> visited;
    while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
        string key = value.substr(1);
        if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
        const auto& next = modelJson["textures"][key];
        if (!next.is_string()) return "";
        value = next.get<string>();
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

string ModelAsset(const string& path) {
    shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
    auto index = GlobalCache::modelAssetIndex.find("modelassets:create:" + path);
    if (index == GlobalCache::modelAssetIndex.end()) return "";
    auto asset = GlobalCache::modelAssets.find(index->second);
    return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
}

// forge:obj / porting_lib:obj. ProcessModelJson only bakes "elements".
// Same mesh path as the blaze cage in SpecialBlock (no flip_v; Blender V).
ModelData LoadObjModel(const string& jsonPath) {
    nlohmann::json modelJson = GetModelJson("create", jsonPath);
    if (modelJson.is_null()) return {};
    modelJson = LoadParentModel("create", jsonPath, modelJson);
    if (!modelJson.contains("model") || !modelJson["model"].is_string()) return {};

    string objPath = modelJson["model"].get<string>();
    size_t colon = objPath.find(':');
    if (colon != string::npos) objPath = objPath.substr(colon + 1);
    if (objPath.starts_with("models/")) objPath = objPath.substr(7);
    string obj = ModelAsset(objPath);
    if (obj.empty()) return {};

    vector<array<float, 3>> positions;
    vector<array<float, 2>> uvs;
    unordered_map<string, string> mtlTextures;
    stringstream objStream(obj);
    string line;
    while (getline(objStream, line)) {
        if (!line.starts_with("mtllib ")) continue;
        string mtlName = line.substr(7);
        while (!mtlName.empty() && (mtlName.back() == '\r' || mtlName.back() == ' ')) mtlName.pop_back();
        string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
        stringstream mtlStream(ModelAsset(directory + mtlName));
        string materialName;
        while (getline(mtlStream, line)) {
            if (line.starts_with("newmtl ")) materialName = line.substr(7);
            else if (line.starts_with("map_Kd ") && !materialName.empty()) {
                string tex = line.substr(7);
                while (!tex.empty() && (tex.back() == '\r' || tex.back() == ' ')) tex.pop_back();
                mtlTextures[materialName] = ResolveTexture(modelJson, tex);
            }
        }
    }

    ModelData result;
    unordered_map<string, int> materialIndices;
    string currentMaterial;
    objStream.clear();
    objStream.seekg(0);
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
                string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial]
                    : "minecraft:block/missingno";
                materialIndex = static_cast<int>(result.materials.size());
                result.materials.push_back(CreateMaterial(texture));
                materialIndices[currentMaterial] = materialIndex;
            }
            else materialIndex = materialIt->second;

            for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                array<pair<int, int>, 4> quad = {
                    corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1]
                };
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

bool IsObjModel(const nlohmann::json& modelJson) {
    if (modelJson.is_null() || !modelJson.is_object()) return false;
    if (modelJson.contains("loader") && modelJson["loader"].is_string()) {
        string loader = modelJson["loader"].get<string>();
        if (loader == "forge:obj" || loader == "porting_lib:obj") return true;
    }
    return modelJson.contains("model") && modelJson["model"].is_string()
        && !modelJson.contains("elements");
}

ModelData LoadPartial(const string& path, int index) {
    nlohmann::json raw = GetModelJson("create", path);
    ModelData model = IsObjModel(raw)
        ? LoadObjModel(path)
        : ProcessModelJson("create", path, 0, 0, false, index);
    MarkNoCull(model);
    return model;
}

bool IsHeatTargetAbove(int x, int y, int z) {
    Block above = GetBlockById(GetBlockId(x, y + 1, z));
    string id = BaseId(above.name);
    return id == "create:basin" || id == "create:fluid_tank" || id == "create:creative_fluid_tank";
}

// SuperByteBuffer: the later call hits the vertex first.
// translate then rotateCentered -> rotate, then translate.
void PoseHead(ModelData& part, float yDeg, float translateY) {
    RotateCentered(part, 'y', yDeg);
    Translate(part, 0.0f, translateY, 0.0f);
}

// AllPartialModels.TRAIN_HAT = entity/train_hat.
// translate(0, headY); inert: translateY(0.5), center, scale(0.75), uncenter;
// else translateY(0.75); rotateCentered(angle+180, UP); translate(0.5, 0, 0.5).
void PoseTrainHat(ModelData& hat, bool inert, float headDeg, float headY) {
    Translate(hat, 0.5f, 0.0f, 0.5f);
    RotateCentered(hat, 'y', headDeg + 180.0f);
    if (inert) ScaleAboutCenter(hat, 0.75f);
    Translate(hat, 0.0f, (inert ? 0.5f : 0.75f) + headY, 0.0f);
}

void AppendBurner(ModelData& model, const string& blockName, int x, int y, int z,
    const NbtTagPtr& nbt) {
    string heat = StateValue(blockName, "blaze");
    if (heat.empty() || heat == "none") return;
    bool seething = heat == "seething";
    bool kindledPlus = seething || heat == "kindled";
    bool fadingPlus = kindledPlus || heat == "fading";
    if (!fadingPlus && heat != "smouldering") return;

    string facing = StateValue(blockName, "facing");
    if (facing.empty()) facing = "south";

    // time = 0 and the block-entity hash term = 0, so renderTick is 0.
    // headAnimation is 1 only when heat >= fading and a basin or tank sits above.
    bool heatedAbove = fadingPlus && IsHeatTargetAbove(x, y, z);
    float animation = heatedAbove ? 0.175f : 0.0f;
    bool blockAbove = animation > 0.125f;
    float offsetMult = fadingPlus ? 64.0f : 16.0f;
    float offset = sin(0.0f) / offsetMult;
    float offset1 = sin(kPi) / offsetMult;
    float offset2 = sin(kPi * 0.5f) / offsetMult;
    float headY = offset - animation * 0.75f;
    // (horizontalAngle(facing) + 180) % 360, then AngleHelper.rad.
    float headDeg = JavaMod(HorizontalAngle(facing) + 180.0f, 360.0f);

    if (fadingPlus && blockAbove) {
        ModelData flame = LoadPartial("block/blaze_burner/flame", kIdxFlame);
        RotateCentered(flame, 'y', headDeg);
        Append(model, std::move(flame));
    }

    const char* blazePath = "block/blaze_burner/blaze/inert";
    int blazeIndex = kIdxInert;
    bool inert = true;
    if (seething) {
        blazePath = blockAbove ? "block/blaze_burner/blaze/super_active"
                                : "block/blaze_burner/blaze/super";
        blazeIndex = blockAbove ? kIdxSuperActive : kIdxSuper;
        inert = false;
    } else if (fadingPlus) {
        bool active = blockAbove && kindledPlus;
        blazePath = active ? "block/blaze_burner/blaze/active" : "block/blaze_burner/blaze/idle";
        blazeIndex = active ? kIdxActive : kIdxIdle;
        inert = false;
    }
    ModelData head = LoadPartial(blazePath, blazeIndex);
    PoseHead(head, headDeg, headY);
    Append(model, std::move(head));

    if (HasTag(nbt, "Goggles")) {
        const char* path = inert ? "block/blaze_burner/goggles_small" : "block/blaze_burner/goggles";
        int index = inert ? kIdxGogglesSmall : kIdxGoggles;
        ModelData goggles = LoadPartial(path, index);
        PoseHead(goggles, headDeg, headY + 0.5f);
        Append(model, std::move(goggles));
    }

    if (HasTag(nbt, "TrainHat")) {
        ModelData hat = LoadPartial("entity/train_hat", kIdxTrainHat);
        PoseTrainHat(hat, inert, headDeg, headY);
        Append(model, std::move(hat));
    }

    if (fadingPlus) {
        // AllPartialModels RODS / RODS_2 are rods_small / rods_large
        // (super variants: superheated_rods_small / superheated_rods_large).
        const char* rodsPath = seething ? "block/blaze_burner/superheated_rods_small"
                                         : "block/blaze_burner/rods_small";
        const char* rods2Path = seething ? "block/blaze_burner/superheated_rods_large"
                                          : "block/blaze_burner/rods_large";
        ModelData rods = LoadPartial(rodsPath, kIdxRods);
        Translate(rods, 0.0f, offset1 + animation + 0.125f, 0.0f);
        Append(model, std::move(rods));
        ModelData rods2 = LoadPartial(rods2Path, kIdxRods2);
        Translate(rods2, 0.0f, offset2 + animation - 0.1875f, 0.0f);
        Append(model, std::move(rods2));
    }
}

// PackagerRenderer.renderSafe, trayOffset 0, hatch closed, no held box.
// facing is the opposite of the block-state facing.
// Hatch calls: translate, rotateYCenteredDegrees, rotateXCenteredDegrees
// -> vertex X, then Y, then translate.
void AppendPackager(ModelData& model, const string& id, const string& blockName) {
    string stateFacing = StateValue(blockName, "facing");
    if (stateFacing.empty()) stateFacing = "north";
    string facing = Opposite(stateFacing);
    float nx, ny, nz;
    DirectionNormal(facing, nx, ny, nz);

    ModelData hatch = LoadPartial("block/packager/hatch_closed", kIdxHatch);
    RotateCentered(hatch, 'x', VerticalAngle(facing));
    RotateCentered(hatch, 'y', HorizontalAngle(facing));
    Translate(hatch, nx * 0.49999f, ny * 0.49999f, nz * 0.49999f);
    Append(model, std::move(hatch));

    const char* trayPath = id == "packager" ? "block/packager/tray" : "block/repackager/tray";
    int trayIndex = id == "packager" ? kIdxTray : kIdxTrayDefrag;
    ModelData tray = LoadPartial(trayPath, trayIndex);
    RotateCentered(tray, 'y', ToYRot(facing));
    Append(model, std::move(tray));
}

} // namespace

void AppendCreateBlazePackager(const string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model) {
    string id = LocalId(blockName);
    if (id == "blaze_burner") {
        AppendBurner(model, blockName, x, y, z, blockEntityNbt);
        return;
    }
    if (id == "packager" || id == "repackager") AppendPackager(model, id, blockName);
}
