#include "CreateCurvedTrack.h"
#include "GlobalCache.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {

// Partial-model cache indices. 390-399 is reserved for curved tracks.
constexpr int kIdxTie = 390;
constexpr int kIdxLeft = 391;
constexpr int kIdxRight = 392;
constexpr int kIdxGirderMiddle = 393;
constexpr int kIdxGirderTop = 394;
constexpr int kIdxGirderBottom = 395;

// BezierConnection iterator lifts the curve to the rail surface.
constexpr double kTrackYOffset = 0.1875;
// Rail / girder offset from the centre line. The Java literal is a float.
constexpr double kRailOffset = static_cast<double>(0.965f);
constexpr float kTiePivotY = -0.12890625f;
constexpr float kModelZNudge = -0.03125f;
// Mth.equal epsilon.
constexpr double kEqualEps = 1.0E-5;

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double x, double y, double z) : x(x), y(y), z(z) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    bool operator==(Vec3 o) const { return x == o.x && y == o.y && z == o.z; }
    double dot(Vec3 o) const { return x * o.x + y * o.y + z * o.z; }
    // Vec3.cross: (y z' - z y', z x' - x z', x y' - y x').
    Vec3 cross(Vec3 o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    double lengthSqr() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(lengthSqr()); }
    double distanceTo(Vec3 o) const { return (*this - o).length(); }
    double distanceToSqr(Vec3 o) const { return (*this - o).lengthSqr(); }
    // Vec3.normalize returns ZERO when the length is under 1e-4.
    Vec3 normalized() const {
        double len = length();
        if (len < 1.0E-4) return {};
        return *this * (1.0 / len);
    }
};

double Signum(double v) {
    if (v > 0.0) return 1.0;
    if (v < 0.0) return -1.0;
    return 0.0;
}

bool NearlyEqual(double a, double b) {
    return std::abs(a - b) < kEqualEps;
}

double Clamp(double v, double lo, double hi) {
    return v < lo ? lo : std::min(v, hi);
}

// AngleHelper.deg: radians to a float degree value. Zero stays zero.
float Deg(double radians) {
    if (radians == 0.0) return 0.0f;
    return static_cast<float>(radians * 180.0 / M_PI);
}

// VecHelper.rotate. The degree value is turned back into a float radian, then
// sin/cos run in double (Mth.sin/cos are a float table; the curve math itself
// stays double, which is what the segment positions use).
Vec3 RotateAxis(Vec3 v, double degrees, char axis) {
    if (degrees == 0.0) return v;
    float angle = static_cast<float>(degrees / 180.0 * M_PI);
    double s = std::sin(static_cast<double>(angle));
    double c = std::cos(static_cast<double>(angle));
    if (axis == 'x') return {v.x, v.y * c - v.z * s, v.z * c + v.y * s};
    if (axis == 'y') return {v.x * c + v.z * s, v.y, v.z * c - v.x * s};
    return {v.x * c - v.y * s, v.y * c + v.x * s, v.z};
}

// VecHelper.lerp. t is the float parameter; the scale is that float widened.
Vec3 Lerp(float t, Vec3 from, Vec3 to) {
    return from + (to - from) * static_cast<double>(t);
}

// VecHelper.bezier. p1/p2 are the ends, q1/q2 the handles.
Vec3 Bezier(Vec3 p1, Vec3 p2, Vec3 q1, Vec3 q2, float t) {
    Vec3 v1 = Lerp(t, p1, q1);
    Vec3 v2 = Lerp(t, q1, q2);
    Vec3 v3 = Lerp(t, q2, p2);
    return Lerp(t, Lerp(t, v1, v2), Lerp(t, v2, v3));
}

// Coefficients are Java float expressions, then widened to double.
Vec3 BezierDerivative(Vec3 p1, Vec3 p2, Vec3 q1, Vec3 q2, float t) {
    float tt = t * t;
    double c1 = static_cast<double>(-3.0f * tt + 6.0f * t - 3.0f);
    double c2 = static_cast<double>(9.0f * tt - 12.0f * t + 3.0f);
    double c3 = static_cast<double>(-9.0f * tt + 6.0f * t);
    double c4 = static_cast<double>(3.0f * tt);
    return p1 * c1 + q1 * c2 + q2 * c3 + p2 * c4;
}

// Bytecode of VecHelper.slerp. Only used when the two face normals differ.
// from * (sin(1-p) * theta) + to * sin(theta * p), then * (1 / sin(theta)).
Vec3 Slerp(float p, Vec3 from, Vec3 to) {
    double theta = std::acos(Clamp(from.dot(to), -1.0, 1.0));
    float sTheta = static_cast<float>(std::sin(static_cast<double>(static_cast<float>(theta))));
    if (sTheta == 0.0f) return from;
    double a = static_cast<double>(static_cast<float>(std::sin(static_cast<double>(1.0f - p)))) * theta;
    double b = static_cast<double>(static_cast<float>(std::sin(static_cast<double>(static_cast<float>(theta * static_cast<double>(p))))));
    double inv = static_cast<double>(1.0f / sTheta);
    return (from * a + to * b) * inv;
}

// VecHelper.intersect on the XZ plane (Direction.Axis.Y). Returns (t, u) for
// p1 + t*r = p2 + u*s, or nullopt when the directions are parallel in XZ.
optional<array<double, 2>> IntersectXZ(Vec3 p1, Vec3 p2, Vec3 r, Vec3 s) {
    Vec3 q = p2 - p1;
    double rcs = r.x * s.z - r.z * s.x;
    if (NearlyEqual(rcs, 0.0)) return nullopt;
    Vec3 rdiv = r * (1.0 / rcs);
    Vec3 sdiv = s * (1.0 / rcs);
    double t = q.x * sdiv.z - q.z * sdiv.x;
    double u = q.x * rdiv.z - q.z * rdiv.x;
    return array<double, 2>{t, u};
}

struct Runtime {
    double handleLength = 1.0;
    double length = 0.0;
    int segments = 0;
    vector<float> stepLUT;
    float segmentT(int index) const {
        if (index == segments) return 1.0f;
        return static_cast<float>(index) * stepLUT[static_cast<size_t>(index)] / static_cast<float>(segments);
    }
};

// BezierConnection.Runtime.determineHandles. Axes are already normalized.
double DetermineHandles(Vec3 end1, Vec3 end2, Vec3 axis1, Vec3 axis2) {
    Vec3 up{0.0, 1.0, 0.0};
    Vec3 cross1 = axis1.cross(up);
    Vec3 cross2 = axis2.cross(up);
    double a1 = std::atan2(-axis2.z, -axis2.x);
    double a2 = std::atan2(axis1.z, axis1.x);
    double angle = a1 - a2;
    // float circle = (float)Math.PI * 2, then the wrap uses that float widened.
    float circleF = static_cast<float>(M_PI) * 2.0f;
    double circle = static_cast<double>(circleF);
    angle = std::fmod(angle + circle, circle);
    if (angle < 0.0) angle += circle;
    if (std::abs(circle - angle) < std::abs(angle)) angle = circle - angle;

    if (NearlyEqual(angle, 0.0)) {
        auto hit = IntersectXZ(end1, end2, axis1, cross2);
        if (hit) {
            double t = std::abs((*hit)[0]);
            double u = std::abs((*hit)[1]);
            double lo = std::min(t, u);
            double hi = std::max(t, u);
            if (lo > 1.2 && hi / lo > 1.0 && hi / lo < 3.0) return hi - lo;
        }
        return end2.distanceTo(end1) / 3.0;
    }

    double n = circle / angle;
    double factor = 1.3333333333333333 * std::tan(M_PI / (2.0 * n));
    auto hit = IntersectXZ(end1, end2, cross1, cross2);
    if (!hit) return end2.distanceTo(end1) / 3.0;
    double radius = std::abs((*hit)[1]);
    double handle = radius * factor;
    if (NearlyEqual(handle, 0.0)) handle = 1.0;
    return handle;
}

double ComputeLength(Vec3 finish1, Vec3 finish2, Vec3 end1, Vec3 end2, int scanCount) {
    double length = 0.0;
    Vec3 previous = end1;
    for (int i = 0; i <= scanCount; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(scanCount);
        Vec3 result = Bezier(end1, end2, finish1, finish2, t);
        length += result.distanceTo(previous);
        previous = result;
    }
    return length;
}

Runtime BuildRuntime(Vec3 end1, Vec3 end2, Vec3 axis1, Vec3 axis2) {
    Runtime runtime;
    Vec3 n1 = axis1.normalized();
    Vec3 n2 = axis2.normalized();
    runtime.handleLength = DetermineHandles(end1, end2, n1, n2);
    if (!std::isfinite(runtime.handleLength)) runtime.handleLength = 1.0;
    Vec3 finish1 = n1 * runtime.handleLength + end1;
    Vec3 finish2 = n2 * runtime.handleLength + end2;
    runtime.length = ComputeLength(finish1, finish2, end1, end2, 16);
    if (!std::isfinite(runtime.length) || runtime.length <= 0.0) return runtime;
    double scaled = runtime.length * 2.0;
    if (scaled > 8192.0) scaled = 8192.0;
    runtime.segments = static_cast<int>(scaled);
    if (runtime.segments < 1) return runtime;

    runtime.stepLUT.assign(static_cast<size_t>(runtime.segments) + 1, 0.0f);
    runtime.stepLUT[0] = 1.0f;
    float combined = 0.0f;
    Vec3 previous = end1;
    for (int i = 0; i <= runtime.segments; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(runtime.segments);
        Vec3 result = Bezier(end1, end2, finish1, finish2, t);
        if (i > 0) {
            combined = static_cast<float>(static_cast<double>(combined) + result.distanceTo(previous) / runtime.length);
            runtime.stepLUT[static_cast<size_t>(i)] = t / combined;
        }
        previous = result;
    }
    return runtime;
}

// TrackRenderer.getModelAngles. Returns (pitch, yaw, roll) in radians.
// `normal` is the track side normal, `diff` is the segment direction.
Vec3 ModelAngles(Vec3 normal, Vec3 diff) {
    double len = static_cast<double>(static_cast<float>(std::sqrt(static_cast<float>(diff.x * diff.x + diff.z * diff.z))));
    double yaw = std::atan2(diff.x, diff.z);
    double pitch = std::atan2(len, diff.y) - 1.5707963267948966;
    Vec3 yawPitchNormal = RotateAxis(RotateAxis(Vec3{0.0, 1.0, 0.0}, Deg(pitch), 'x'), Deg(yaw), 'y');
    double signum = Signum(yawPitchNormal.dot(normal));
    if (std::abs(signum) < 0.5)
        signum = yawPitchNormal.distanceToSqr(normal) < 0.5 ? -1.0 : 1.0;
    Vec3 side = diff.cross(normal).normalized();
    double roll = std::acos(Clamp(side.dot(yawPitchNormal), -1.0, 1.0)) * signum;
    return {pitch, yaw, roll};
}

void MarkNoCull(ModelData& model) {
    for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
}

void Append(ModelData& target, ModelData part) {
    if (part.vertices.empty()) return;
    MarkNoCull(part);
    MergeModelsDirectly(target, part);
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

void Translate(ModelData& model, Vec3 p) {
    Translate(model, static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
}

void ScaleZ(ModelData& model, float scale) {
    if (scale == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) v[i + 2] *= scale;
}

// PoseStack axis rotations (radians). Later pose calls are applied first by the caller.
void RotateX(ModelData& model, float radians) {
    if (radians == 0.0f) return;
    double s = std::sin(static_cast<double>(radians));
    double c = std::cos(static_cast<double>(radians));
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        double y = v[i + 1];
        double z = v[i + 2];
        v[i + 1] = static_cast<float>(y * c - z * s);
        v[i + 2] = static_cast<float>(y * s + z * c);
    }
}

void RotateY(ModelData& model, float radians) {
    if (radians == 0.0f) return;
    double s = std::sin(static_cast<double>(radians));
    double c = std::cos(static_cast<double>(radians));
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        double x = v[i];
        double z = v[i + 2];
        v[i] = static_cast<float>(x * c + z * s);
        v[i + 2] = static_cast<float>(-x * s + z * c);
    }
}

void RotateZ(ModelData& model, float radians) {
    if (radians == 0.0f) return;
    double s = std::sin(static_cast<double>(radians));
    double c = std::cos(static_cast<double>(radians));
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        double x = v[i];
        double y = v[i + 1];
        v[i] = static_cast<float>(x * c - y * s);
        v[i + 1] = static_cast<float>(x * s + y * c);
    }
}

// PoseStack calls are translate, rotateY, rotateX, rotateZ, then the local
// translate (and scale for rails). Vertices see that list in reverse.
void PosePart(ModelData& model, Vec3 place, Vec3 angles, float ox, float oy, float oz, float zScale) {
    if (zScale != 1.0f) ScaleZ(model, zScale);
    Translate(model, ox, oy, oz);
    RotateZ(model, static_cast<float>(angles.z));
    RotateX(model, static_cast<float>(angles.x));
    RotateY(model, static_cast<float>(angles.y));
    Translate(model, place);
}

float GirderYOffset(int segmentIndex) {
    float parity = static_cast<float>(segmentIndex % 2 == 0 ? 1 : -1);
    return 0.125f + parity / 2048.0f - 9.765625E-4f;
}

string LocalId(const string& blockName) {
    size_t bracket = blockName.find('[');
    string base = bracket == string::npos ? blockName : blockName.substr(0, bracket);
    size_t colon = base.rfind(':');
    return colon == string::npos ? base : base.substr(colon + 1);
}

string Trim(string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == ' ' || value.back() == '\t'))
        value.pop_back();
    size_t start = 0;
    while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) ++start;
    return value.substr(start);
}

string GetModelAsset(const string& ns, const string& path) {
    shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
    auto index = GlobalCache::modelAssetIndex.find("modelassets:" + ns + ":" + path);
    if (index == GlobalCache::modelAssetIndex.end()) return "";
    auto asset = GlobalCache::modelAssets.find(index->second);
    return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
}

bool ModelExists(const string& ns, const string& path) {
    shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
    if (GlobalCache::modelIndex.find("models:" + ns + ":" + path) != GlobalCache::modelIndex.end())
        return true;
    for (const auto& modId : GlobalCache::jarOrder) {
        if (GlobalCache::models.find(modId + ":" + ns + ":" + path) != GlobalCache::models.end())
            return true;
    }
    return false;
}

string ResolveTexture(const nlohmann::json& modelJson, string value) {
    unordered_set<string> visited;
    while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
        string key = value.substr(1);
        if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
        if (!modelJson["textures"][key].is_string()) return "";
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

// forge:obj partials have no elements, so ProcessModelJson caches an empty model.
// Bake the OBJ the same way as the chain conveyor (no flip_v: WI UVs stay
// Blender-native, V = 0 at the bottom of the image).
ModelData ParseCreateObj(const string& ns, const string& jsonPath) {
    nlohmann::json modelJson = GetModelJson(ns, jsonPath);
    if (modelJson.is_null()) return {};
    modelJson = LoadParentModel(ns, jsonPath, modelJson);
    if (!modelJson.contains("model") || !modelJson["model"].is_string()) return {};

    string objPath = modelJson["model"].get<string>();
    size_t colon = objPath.find(':');
    string objNs = ns;
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
        if (!Trim(line).starts_with("mtllib ")) continue;
        string mtlName = Trim(line.substr(line.find("mtllib ") + 7));
        string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
        stringstream mtlStream(GetModelAsset(objNs, directory + mtlName));
        string materialName;
        while (getline(mtlStream, line)) {
            string trimmed = Trim(line);
            if (trimmed.starts_with("newmtl ")) materialName = Trim(trimmed.substr(7));
            else if (trimmed.starts_with("map_Kd ") && !materialName.empty())
                mtlTextures[materialName] = ResolveTexture(modelJson, Trim(trimmed.substr(7)));
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
        else if (type == "usemtl") {
            values >> currentMaterial;
            currentMaterial = Trim(currentMaterial);
        }
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

ModelData LoadObjPartial(const string& ns, const string& jsonPath) {
    string key = ns + ":" + jsonPath;
    {
        lock_guard<mutex> lock(ObjCacheMutex());
        auto it = ObjCache().find(key);
        if (it != ObjCache().end()) return it->second;
    }
    ModelData loaded = ParseCreateObj(ns, jsonPath);
    lock_guard<mutex> lock(ObjCacheMutex());
    auto [it, inserted] = ObjCache().emplace(key, std::move(loaded));
    (void)inserted;
    return it->second;
}

ModelData LoadPartial(const string& ns, const string& path, int index) {
    ModelData model = ProcessModelJson(ns, path, 0, 0, false, index);
    if (model.vertices.empty()) model = LoadObjPartial(ns, path);
    MarkNoCull(model);
    return model;
}

struct Partials {
    ModelData tie;
    ModelData left;
    ModelData right;
    ModelData girderMiddle;
    ModelData girderTop;
    ModelData girderBottom;
};

struct MaterialPaths {
    string ns = "create";
    string tie = "block/track/tie";
    string left = "block/track/segment_left";
    string right = "block/track/segment_right";
};

// TrackMaterial.defaultModels is the andesite set. standardModels is
// block/track/<id>/{tie,segment_left,segment_right} when that model exists.
MaterialPaths PathsFor(const string& material) {
    MaterialPaths paths;
    string id = material;
    size_t colon = id.find(':');
    string path = colon == string::npos ? id : id.substr(colon + 1);
    string ns = colon == string::npos ? "create" : id.substr(0, colon);
    if (path.empty() || path == "andesite") return paths;
    string tie = "block/track/" + path + "/tie";
    if (!ModelExists(ns, tie)) return paths;
    paths.ns = ns;
    paths.tie = tie;
    paths.left = "block/track/" + path + "/segment_left";
    paths.right = "block/track/" + path + "/segment_right";
    return paths;
}

// shared_ptr so a later material insert cannot rehash a Partials another
// chunk thread is still reading. Model workers run in parallel.
shared_ptr<const Partials> PartsFor(const string& material) {
    static mutex mutex;
    static unordered_map<string, shared_ptr<const Partials>> cache;
    lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(material);
    if (it != cache.end()) return it->second;
    MaterialPaths paths = PathsFor(material);
    auto loaded = make_shared<Partials>();
    loaded->tie = LoadPartial(paths.ns, paths.tie, kIdxTie);
    loaded->left = LoadPartial(paths.ns, paths.left, kIdxLeft);
    loaded->right = LoadPartial(paths.ns, paths.right, kIdxRight);
    loaded->girderMiddle = LoadPartial("create", "block/metal_girder/segment_middle", kIdxGirderMiddle);
    loaded->girderTop = LoadPartial("create", "block/metal_girder/segment_top", kIdxGirderTop);
    loaded->girderBottom = LoadPartial("create", "block/metal_girder/segment_bottom", kIdxGirderBottom);
    cache.emplace(material, loaded);
    return loaded;
}

struct Connection {
    Vec3 starts[2];
    Vec3 axes[2];
    Vec3 normals[2];
    bool primary = false;
    bool girder = false;
    string material = "create:andesite";
};

bool ReadDouble(const NbtTagPtr& tag, double& out) {
    if (!tag || tag->type != TagType::DOUBLE || tag->payload.size() < 8) return false;
    out = bytesToDouble(tag->payload);
    return true;
}

bool ReadVec3List(const NbtTagPtr& compound, Vec3& out) {
    auto list = getChildByName(compound, "V");
    if (!list || list->type != TagType::LIST || list->children.size() < 3) return false;
    double c[3];
    for (int i = 0; i < 3; ++i) {
        if (!ReadDouble(list->children[static_cast<size_t>(i)], c[i])) return false;
    }
    out = {c[0], c[1], c[2]};
    return true;
}

bool ReadCouple(const NbtTagPtr& parent, const char* name, Vec3& a, Vec3& b) {
    auto list = getChildByName(parent, name);
    if (!list || list->type != TagType::LIST || list->children.size() < 2) return false;
    auto first = list->children[0];
    auto second = list->children[1];
    if (!first || first->type != TagType::COMPOUND || !second || second->type != TagType::COMPOUND) return false;
    return ReadVec3List(first, a) && ReadVec3List(second, b);
}

bool ReadBool(const NbtTagPtr& parent, const char* name) {
    auto tag = getChildByName(parent, name);
    return tag && tag->type == TagType::BYTE && !tag->payload.empty() && bytesToByte(tag->payload) != 0;
}

string ReadString(const NbtTagPtr& parent, const char* name) {
    auto tag = getChildByName(parent, name);
    if (!tag || tag->type != TagType::STRING) return "";
    return bytesToString(tag->payload);
}

struct Sample {
    Vec3 position;
    Vec3 derivative;
    Vec3 normal;
};

Sample MakeSample(const Runtime& runtime, const Connection& bc, int index) {
    float t = runtime.segmentT(index);
    Vec3 lift{0.0, kTrackYOffset, 0.0};
    Vec3 end1 = bc.starts[0] + lift;
    Vec3 end2 = bc.starts[1] + lift;
    // The iterator scales the raw axis, not the normalized one used for the length.
    Vec3 finish1 = bc.axes[0] * runtime.handleLength + end1;
    Vec3 finish2 = bc.axes[1] * runtime.handleLength + end2;
    Sample sample;
    sample.position = Bezier(end1, end2, finish1, finish2, t);
    sample.derivative = BezierDerivative(end1, end2, finish1, finish2, t).normalized();
    Vec3 face = bc.normals[0] == bc.normals[1] ? bc.normals[0] : Slerp(t, bc.normals[0], bc.normals[1]);
    sample.normal = face.cross(sample.derivative).normalized();
    return sample;
}

void EmitTie(ModelData& target, const ModelData& src, Vec3 place, Vec3 normal, Vec3 diff) {
    ModelData part = src;
    PosePart(part, place, ModelAngles(normal, diff), -0.5f, kTiePivotY, 0.0f, 1.0f);
    Append(target, std::move(part));
}

void EmitScaled(ModelData& target, const ModelData& src, Vec3 place, Vec3 normal, Vec3 diff,
    float y, float zNudge, float lengthScale) {
    float zScale = static_cast<float>(diff.length()) * lengthScale;
    ModelData part = src;
    PosePart(part, place, ModelAngles(normal, diff), 0.0f, y, zNudge, zScale);
    Append(target, std::move(part));
}

void EmitConnection(ModelData& target, const Connection& bc, const Partials& parts) {
    // Length and the step LUT are measured on the raw starts (no rail lift).
    Runtime runtime = BuildRuntime(bc.starts[0], bc.starts[1], bc.axes[0], bc.axes[1]);
    if (runtime.segments < 1) return;

    bool havePrev = false;
    Vec3 prevLeft, prevRight, prevMiddle;
    Vec3 prevLeftTop, prevRightTop, prevLeftBottom, prevRightBottom;
    for (int i = 0; i <= runtime.segments; ++i) {
        Sample sample = MakeSample(runtime, bc, i);
        Vec3 left = sample.position + sample.normal * kRailOffset;
        Vec3 right = sample.position - sample.normal * kRailOffset;
        Vec3 middle = (left + right) * 0.5;

        Vec3 up = sample.derivative.normalized().cross(sample.normal);
        Vec3 leftTop = sample.position + sample.normal * 1.0 + up * -0.5;
        Vec3 rightTop = sample.position - sample.normal * 1.0 + up * -0.5;
        Vec3 leftBottom = leftTop + up * -0.625;
        Vec3 rightBottom = rightTop + up * -0.625;

        if (!havePrev) {
            prevLeft = left;
            prevRight = right;
            prevMiddle = middle;
            prevLeftTop = leftTop;
            prevRightTop = rightTop;
            prevLeftBottom = leftBottom;
            prevRightBottom = rightBottom;
            havePrev = true;
            continue;
        }

        bool end = i == runtime.segments;
        float railScale = end ? 2.2f : 2.1f;
        Vec3 tieDiff = middle - prevMiddle;
        EmitTie(target, parts.tie, prevMiddle, sample.normal, tieDiff);
        EmitScaled(target, parts.left, prevLeft, sample.normal, left - prevLeft, kTiePivotY, kModelZNudge, railScale);
        EmitScaled(target, parts.right, prevRight, sample.normal, right - prevRight, kTiePivotY, kModelZNudge, railScale);

        if (bc.girder) {
            float beamScale = end ? 2.3f : 2.2f;
            float yOff = GirderYOffset(i);
            Vec3 leftBeam = (leftTop + leftBottom) * 0.5;
            Vec3 rightBeam = (rightTop + rightBottom) * 0.5;
            Vec3 prevLeftBeam = (prevLeftTop + prevLeftBottom) * 0.5;
            Vec3 prevRightBeam = (prevRightTop + prevRightBottom) * 0.5;
            EmitScaled(target, parts.girderMiddle, prevLeftBeam, sample.normal, leftBeam - prevLeftBeam, yOff, kModelZNudge, beamScale);
            EmitScaled(target, parts.girderMiddle, prevRightBeam, sample.normal, rightBeam - prevRightBeam, yOff, kModelZNudge, beamScale);
            EmitScaled(target, parts.girderTop, prevLeftTop, sample.normal, leftTop - prevLeftTop, yOff, kModelZNudge, beamScale);
            EmitScaled(target, parts.girderTop, prevRightTop, sample.normal, rightTop - prevRightTop, yOff, kModelZNudge, beamScale);
            EmitScaled(target, parts.girderBottom, prevLeftBottom, sample.normal, leftBottom - prevLeftBottom, yOff, kModelZNudge, beamScale);
            EmitScaled(target, parts.girderBottom, prevRightBottom, sample.normal, rightBottom - prevRightBottom, yOff, kModelZNudge, beamScale);
        }

        prevLeft = left;
        prevRight = right;
        prevMiddle = middle;
        prevLeftTop = leftTop;
        prevRightTop = rightTop;
        prevLeftBottom = leftBottom;
        prevRightBottom = rightBottom;
    }
}

} // namespace

void AppendCreateCurvedTrack(const string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model) {
    if (LocalId(blockName) != "track" || !blockEntityNbt) return;
    auto connections = getChildByName(blockEntityNbt, "Connections");
    if (!connections || connections->type != TagType::LIST) return;

    for (const auto& entry : connections->children) {
        if (!entry || entry->type != TagType::COMPOUND) continue;
        // TrackRenderer.renderBezierTurn returns immediately when Primary is false,
        // so each curve is drawn once, from the primary end.
        if (!ReadBool(entry, "Primary")) continue;
        Connection bc;
        if (!ReadCouple(entry, "Starts", bc.starts[0], bc.starts[1])) continue;
        if (!ReadCouple(entry, "Axes", bc.axes[0], bc.axes[1])) continue;
        if (!ReadCouple(entry, "Normals", bc.normals[0], bc.normals[1])) continue;
        bc.primary = true;
        bc.girder = ReadBool(entry, "Girder");
        bc.material = ReadString(entry, "Material");
        if (bc.material.empty()) bc.material = "create:andesite";
        EmitConnection(model, bc, *PartsFor(bc.material));
    }
}
