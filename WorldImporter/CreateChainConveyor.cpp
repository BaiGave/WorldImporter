#include "CreateChainConveyor.h"
#include "GlobalCache.h"

#include <array>
#include <cmath>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {

constexpr int kIdxShaft = 350;
constexpr int kIdxWheel = 351;
constexpr int kIdxGuard = 352;

// ChainConveyorBlockEntity.calculateConnectionStats multiplies atan2 by this float.
constexpr float kDirRadToDeg = 57.295776f;
// Renderer yaw/pitch uses the float32 value of 180/pi, kept as a double multiply.
constexpr double kPoseRadToDeg = 57.2957763671875;
constexpr float kChainRadius = 0.09375f;
constexpr float kWheelRadius = 1.25f;
constexpr float kRimHeight = 0.375f;
// time 0 -> animation = 0 - 0.5. Near chain (not the far mip).
constexpr float kAnimation = -0.5f;

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

// Right-handed degrees. Exact at multiples of 90 so axis-aligned chains stay on grid.
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

// Ry: x' = x c + z s, z' = -x s + z c.
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

void RotateAbout(ModelData& model, float px, float py, float pz, char axis, float deg) {
    Translate(model, -px, -py, -pz);
    if (axis == 'x') RotateX(model, deg);
    else RotateY(model, deg);
    Translate(model, px, py, pz);
}

void RotateCentered(ModelData& model, char axis, float deg) {
    RotateAbout(model, 0.5f, 0.5f, 0.5f, axis, deg);
}

// VecHelper.rotate about Y: rad = (float)(deg/180*pi), then x' = x c + z s, z' = z c - x s.
void RotateOffsetY(double& x, double& z, float deg) {
    if (deg == 0.0f) return;
    float rad = static_cast<float>(static_cast<double>(deg) / 180.0 * 3.14159265358979323846);
    float s = sin(rad);
    float c = cos(rad);
    double nx = x * static_cast<double>(c) + z * static_cast<double>(s);
    double nz = z * static_cast<double>(c) - x * static_cast<double>(s);
    x = nx;
    z = nz;
}

// Java float remainder, then add 360 when negative. ChainConveyorBlockEntity.wrapAngle.
float WrapAngle(float angle) {
    angle = fmod(angle, 360.0f);
    if (angle < 0.0f) angle += 360.0f;
    return angle;
}

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

Material MakeTexMaterial(const string& texture) {
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

// forge:obj partials have no "elements", so ProcessModelJson caches an empty model.
// Bake the OBJ the same way as SpecialBlock::LoadCreateObjModel (no flip_v: WI UVs
// stay Blender-native, V = 0 at the bottom of the image).
ModelData ParseCreateObj(const string& jsonPath) {
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
        if (!line.starts_with("mtllib ")) continue;
        string mtlName = line.substr(7);
        while (!mtlName.empty() && (mtlName.back() == '\r' || mtlName.back() == ' ')) mtlName.pop_back();
        string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
        stringstream mtlStream(GetCreateModelAsset(directory + mtlName));
        string materialName;
        while (getline(mtlStream, line)) {
            if (line.starts_with("newmtl ")) materialName = line.substr(7);
            else if (line.starts_with("map_Kd ") && !materialName.empty()) {
                string tex = line.substr(7);
                while (!tex.empty() && (tex.back() == '\r' || tex.back() == ' ')) tex.pop_back();
                while (!materialName.empty() && (materialName.back() == '\r' || materialName.back() == ' '))
                    materialName.pop_back();
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
                string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                materialIndex = static_cast<int>(result.materials.size());
                result.materials.push_back(MakeTexMaterial(texture));
                materialIndices[currentMaterial] = materialIndex;
            }
            else materialIndex = materialIt->second;

            for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                array<pair<int, int>, 4> quad = { corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1] };
                Face face{};
                face.materialIndex = materialIndex;
                face.faceDirection = FaceType::DO_NOT_CULL;
                for (size_t i = 0; i < quad.size(); ++i) {
                    if (quad[i].first < 0 || static_cast<size_t>(quad[i].first) >= positions.size()) continue;
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

mutex& ObjCacheMutex() {
    static mutex mutex;
    return mutex;
}

unordered_map<string, ModelData>& ObjCache() {
    static unordered_map<string, ModelData> cache;
    return cache;
}

ModelData LoadObjPartial(const string& jsonPath) {
    {
        lock_guard<mutex> lock(ObjCacheMutex());
        auto it = ObjCache().find(jsonPath);
        if (it != ObjCache().end()) return it->second;
    }
    ModelData loaded = ParseCreateObj(jsonPath);
    lock_guard<mutex> lock(ObjCacheMutex());
    auto [it, inserted] = ObjCache().emplace(jsonPath, std::move(loaded));
    (void)inserted;
    return it->second;
}

ModelData LoadPartial(const string& path, int index) {
    ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
    if (model.vertices.empty()) model = LoadObjPartial(path);
    MarkNoCull(model);
    return model;
}

const Material& ChainMaterial() {
    static once_flag once;
    static Material material;
    call_once(once, [] { material = MakeTexMaterial("minecraft:block/chain"); });
    return material;
}

int AddUv(ModelData& model, float u, float minecraftV) {
    int index = static_cast<int>(model.uvCoordinates.size() / 2);
    // JSON models store v' = 1 - v so Blender (V = 0 at the bottom) matches the PNG.
    // Values outside 0..1 repeat; chain.png is sampled with REPEAT.
    model.uvCoordinates.push_back(u);
    model.uvCoordinates.push_back(1.0f - minecraftV);
    return index;
}

// One plane of renderPart, both windings. Front and back share 4 vertex indices so
// SeparateCoincidentFaces keeps both sides and registers the chain material.
void AddChainPlane(ModelData& model, float x0, float z0, float x1, float z1,
    float length, float minU, float maxU, float minV, float maxV) {
    int base = static_cast<int>(model.vertices.size() / 3);
    float corners[4][3] = {
        {x0, length, z0},
        {x0, 0.0f, z0},
        {x1, 0.0f, z1},
        {x1, length, z1},
    };
    for (int i = 0; i < 4; ++i) {
        model.vertices.push_back(corners[i][0]);
        model.vertices.push_back(corners[i][1]);
        model.vertices.push_back(corners[i][2]);
    }
    int uvFront[4] = {
        AddUv(model, maxU, minV),
        AddUv(model, maxU, maxV),
        AddUv(model, minU, maxV),
        AddUv(model, minU, minV),
    };
    // Swapped-corner quad: U flips onto the same positions, winding reverses.
    int uvBack[4] = {
        AddUv(model, maxU, minV),
        AddUv(model, maxU, maxV),
        AddUv(model, minU, maxV),
        AddUv(model, minU, minV),
    };

    int materialIndex = 0;
    Face front{};
    front.materialIndex = materialIndex;
    front.faceDirection = FaceType::DO_NOT_CULL;
    front.bothSided = true;
    front.vertexIndices = {base, base + 1, base + 2, base + 3};
    front.uvIndices = {uvFront[0], uvFront[1], uvFront[2], uvFront[3]};

    Face back{};
    back.materialIndex = materialIndex;
    back.faceDirection = FaceType::DO_NOT_CULL;
    back.bothSided = true;
    back.vertexIndices = {base + 3, base + 2, base + 1, base};
    back.uvIndices = {uvBack[0], uvBack[1], uvBack[2], uvBack[3]};

    model.faces.push_back(front);
    model.faces.push_back(back);
}

// Local chain verts are (x, along, z). PoseStack applies later calls first:
// Ry(45), Rx(90 - pitch), Ry(yaw), then translate to the block-local start.
// translate(0.5,0,0.5) + uncenter + translate(0,0.5,0) cancel, so they are omitted.
void PoseChain(ModelData& chain, float yaw, float pitch, double startX, double startY, double startZ) {
    RotateY(chain, 45.0f);
    RotateX(chain, 90.0f - pitch);
    RotateY(chain, yaw);
    Translate(chain,
        static_cast<float>(startX - 0.5),
        static_cast<float>(startY - 0.5),
        static_cast<float>(startZ - 0.5));
    Translate(chain, 0.5f, 0.5f, 0.5f);
}

bool ReadCoord(const NbtTagPtr& compound, const char* name, int& out) {
    auto tag = getChildByName(compound, name);
    if (!tag) return false;
    if (tag->type == TagType::INT && tag->payload.size() >= 4) {
        out = bytesToInt(tag->payload);
        return true;
    }
    if (tag->type == TagType::SHORT && tag->payload.size() >= 2) {
        out = bytesToShort(tag->payload);
        return true;
    }
    if (tag->type == TagType::BYTE && !tag->payload.empty()) {
        out = bytesToByte(tag->payload);
        return true;
    }
    if (tag->type == TagType::LONG && tag->payload.size() >= 8) {
        out = static_cast<int>(bytesToLong(tag->payload));
        return true;
    }
    return false;
}

void AppendConnection(ModelData& model, const ModelData& guardTemplate, int cx, int cy, int cz) {
    // reversed stays false at speed 0. direction = 57.295776f * atan2(dx, dz).
    float direction = kDirRadToDeg * static_cast<float>(atan2(static_cast<double>(cx), static_cast<double>(cz)));
    float angle = WrapAngle(direction - 35.0f);
    float oppositeAngle = WrapAngle(angle + 180.0f + 70.0f);

    double sx = 0.0, sz = static_cast<double>(kWheelRadius);
    RotateOffsetY(sx, sz, angle);
    double startX = 0.5 + sx;
    double startY = static_cast<double>(kRimHeight);
    double startZ = 0.5 + sz;

    double ex = 0.0, ez = static_cast<double>(kWheelRadius);
    RotateOffsetY(ex, ez, oppositeAngle);
    double endX = static_cast<double>(cx) + 0.5 + ex;
    double endY = static_cast<double>(cy) + static_cast<double>(kRimHeight);
    double endZ = static_cast<double>(cz) + 0.5 + ez;

    double dx = endX - startX;
    double dy = endY - startY;
    double dz = endZ - startZ;
    double horiz = sqrt(dx * dx + dz * dz);
    float yaw = static_cast<float>(kPoseRadToDeg * atan2(dx, dz));
    float pitch = static_cast<float>(kPoseRadToDeg * atan2(dy, horiz));
    float length = static_cast<float>(sqrt(dx * dx + dy * dy + dz * dz));

    ModelData guard = guardTemplate;
    RotateCentered(guard, 'y', yaw);
    Append(model, std::move(guard));

    if (!(length > 0.0f)) return;

    float minV = kAnimation;
    float maxV = length + minV;
    float minU = 0.0f;
    float maxU = 0.1875f;
    float uO = 0.1875f;
    float radius = kChainRadius;

    ModelData chain;
    chain.materials.push_back(ChainMaterial());
    // renderPart near arguments: (0, r), (r, 0), (-r, 0), (0, -r).
    AddChainPlane(chain, 0.0f, radius, 0.0f, -radius, length, minU, maxU, minV, maxV);
    AddChainPlane(chain, radius, 0.0f, -radius, 0.0f, length, minU + uO, maxU + uO, minV, maxV);
    PoseChain(chain, yaw, pitch, startX, startY, startZ);
    Append(model, std::move(chain));
}

} // namespace

void AppendCreateChainConveyor(const string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model) {
    (void)x;
    (void)y;
    (void)z;
    if (LocalId(blockName) != "chain_conveyor") return;

    // Kinetic axis is Y and the angle at speed 0 is 0. The shaft OBJ is already on Y.
    Append(model, LoadPartial("block/chain_conveyor/shaft", kIdxShaft));
    Append(model, LoadPartial("block/chain_conveyor/wheel", kIdxWheel));

    if (!blockEntityNbt) return;
    auto connections = getChildByName(blockEntityNbt, "Connections");
    if (!connections || connections->type != TagType::LIST) return;

    ModelData guardTemplate = LoadPartial("block/chain_conveyor/guard", kIdxGuard);
    for (const auto& entry : connections->children) {
        if (!entry || entry->type != TagType::COMPOUND) continue;
        int cx = 0, cy = 0, cz = 0;
        if (!ReadCoord(entry, "X", cx) || !ReadCoord(entry, "Y", cy) || !ReadCoord(entry, "Z", cz)) continue;
        // Each conveyor stores the link, and the game draws a chain from both ends.
        AppendConnection(model, guardTemplate, cx, cy, cz);
    }
}
