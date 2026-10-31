#include "CableMesher.h"
#include "GlobalCache.h"
#include "block.h"
#include "Occlusion.h"
#include "blockstate.h"
#include "texture.h"
#include "model.h"
#include "include/json.hpp"
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace {

std::mutex g_mu;
std::vector<ICableMesher*> g_meshers;

enum class ConnType : int { NORMAL = 0, PUSH = 1, PULL = 2, NONE = 3 };

static const char* kSideNames[6] = { "down", "up", "north", "south", "west", "east" };
static const int kSideDelta[6][3] = {
    {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}
};

static std::string StripState(std::string s) {
    size_t br = s.find('[');
    if (br != std::string::npos) s = s.substr(0, br);
    return s;
}

static std::string BaseId(const std::string& ns, const std::string& blockName) {
    return ns + ":" + StripState(blockName);
}

static bool IsMekTransmitterId(const std::string& base) {
    // universal_cable / mechanical_pipe / pressurized_tube / logistical_transporter /
    // thermodynamic_conductor / diversion_transporter / restrictive_transporter
    static const char* needles[] = {
        "universal_cable", "mechanical_pipe", "pressurized_tube",
        "logistical_transporter", "thermodynamic_conductor",
        "diversion_transporter", "restrictive_transporter"
    };
    if (base.rfind("mekanism:", 0) != 0) return false;
    for (const char* n : needles) {
        if (base.find(n) != std::string::npos) return true;
    }
    return false;
}

static bool SameTransmitterFamily(const std::string& a, const std::string& b) {
    if (!IsMekTransmitterId(a) || !IsMekTransmitterId(b)) return false;
    auto kind = [](const std::string& id) -> std::string {
        for (const char* n : {
            "universal_cable", "mechanical_pipe", "pressurized_tube",
            "logistical_transporter", "thermodynamic_conductor",
            "diversion_transporter", "restrictive_transporter"
        }) {
            if (id.find(n) != std::string::npos) return n;
        }
        return {};
    };
    return kind(a) == kind(b) && !kind(a).empty();
}

static bool IsLargeTransmitter(const std::string& base) {
    return base.find("mechanical_pipe") != std::string::npos ||
           base.find("logistical_transporter") != std::string::npos ||
           base.find("diversion_transporter") != std::string::npos ||
           base.find("restrictive_transporter") != std::string::npos;
}

static ConnType ConnFromOrdinal(int v) {
    if (v < 0 || v > 3) return ConnType::NONE;
    return static_cast<ConnType>(v);
}

static const char* ConnSuffix(ConnType t) {
    switch (t) {
    case ConnType::NORMAL: return "NORMAL";
    case ConnType::PUSH: return "PUSH";
    case ConnType::PULL: return "PULL";
    default: return "NONE";
    }
}

static Material MakeTexMaterial(const std::string& texture) {
    std::string ns = "minecraft", path = texture;
    size_t colon = texture.find(':');
    if (colon != std::string::npos) {
        ns = texture.substr(0, colon);
        path = texture.substr(colon + 1);
    }
    std::string saved = "textures/" + ns + "/" + path + ".png";
    std::string root = "textures";
    if (SaveTextureToFile(ns, path, root)) RegisterTexture(ns, path, saved);
    Material m(texture, saved, -1);
    m.type = DetectMaterialType(ns, path, m.aspectRatio);
    return m;
}

static std::string ResolveTexRef(const nlohmann::json& modelJson, std::string value) {
    std::unordered_set<std::string> visited;
    while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
        std::string key = value.substr(1);
        if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) {
            // Opaque variants often omitted; fall back to non-opaque slots.
            if (key == "center_opaque") key = "center_down";
            else if (key == "side_opaque") key = "side";
            else if (key.rfind("center_", 0) == 0) key = "center_down";
            else return "";
            if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
        }
        value = modelJson["textures"][key].get<std::string>();
    }
    return value;
}

static std::string GetModelAsset(const std::string& ns, const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    auto index = GlobalCache::modelAssetIndex.find("modelassets:" + ns + ":" + path);
    if (index == GlobalCache::modelAssetIndex.end()) return "";
    auto asset = GlobalCache::modelAssets.find(index->second);
    return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
}

// Parse OBJ text, keep only objects whose names are in `keep`.
static ModelData BakeObjParts(const std::string& objText, const std::string& mtlText,
                              const nlohmann::json& modelJson,
                              const std::unordered_set<std::string>& keep) {
    std::unordered_map<std::string, std::string> mtlTextures;
    {
        std::stringstream mtlStream(mtlText);
        std::string line, materialName;
        while (std::getline(mtlStream, line)) {
            if (line.rfind("newmtl ", 0) == 0) materialName = line.substr(7);
            else if (line.rfind("map_Kd ", 0) == 0 && !materialName.empty()) {
                mtlTextures[materialName] = ResolveTexRef(modelJson, line.substr(7));
            }
        }
    }

    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 2>> uvs;
    ModelData result;
    std::unordered_map<std::string, int> materialIndices;
    std::string currentMaterial;
    std::string currentObject;
    bool include = false;

    // Mek transmitter OBJ uses Minecraft UV (V=0 at top). WI/Blender wants V=0 at bottom.
    const bool flipV = true;

    std::stringstream objStream(objText);
    std::string line;
    while (std::getline(objStream, line)) {
        if (line.empty()) continue;
        std::stringstream values(line);
        std::string type;
        values >> type;
        if (type == "o" || type == "g") {
            values >> currentObject;
            include = keep.count(currentObject) > 0;
        } else if (type == "v") {
            std::array<float, 3> value{};
            values >> value[0] >> value[1] >> value[2];
            positions.push_back(value);
        } else if (type == "vt") {
            std::array<float, 2> value{};
            values >> value[0] >> value[1];
            if (flipV) value[1] = 1.0f - value[1];
            uvs.push_back(value);
        } else if (type == "usemtl") {
            values >> currentMaterial;
        } else if (type == "f" && include) {
            std::vector<std::pair<int, int>> corners;
            std::string corner;
            while (values >> corner) {
                size_t slash = corner.find('/');
                int vertex = std::stoi(corner.substr(0, slash)) - 1;
                size_t nextSlash = slash == std::string::npos ? std::string::npos
                    : corner.find('/', slash + 1);
                int uv = slash == std::string::npos ? -1
                    : std::stoi(corner.substr(slash + 1, nextSlash - slash - 1)) - 1;
                corners.emplace_back(vertex, uv);
            }
            if (corners.size() < 3) continue;
            int materialIndex;
            auto mit = materialIndices.find(currentMaterial);
            if (mit == materialIndices.end()) {
                std::string texture = mtlTextures.count(currentMaterial)
                    ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                if (texture.empty()) texture = "minecraft:block/missingno";
                materialIndex = static_cast<int>(result.materials.size());
                result.materials.push_back(MakeTexMaterial(texture));
                materialIndices[currentMaterial] = materialIndex;
            } else materialIndex = mit->second;

            for (size_t tri = 1; tri + 1 < corners.size(); ++tri) {
                std::array<std::pair<int, int>, 4> quad = {
                    corners[0], corners[tri], corners[tri + 1], corners[tri + 1]
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
                    } else {
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

static std::array<ConnType, 6> InferMekConnections(const std::string& selfBase,
                                                    int x, int y, int z,
                                                    const NbtTagPtr& beNbt) {
    std::array<ConnType, 6> conns;
    conns.fill(ConnType::NONE);

    // Prefer BE connection0..5 when any side is PUSH/PULL (explicit); otherwise
    // neighbor-infer NORMAL/NONE so snapped worlds without a tick still mesh.
    bool anyPushPull = false;
    std::array<ConnType, 6> fromBe;
    fromBe.fill(ConnType::NONE);
    if (beNbt) {
        for (int i = 0; i < 6; ++i) {
            auto tag = getChildByName(beNbt, "connection" + std::to_string(i));
            if (!tag) continue;
            int v = 3;
            if (tag->type == TagType::INT) v = bytesToInt(tag->payload);
            else if (tag->type == TagType::BYTE) v = static_cast<unsigned char>(bytesToByte(tag->payload));
            fromBe[i] = ConnFromOrdinal(v);
            if (fromBe[i] == ConnType::PUSH || fromBe[i] == ConnType::PULL) anyPushPull = true;
        }
    }

    for (int i = 0; i < 6; ++i) {
        if (anyPushPull && (fromBe[i] == ConnType::PUSH || fromBe[i] == ConnType::PULL)) {
            conns[i] = fromBe[i];
            continue;
        }
        int nx = x + kSideDelta[i][0];
        int ny = y + kSideDelta[i][1];
        int nz = z + kSideDelta[i][2];
        int nid = GetBlockId(nx, ny, nz);
        if (nid < 0) { conns[i] = ConnType::NONE; continue; }
        Block nb = GetBlockById(nid);
        std::string nbBase = nb.GetNameAndNameSpaceWithoutState();
        if (nbBase == "minecraft:air" || nbBase.empty()) {
            conns[i] = ConnType::NONE;
            continue;
        }
        if (SameTransmitterFamily(selfBase, nbBase)) {
            conns[i] = ConnType::NORMAL;
            continue;
        }
        // Acceptor: any non-air Mek block, or any solid-ish neighbor.
        if (nbBase.rfind("mekanism:", 0) == 0) {
            conns[i] = ConnType::NORMAL;
            continue;
        }
        const BlockOcclusion& occ = GetBlockOcclusion(nid);
        if (occ.solidLike || occ.occludes) {
            conns[i] = ConnType::NORMAL;
            continue;
        }
        conns[i] = ConnType::NONE;
    }
    return conns;
}

static std::string TransmitterModelJsonPath(const std::string& base) {
    // Resolve via blockstate variants[""].model → "block/transmitter/..."
    size_t colon = base.find(':');
    std::string ns = colon == std::string::npos ? "mekanism" : base.substr(0, colon);
    std::string local = colon == std::string::npos ? base : base.substr(colon + 1);
    nlohmann::json bs = GetBlockstateJson(ns, local);
    if (!bs.is_null() && bs.contains("variants") && bs["variants"].is_object()) {
        const auto& variants = bs["variants"];
        nlohmann::json entry;
        if (variants.contains("")) entry = variants[""];
        else if (!variants.empty()) entry = variants.begin().value();
        if (entry.is_array() && !entry.empty()) entry = entry[0];
        if (entry.is_object() && entry.contains("model") && entry["model"].is_string()) {
            std::string model = entry["model"].get<std::string>();
            size_t c = model.find(':');
            if (c != std::string::npos) model = model.substr(c + 1);
            if (model.rfind("block/", 0) != 0 && model.rfind("item/", 0) != 0)
                model = "block/" + model;
            return model;
        }
    }
    return "block/" + local;
}


static std::string ResolveTransmitterObjPath(nlohmann::json modelJson) {
    // MergeModelJson does not copy custom-loader "model" from parents; walk chain.
    std::unordered_set<std::string> seen;
    for (int depth = 0; depth < 16; ++depth) {
        if (modelJson.contains("model") && modelJson["model"].is_string()) {
            return modelJson["model"].get<std::string>();
        }
        if (!modelJson.contains("parent") || !modelJson["parent"].is_string()) break;
        std::string parentId = modelJson["parent"].get<std::string>();
        if (!seen.insert(parentId).second) break;
        std::string pns = "minecraft", pid = parentId;
        size_t c = parentId.find(':');
        if (c != std::string::npos) { pns = parentId.substr(0, c); pid = parentId.substr(c + 1); }
        nlohmann::json parent = GetModelJson(pns, pid);
        if (parent.is_null()) break;
        // Keep child textures; only need parent's model path.
        if (parent.contains("model") && parent["model"].is_string())
            return parent["model"].get<std::string>();
        modelJson = parent;
    }
    return {};
}

class MekTransmitterMesher : public ICableMesher {
public:
    const char* Name() const override { return "mekanism_transmitter"; }
    bool AppliesTo(const std::string& ns, const std::string& blockName) const override {
        return IsMekTransmitterId(BaseId(ns, blockName));
    }
    bool TryGenerate(ModelData& outModel, const std::string& ns,
                     const std::string& blockName, int x, int y, int z,
                     const NbtTagPtr& beNbt) override {
        std::string base = BaseId(ns, blockName);
        auto conns = InferMekConnections(base, x, y, z, beNbt);

        std::string jsonPath = TransmitterModelJsonPath(base);
        nlohmann::json modelJson = GetModelJson("mekanism", jsonPath);
        // Keep textures from child; resolve OBJ path via parent chain (loader models).
        nlohmann::json textured = modelJson;
        modelJson = LoadParentModel("mekanism", jsonPath, modelJson);
        std::string objPath = ResolveTransmitterObjPath(textured);
        if (objPath.empty()) objPath = ResolveTransmitterObjPath(modelJson);
        if (objPath.empty()) {
            return false;
        }
        // Prefer merged textures (child overrides).
        if (textured.contains("textures")) modelJson["textures"] = textured["textures"];
        size_t colon = objPath.find(':');
        if (colon != std::string::npos) objPath = objPath.substr(colon + 1);
        if (objPath.rfind("models/", 0) == 0) objPath = objPath.substr(7);

        // Prefer .obj.mek asset; fall back to stripping suffix for cache key variants.
        std::string obj = GetModelAsset("mekanism", objPath);
        if (obj.empty() && objPath.size() > 8 && objPath.ends_with(".obj.mek")) {
            obj = GetModelAsset("mekanism", objPath.substr(0, objPath.size() - 4)); // .mek stripped → .obj
        }

        std::string mtlName = "transmitter_small.mtl";
        if (IsLargeTransmitter(base)) mtlName = "transmitter_large.mtl";
        // Try to read mtllib from obj
        {
            std::stringstream probe(obj);
            std::string line;
            while (std::getline(probe, line)) {
                if (line.rfind("mtllib ", 0) == 0) {
                    mtlName = line.substr(7);
                    // trim CR
                    while (!mtlName.empty() && (mtlName.back() == '\r' || mtlName.back() == ' '))
                        mtlName.pop_back();
                    break;
                }
            }
        }
        std::string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
        std::string mtl = GetModelAsset("mekanism", directory + mtlName);
        if (mtl.empty()) mtl = GetModelAsset("mekanism", "block/" + mtlName);

        std::unordered_set<std::string> keep;
        for (int i = 0; i < 6; ++i) {
            keep.insert(std::string(kSideNames[i]) + ConnSuffix(conns[i]));
        }

        outModel = BakeObjParts(obj, mtl, modelJson, keep);
        for (auto& f : outModel.faces) f.faceDirection = FaceType::DO_NOT_CULL;
        return true;
    }
};

MekTransmitterMesher g_mekMesher;

// --- Thermal Dynamics ducts (custom loader; JSON has all arm groups) ---
static bool IsThermalDuctId(const std::string& base) {
    return base == "thermal:energy_duct" ||
           base == "thermal:fluid_duct" ||
           base == "thermal:fluid_duct_windowed";
}

static bool IsThermalDuctNeighbor(const std::string& id) {
    std::string b = StripState(id);
    if (IsThermalDuctId(b)) return true;
    // Attach to machines / any solid thermal block lightly
    if (b.rfind("thermal:", 0) == 0) return true;
    return false;
}

static ModelData MakeAxisCuboid(float x0, float y0, float z0, float x1, float y1, float z1,
                                const Material& mat) {
    ModelData m;
    m.materials.push_back(mat);
    m.vertices = {
        x0,y1,z0, x1,y1,z0, x1,y1,z1, x0,y1,z1,
        x0,y0,z1, x1,y0,z1, x1,y0,z0, x0,y0,z0,
        x1,y0,z0, x1,y1,z0, x1,y1,z1, x1,y0,z1,
        x0,y0,z1, x0,y1,z1, x0,y1,z0, x0,y0,z0,
        x0,y0,z0, x1,y0,z0, x1,y1,z0, x0,y1,z0,
        x1,y0,z1, x0,y0,z1, x0,y1,z1, x1,y1,z1
    };
    m.uvCoordinates = {
        x0,z0, x1,z0, x1,z1, x0,z1,
        x0,z1, x1,z1, x1,z0, x0,z0,
        z0,y0, z0,y1, z1,y1, z1,y0,
        z1,y0, z1,y1, z0,y1, z0,y0,
        x0,y0, x1,y0, x1,y1, x0,y1,
        x1,y0, x0,y0, x0,y1, x1,y1
    };
    struct FaceInfo { FaceType type; int vi[4]; };
    const FaceInfo infos[] = {
        {FaceType::UP,{0,1,2,3}},{FaceType::DOWN,{4,5,6,7}},
        {FaceType::EAST,{8,9,10,11}},{FaceType::WEST,{12,13,14,15}},
        {FaceType::NORTH,{16,17,18,19}},{FaceType::SOUTH,{20,21,22,23}},
    };
    for (const auto& info : infos) {
        Face f{}; f.faceDirection = info.type; f.materialIndex = 0;
        for (int i = 0; i < 4; ++i) { f.vertexIndices[i] = info.vi[i]; f.uvIndices[i] = info.vi[i]; }
        m.faces.push_back(f);
    }
    return m;
}

class ThermalDuctMesher : public ICableMesher {
public:
    const char* Name() const override { return "thermal_duct"; }
    bool AppliesTo(const std::string& ns, const std::string& blockName) const override {
        return IsThermalDuctId(BaseId(ns, blockName));
    }
    bool TryGenerate(ModelData& outModel, const std::string& ns,
                     const std::string& blockName, int x, int y, int z,
                     const NbtTagPtr&) override {
        std::string base = BaseId(ns, blockName);
        std::string texPath = "block/ducts/energy_duct";
        if (base.find("fluid_duct_windowed") != std::string::npos)
            texPath = "block/ducts/fluid_duct_windowed";
        else if (base.find("fluid_duct") != std::string::npos)
            texPath = "block/ducts/fluid_duct";
        // fluid_duct.png may not exist — fall back
        Material mat = MakeTexMaterial("thermal:" + texPath);
        if (mat.texturePath.empty() || mat.texturePath == "None")
            mat = MakeTexMaterial("thermal:block/ducts/energy_duct");

        bool conn[6] = {};
        for (int i = 0; i < 6; ++i) {
            int nx = x + kSideDelta[i][0], ny = y + kSideDelta[i][1], nz = z + kSideDelta[i][2];
            int id = GetBlockId(nx, ny, nz);
            if (id <= 0) continue;
            Block nb = GetBlockById(id);
            std::string bn = nb.GetNameAndNameSpaceWithoutState();
            if (bn.empty() || bn == "minecraft:air" || bn == "minecraft:cave_air" ||
                bn == "minecraft:void_air") continue;
            if (IsThermalDuctNeighbor(bn)) { conn[i] = true; continue; }
            if (!nb.HasFluid()) conn[i] = true;
        }

        // Center 5/16..11/16
        const float a = 5.f/16.f, b = 11.f/16.f;
        outModel = MakeAxisCuboid(a, a, a, b, b, b, mat);
        auto append = [&](ModelData part) {
            // merge materials carefully
            int baseMat = (int)outModel.materials.size();
            for (auto& m : part.materials) outModel.materials.push_back(m);
            size_t vOff = outModel.vertices.size() / 3;
            size_t uvOff = outModel.uvCoordinates.size() / 2;
            outModel.vertices.insert(outModel.vertices.end(), part.vertices.begin(), part.vertices.end());
            outModel.uvCoordinates.insert(outModel.uvCoordinates.end(), part.uvCoordinates.begin(), part.uvCoordinates.end());
            for (auto f : part.faces) {
                f.materialIndex += baseMat;
                for (int i = 0; i < 4; ++i) {
                    f.vertexIndices[i] += (int)vOff;
                    f.uvIndices[i] += (int)uvOff;
                }
                outModel.faces.push_back(f);
            }
        };
        // Arms: down,up,north,south,west,east
        if (conn[0]) append(MakeAxisCuboid(a, 0, a, b, a, b, mat));
        if (conn[1]) append(MakeAxisCuboid(a, b, a, b, 1, b, mat));
        if (conn[2]) append(MakeAxisCuboid(a, a, 0, b, b, a, mat));
        if (conn[3]) append(MakeAxisCuboid(a, a, b, b, b, 1, mat));
        if (conn[4]) append(MakeAxisCuboid(0, a, a, a, b, b, mat));
        if (conn[5]) append(MakeAxisCuboid(b, a, a, 1, b, b, mat));
        for (auto& f : outModel.faces) f.faceDirection = FaceType::DO_NOT_CULL;
        return !outModel.faces.empty();
    }
};

ThermalDuctMesher g_thermalDuctMesher;

// --- AE2 cable_bus: neighbor-inferred thin cable (glass-cable size) ---
static bool IsAe2CableBus(const std::string& base) {
    return base == "ae2:cable_bus";
}

class Ae2CableMesher : public ICableMesher {
public:
    const char* Name() const override { return "ae2_cable"; }
    bool AppliesTo(const std::string& ns, const std::string& blockName) const override {
        return IsAe2CableBus(BaseId(ns, blockName));
    }
    bool TryGenerate(ModelData& outModel, const std::string& ns,
                     const std::string& blockName, int x, int y, int z,
                     const NbtTagPtr&) override {
        Material mat = MakeTexMaterial("ae2:part/cable/glass/white");
        // MakeTexMaterial name is the texture id; texturePath is textures/...
        if (mat.texturePath.empty() || mat.texturePath == "None")
            mat = MakeTexMaterial("ae2:block/fluix_block");
        const float a = 6.f/16.f, b = 10.f/16.f; // glass cable ~4px
        bool conn[6] = {};
        for (int i = 0; i < 6; ++i) {
            int nx = x + kSideDelta[i][0], ny = y + kSideDelta[i][1], nz = z + kSideDelta[i][2];
            int id = GetBlockId(nx, ny, nz);
            if (id <= 0) continue;
            Block nb = GetBlockById(id);
            std::string bn = nb.GetNameAndNameSpaceWithoutState();
            if (bn == "ae2:cable_bus" || bn.rfind("ae2:", 0) == 0) conn[i] = true;
        }
        outModel = MakeAxisCuboid(a, a, a, b, b, b, mat);
        auto append = [&](ModelData part) {
            int baseMat = (int)outModel.materials.size();
            for (auto& m : part.materials) outModel.materials.push_back(m);
            size_t vOff = outModel.vertices.size() / 3;
            size_t uvOff = outModel.uvCoordinates.size() / 2;
            outModel.vertices.insert(outModel.vertices.end(), part.vertices.begin(), part.vertices.end());
            outModel.uvCoordinates.insert(outModel.uvCoordinates.end(), part.uvCoordinates.begin(), part.uvCoordinates.end());
            for (auto f : part.faces) {
                f.materialIndex += baseMat;
                for (int i = 0; i < 4; ++i) {
                    f.vertexIndices[i] += (int)vOff;
                    f.uvIndices[i] += (int)uvOff;
                }
                outModel.faces.push_back(f);
            }
        };
        if (conn[0]) append(MakeAxisCuboid(a, 0, a, b, a, b, mat));
        if (conn[1]) append(MakeAxisCuboid(a, b, a, b, 1, b, mat));
        if (conn[2]) append(MakeAxisCuboid(a, a, 0, b, b, a, mat));
        if (conn[3]) append(MakeAxisCuboid(a, a, b, b, b, 1, mat));
        if (conn[4]) append(MakeAxisCuboid(0, a, a, a, b, b, mat));
        if (conn[5]) append(MakeAxisCuboid(b, a, a, 1, b, b, mat));
        for (auto& f : outModel.faces) f.faceDirection = FaceType::DO_NOT_CULL;
        return true;
    }
};

Ae2CableMesher g_ae2CableMesher;

// --- Extreme Reactors casing: swap cube texture by neighbor count (approx) ---
static bool IsErCasing(const std::string& base) {
    return base.find("bigreactors:") == 0 &&
           (base.find("reactorcasing") != std::string::npos ||
            base.find("turbinecasing") != std::string::npos ||
            base.find("energizercasing") != std::string::npos ||
            base.find("fluidizercasing") != std::string::npos);
}

class ErCasingMesher : public ICableMesher {
public:
    const char* Name() const override { return "er_casing"; }
    bool AppliesTo(const std::string& ns, const std::string& blockName) const override {
        return IsErCasing(BaseId(ns, blockName));
    }
    bool TryGenerate(ModelData& outModel, const std::string& ns,
                     const std::string& blockName, int x, int y, int z,
                     const NbtTagPtr&) override {
        // Prefer static model path; pick connected face texture when surrounded.
        std::string local = StripState(blockName);
        std::string family = "reactor";
        if (local.find("turbine") != std::string::npos) family = "turbine";
        else if (local.find("energizer") != std::string::npos) family = "energizer";
        else if (local.find("fluidizer") != std::string::npos) family = "fluidizer";
        std::string tier = "basic";
        if (local.rfind("reinforced_", 0) == 0 || local.find("reinforced") != std::string::npos)
            tier = "reinforced";

        int same = 0;
        for (int i = 0; i < 6; ++i) {
            int nx = x + kSideDelta[i][0], ny = y + kSideDelta[i][1], nz = z + kSideDelta[i][2];
            int id = GetBlockId(nx, ny, nz);
            if (id <= 0) continue;
            Block nb = GetBlockById(id);
            std::string bn = nb.GetNameAndNameSpaceWithoutState();
            if (IsErCasing(bn) || bn.rfind("bigreactors:", 0) == 0) ++same;
        }

        // Assembled face uses plating; frames use directional casing_* sheets.
        std::string tex = "bigreactors:block/" + family + "/" + tier + "/casing_single";
        if (same >= 4) tex = "bigreactors:block/" + family + "/" + tier + "/plating";
        else if (same >= 2) tex = "bigreactors:block/" + family + "/" + tier + "/casing_up";

        Material mat = MakeTexMaterial(tex);
        // Fallback to whatever the blockstate model already provides
        ModelData baseModel = GetRandomModelFromCache(ns, blockName);
        if (baseModel.faces.empty()) {
            outModel = MakeAxisCuboid(0, 0, 0, 1, 1, 1, mat);
        } else {
            outModel = baseModel;
            for (auto& m : outModel.materials) {
                if (m.texturePath.empty() || m.texturePath == "None") continue;
                m.texturePath = mat.texturePath;
                m.name += "@er_casing";
            }
        }
        return !outModel.faces.empty();
    }
};

ErCasingMesher g_erCasingMesher;

bool g_inited = false;

} // namespace

void RegisterCableMesher(ICableMesher* mesher) {
    if (!mesher) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_meshers.push_back(mesher);
}

void InitializeCableMeshers() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_inited) return;
    g_meshers.push_back(&g_mekMesher);
    g_meshers.push_back(&g_thermalDuctMesher);
    g_meshers.push_back(&g_ae2CableMesher);
    g_meshers.push_back(&g_erCasingMesher);
    g_inited = true;
    std::cout << "[CableMesher] registered " << g_meshers.size()
              << " meshers (Mek/Thermal/AE2/ER)" << std::endl;
}

bool TryGenerateCableModel(ModelData& outModel, const std::string& ns,
                           const std::string& blockName, int x, int y, int z,
                           const NbtTagPtr& beNbt) {
    if (!g_inited) InitializeCableMeshers();
    std::vector<ICableMesher*> snap;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        snap = g_meshers;
    }
    for (auto* m : snap) {
        if (m && m->AppliesTo(ns, blockName)) {
            if (m->TryGenerate(outModel, ns, blockName, x, y, z, beNbt)) return true;
        }
    }
    return false;
}
