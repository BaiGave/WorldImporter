#include "CreateBogey.h"

#include <array>
#include <cmath>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {

// Partial-model cache indices. 400-409 is reserved for bogey parts.
// Shafts go through ProcessModelJson. OBJ partials are cached locally at 402-408
// because forge:obj is not a generic model path (see LoadCreateObjModel).
constexpr int kIdxShaftZ = 400;
constexpr int kIdxShaftX = 401;
constexpr int kIdxFrame = 402;
constexpr int kIdxSmallWheels = 403;
constexpr int kIdxDrive = 404;
constexpr int kIdxBelt = 405;
constexpr int kIdxPiston = 406;
constexpr int kIdxLargeWheels = 407;
constexpr int kIdxPin = 408;

// BogeyStyle.render translates the pose by this before the size renderer.
constexpr float kStyleDown = -1.5078125f;
// StandardBogeyRenderer scales the frame, drive and belt to avoid z-fighting.
constexpr float kZFightScale = 0.9980469f;

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

string NbtString(const NbtTagPtr& tag) {
    return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
}

// Right-handed degrees. Exact at multiples of 90 so ends stay on the block grid.
void SinCosDeg(float deg, float& s, float& c) {
    float wrapped = fmod(deg, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    if (wrapped == 0.0f) { s = 0.0f; c = 1.0f; return; }
    if (wrapped == 90.0f) { s = 1.0f; c = 0.0f; return; }
    if (wrapped == 180.0f) { s = 0.0f; c = -1.0f; return; }
    if (wrapped == 270.0f) { s = -1.0f; c = 0.0f; return; }
    float rad = deg * (3.14159265358979323846f / 180.0f);
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

void Scale(ModelData& model, float s) {
    if (s == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i < v.size(); ++i) v[i] *= s;
}

// Ry: x' = x c + z s, z' = -x s + z c. PoseStack Y, same as CreateFlaps.
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

// Rx: y' = y c - z s, z' = y s + z c.
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

// Rz: x' = x c - y s, y' = x s + y c.
void RotateZ(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float x = v[i];
        float y = v[i + 1];
        v[i] = x * c - y * s;
        v[i + 1] = x * s + y * c;
    }
}

// PoseStack and SuperByteBuffer post-multiply: the last call hits the vertex
// first, and each renderInto resets the SuperByteBuffer pose. Shared tail is
// BogeyStyle.translate(0, -1.5078125, 0), then the BER's axis yaw, then
// translate(0.5, 0.5, 0.5). Call this after the part-local transforms.
void ApplyBogeyPose(ModelData& model, bool axisX) {
    Translate(model, 0.0f, kStyleDown, 0.0f);
    if (axisX) RotateY(model, 90.0f);
    Translate(model, 0.5f, 0.5f, 0.5f);
}

void Emit(ModelData& target, ModelData part, bool axisX) {
    if (part.vertices.empty()) return;
    ApplyBogeyPose(part, axisX);
    Append(target, std::move(part));
}

// ---- forge:obj partials. Same loader as SpecialBlock.cpp LoadCreateObjModel.
// Kept here so bogeys do not edit SpecialBlock (other branches own that file).

string GetCreateModelAsset(const string& path) {
    shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
    auto index = GlobalCache::modelAssetIndex.find("modelassets:create:" + path);
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

ModelData LoadCreateObjModel(const string& jsonPath) {
    nlohmann::json modelJson = GetModelJson("create", jsonPath);
    if (modelJson.is_null()) return {};
    modelJson = LoadParentModel("create", jsonPath, modelJson);
    if (!modelJson.contains("model")) return {};

    string objPath = modelJson["model"].get<string>();
    size_t colon = objPath.find(':');
    if (colon != string::npos) objPath = objPath.substr(colon + 1);
    if (objPath.starts_with("models/")) objPath = objPath.substr(7);
    string obj = GetCreateModelAsset(objPath);
    if (obj.empty()) return {};

    vector<array<float, 3>> positions;
    vector<array<float, 2>> uvs;
    unordered_map<string, string> mtlTextures;
    stringstream objStream(obj);
    string line;
    while (getline(objStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.starts_with("mtllib ")) continue;
        string mtlName = line.substr(7);
        string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
        stringstream mtlStream(GetCreateModelAsset(directory + mtlName));
        string materialName;
        while (getline(mtlStream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
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
    // Create JSON sets flip_v for Minecraft (V=0 at top). WI exports for Blender
    // (V=0 at bottom), matching the JSON model path. Keep Blender-native OBJ V.
    while (getline(objStream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
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
                string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                materialIndex = static_cast<int>(result.materials.size());
                result.materials.push_back(CreateMaterial(texture));
                materialIndices[currentMaterial] = materialIndex;
            }
            else materialIndex = materialIt->second;

            for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                array<pair<int, int>, 4> quad = { corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1] };
                bool inRange = true;
                for (const auto& c : quad) {
                    if (c.first < 0 || static_cast<size_t>(c.first) >= positions.size()) inRange = false;
                }
                if (!inRange) continue;
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

ModelData CachedObj(const string& path, int index) {
    static mutex mu;
    static unordered_map<int, ModelData> cache;
    lock_guard<mutex> lock(mu);
    auto it = cache.find(index);
    if (it == cache.end()) it = cache.emplace(index, LoadCreateObjModel(path)).first;
    return it->second;
}

// CachedBuffers.block(create:shaft[axis=z|x]). Blockstate rotations:
// axis=z -> x 90, y 180; axis=x -> x 90, y 90. Distinct cache indices.
ModelData LoadShaft(bool alongX) {
    ModelData model = ProcessModelJson("create", "block/shaft", 90, alongX ? 90 : 180, false,
        alongX ? kIdxShaftX : kIdxShaftZ);
    MarkNoCull(model);
    return model;
}

// Java: translate, center, rotate(wheelAngle), uncenter.
// wheelAngle 0 makes center/rotate/uncenter the identity, so only the
// translate moves vertices. spinOnX selects rotateX vs rotateZ.
void EmitShaft(ModelData& target, const ModelData& shaft, float z, bool axisX, bool spinOnX) {
    ModelData part = shaft;
    if (spinOnX) RotateX(part, 0.0f);
    else RotateZ(part, 0.0f);
    Translate(part, -0.5f, 0.25f, z);
    Emit(target, std::move(part), axisX);
}

void EmitScaled(ModelData& target, const string& path, int index, bool axisX) {
    ModelData part = CachedObj(path, index);
    Scale(part, kZFightScale);
    Emit(target, std::move(part), axisX);
}

void RenderStandard(ModelData& target, bool large, bool axisX) {
    ModelData shaftZ = LoadShaft(false);
    for (int i = 0; i < 2; ++i) EmitShaft(target, shaftZ, static_cast<float>(-i), axisX, false);

    if (!large) {
        EmitScaled(target, "block/track/bogey/bogey_frame", kIdxFrame, axisX);
        ModelData wheels = CachedObj("block/track/bogey/bogey_wheel", kIdxSmallWheels);
        // Iterate.positiveAndNegative is +1 then -1. rotateX(wheelAngle) is 0.
        for (float side : {1.0f, -1.0f}) {
            ModelData part = wheels;
            RotateX(part, 0.0f);
            Translate(part, 0.0f, 0.75f, side);
            Emit(target, std::move(part), axisX);
        }
        return;
    }

    ModelData shaftX = LoadShaft(true);
    for (int i = 0; i < 2; ++i)
        EmitShaft(target, shaftX, 0.5f + static_cast<float>(i * -2), axisX, true);

    EmitScaled(target, "block/track/bogey/bogey_drive", kIdxDrive, axisX);
    // Belt UV scroll is 0 at wheelAngle 0.
    EmitScaled(target, "block/track/bogey/bogey_drive_belt", kIdxBelt, axisX);
    // Piston translate Z is 0.25 * sin(wheelAngle) = 0.
    Emit(target, CachedObj("block/track/bogey/bogey_drive_piston", kIdxPiston), axisX);

    {
        ModelData part = CachedObj("block/track/bogey/bogey_drive_wheel", kIdxLargeWheels);
        RotateX(part, 0.0f);
        Translate(part, 0.0f, 1.0f, 0.0f);
        Emit(target, std::move(part), axisX);
    }
    {
        // Vertex-first: rotX(0), translate(0, 0.25, 0), rotX(0), translate(0, 1, 0).
        ModelData part = CachedObj("block/track/bogey/bogey_drive_wheel_pin", kIdxPin);
        RotateX(part, 0.0f);
        Translate(part, 0.0f, 0.25f, 0.0f);
        RotateX(part, 0.0f);
        Translate(part, 0.0f, 1.0f, 0.0f);
        Emit(target, std::move(part), axisX);
    }
}

} // namespace

void AppendCreateBogey(const string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model) {
    string id = LocalId(blockName);
    if (id != "small_bogey" && id != "large_bogey") return;

    // Missing BogeyData uses the block's default style, create:standard.
    // StandardBogeyRenderer and BogeyStyle do not read UpsideDown (standard
    // blocks return isUpsideDown == false). No extra 180-degree flip.
    string style = "create:standard";
    if (blockEntityNbt) {
        auto data = getChildByName(blockEntityNbt, "BogeyData");
        if (data) {
            string stored = NbtString(getChildByName(data, "BogeyStyle"));
            if (!stored.empty()) style = stored;
        }
    }
    if (style != "create:standard") return;

    bool axisX = StateValue(blockName, "axis") == "x";
    RenderStandard(model, id == "large_bogey", axisX);
}
