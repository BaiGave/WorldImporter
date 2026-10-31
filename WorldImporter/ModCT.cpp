#include "ModCT.h"
#include "CTM.h"
#include "GlobalCache.h"
#include "blockstate.h"
#include "nbtutils.h"
#include "blocktint.h"
#include <atomic>

#include "include/json.hpp"
#include "include/stb_image.h"
#include "include/stb_image_write.h"
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <iostream>
#include <fstream>
#include <vector>
#include <array>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cmath>

namespace {

std::mutex g_mu;
std::vector<IModCTProvider*> g_providers;

std::unordered_map<std::string, std::unordered_set<std::string>> g_blockConnect;
std::unordered_map<std::string, std::unordered_set<std::string>> g_texConnect;

// Packed block pos → camo block id (ns:path without state)
std::unordered_map<uint64_t, std::string> g_posCamo;

static uint64_t PackPos(int x, int y, int z) {
    // 21 bits each for x/z (signed via bias), 22 for y
    auto pack = [](int v, int bits) -> uint64_t {
        uint64_t mask = (1ull << bits) - 1ull;
        return static_cast<uint64_t>(v) & mask;
    };
    return (pack(x, 21) << 43) | (pack(y, 22) << 21) | pack(z, 21);
}

static void AddDirected(std::unordered_map<std::string, std::unordered_set<std::string>>& map,
                        const std::string& from, const std::string& to) {
    map[from].insert(to);
}

static void AddGroup(std::unordered_map<std::string, std::unordered_set<std::string>>& map,
                     const std::vector<std::string>& ids) {
    for (const auto& a : ids) {
        for (const auto& b : ids) AddDirected(map, a, b);
    }
}

static void LoadConnectFromMcmeta() {
    // extra.connect_to 只有 ConnectedTexturesMod 会读
    if (!config.mcmetaCtmEnabled) return;
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    for (const auto& entry : GlobalCache::mcmetaIndex) {
        auto it = GlobalCache::mcmetaCache.find(entry.second);
        if (it == GlobalCache::mcmetaCache.end() || !it->second.is_object()) continue;
        if (!it->second.contains("ctm") || !it->second["ctm"].is_object()) continue;
        const auto& ctm = it->second["ctm"];
        if (ctm.value("ctm_version", 0) != 1) continue;
        if (!ctm.contains("extra") || !ctm["extra"].is_object()) continue;
        const auto& extra = ctm["extra"];
        if (!extra.contains("connect_to") || !extra["connect_to"].is_array()) continue;

        std::string idx = entry.first;
        const std::string prefix = "mcmetas:";
        if (idx.rfind(prefix, 0) != 0) continue;
        std::string nsPath = idx.substr(prefix.size());
        size_t colon = nsPath.find(':');
        if (colon == std::string::npos) continue;
        std::string texNs = nsPath.substr(0, colon);
        std::string texPath = nsPath.substr(colon + 1);
        if (texPath.size() > 4 && texPath.rfind(".png") == texPath.size() - 4)
            texPath = texPath.substr(0, texPath.size() - 4);

        std::unordered_set<std::string> targets;
        for (const auto& item : extra["connect_to"]) {
            if (item.is_object() && item.contains("block") && item["block"].is_string())
                targets.insert(item["block"].get<std::string>());
            else if (item.is_string())
                targets.insert(item.get<std::string>());
        }
        if (targets.empty()) continue;

        g_texConnect[texNs + ":" + texPath] = targets;
        std::vector<std::string> group(targets.begin(), targets.end());
        AddGroup(g_blockConnect, group);
    }
}

static void LoadConnectFromJson() {
    std::ifstream in("config/mod_ct_connect.json");
    if (!in) return;
    nlohmann::json j;
    try { in >> j; } catch (...) { return; }
    if (!j.contains("groups") || !j["groups"].is_array()) return;
    for (const auto& g : j["groups"]) {
        if (!g.is_array()) continue;
        std::vector<std::string> ids;
        for (const auto& x : g) if (x.is_string()) ids.push_back(x.get<std::string>());
        if (ids.size() >= 2) AddGroup(g_blockConnect, ids);
    }
    std::cout << "[ModCT] Loaded connect groups from config/mod_ct_connect.json" << std::endl;
}

struct CamoAppearance {
    bool ok = false;
    bool isFluid = false;
    std::string ns;
    std::string local;      // without state
    std::string fullId;     // ns:local
    Material mat;
    TintResult tint;
};

static bool TextureExists(const std::string& texNs, const std::string& texPathNoPng) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    std::string idxKey = "textures:" + texNs + ":" + texPathNoPng;
    return GlobalCache::textureIndex.count(idxKey) > 0;
}

static CamoAppearance ResolveCamoTag(const NbtTagPtr& camoTag) {
    CamoAppearance out;
    if (!camoTag || camoTag->type != TagType::COMPOUND) return out;

    auto typeTag = getChildByName(camoTag, "type");
    std::string typ;
    if (typeTag && typeTag->type == TagType::STRING)
        typ.assign(typeTag->payload.begin(), typeTag->payload.end());
    if (typ == "framedblocks:empty" || typ.empty()) return out;

    if (typ == "framedblocks:fluid") {
        auto fluidTag = getChildByName(camoTag, "fluid");
        if (!fluidTag || fluidTag->type != TagType::COMPOUND) return out;
        auto nameTag = getChildByName(fluidTag, "Name");
        if (!nameTag || nameTag->type != TagType::STRING) return out;
        std::string fluidFull(nameTag->payload.begin(), nameTag->payload.end());
        if (fluidFull.empty()) return out;
        out.ns = "minecraft";
        out.local = fluidFull;
        size_t colon = fluidFull.find(':');
        if (colon != std::string::npos) {
            out.ns = fluidFull.substr(0, colon);
            out.local = fluidFull.substr(colon + 1);
        }
        size_t br = out.local.find('[');
        if (br != std::string::npos) out.local = out.local.substr(0, br);
        out.fullId = out.ns + ":" + out.local;
        out.isFluid = true;

        // Still textures: water_still / lava_still / <id>_still fallback
        std::string texLocal;
        if (out.local == "water" || out.local == "flowing_water") texLocal = "block/water_still";
        else if (out.local == "lava" || out.local == "flowing_lava") texLocal = "block/lava_still";
        else texLocal = "block/" + out.local + "_still";

        if (!TextureExists(out.ns, texLocal) && out.ns == "minecraft") {
            // Some mods register fluid without _still; try block/<id>
            texLocal = "block/" + out.local;
        }
        out.mat.name = out.fullId;
        out.mat.texturePath = "textures/" + out.ns + "/" + texLocal + ".png";
        out.mat.textureKey = "all";
        if (out.local.find("water") != std::string::npos) {
            out.tint.kind = TintKind::Water;
        }
        out.ok = TextureExists(out.ns, texLocal);
        if (!out.ok) {
            // Soft-accept path even if index miss — SaveTexture may still work later
            out.ok = true;
        }
        return out;
    }

    if (typ != "framedblocks:block") return out;

    auto stateTag = getChildByName(camoTag, "state");
    if (!stateTag || stateTag->type != TagType::COMPOUND) return out;
    auto nameTag = getChildByName(stateTag, "Name");
    if (!nameTag || nameTag->type != TagType::STRING) return out;

    std::string camoFull(nameTag->payload.begin(), nameTag->payload.end());
    if (camoFull.empty()) return out;

    auto propsTag = getChildByName(stateTag, "Properties");
    if (propsTag && propsTag->type == TagType::COMPOUND && !propsTag->children.empty()) {
        std::string props;
        for (const auto& p : propsTag->children) {
            if (!props.empty()) props += ",";
            props += p->name + "=" + std::string(p->payload.begin(), p->payload.end());
        }
        if (!props.empty()) camoFull += "[" + props + "]";
    }

    out.ns = "minecraft";
    std::string camoLocal = camoFull;
    size_t colon = camoFull.find(':');
    if (colon != std::string::npos) {
        out.ns = camoFull.substr(0, colon);
        camoLocal = camoFull.substr(colon + 1);
    }
    out.local = camoLocal;
    size_t br0 = out.local.find('[');
    if (br0 != std::string::npos) out.local = out.local.substr(0, br0);
    out.fullId = out.ns + ":" + out.local;

    ModelData camoModel = GetRandomModelFromCache(out.ns, camoLocal);
    if (camoModel.materials.empty())
        camoModel = GetRandomModelFromCache(out.ns, out.local);
    if (camoModel.materials.empty() && !blockstateCachesFrozen.load(std::memory_order_acquire)) {
        ProcessBlockstate(out.ns, { out.local });
        camoModel = GetRandomModelFromCache(out.ns, out.local);
    }

    const Material* src = nullptr;
    for (const auto& m : camoModel.materials) {
        if (m.texturePath.empty() || m.texturePath == "None") continue;
        src = &m;
        if (m.texturePath.find("/block/") != std::string::npos) break;
    }
    if (src) {
        out.mat = *src;
    } else {
        out.mat.name = out.fullId;
        out.mat.texturePath = "textures/" + out.ns + "/block/" + out.local + ".png";
        out.mat.textureKey = "all";
        if (!TextureExists(out.ns, "block/" + out.local)) return out;
    }
    out.ok = true;
    return out;
}

static bool IsFramedFrameTex(const std::string& tp) {
    return tp.find("framedblocks:") != std::string::npos ||
           tp.find("framedblocks/") != std::string::npos ||
           tp.find("/framedblocks/") != std::string::npos;
}

static void ApplyAppearanceToFramedMats(ModelData& model, const CamoAppearance& app,
                                        const char* tagSuffix) {
    for (auto& mat : model.materials) {
        const std::string& tp = mat.texturePath;
        if (!IsFramedFrameTex(tp)) continue;
        if (tp.find("reinforcement") != std::string::npos) continue;
        if (tp.find("underlay") != std::string::npos) continue;
        if (tp.find("overlay") != std::string::npos) continue;

        mat.texturePath = app.mat.texturePath;
        mat.type = app.mat.type;
        mat.aspectRatio = app.mat.aspectRatio;
        mat.tintIndex = app.mat.tintIndex;
        mat.name = mat.name + tagSuffix + app.ns + "/" + app.local;
        if (app.tint.on()) {
            mat.tint = app.tint;
            mat.tintLocked = true;
            mat.name += TintSuffix(app.tint);
        }
    }
}

// Build a unit cuboid [min..max] with a single material (for double-camo halves).
static ModelData MakeCamoCuboid(float minX, float minY, float minZ,
                                float maxX, float maxY, float maxZ,
                                const CamoAppearance& app, const char* tag) {
    ModelData m;
    Material mat = app.mat;
    mat.name = std::string("framed_camo") + tag + app.ns + "/" + app.local;
    if (app.tint.on()) {
        mat.tint = app.tint;
        mat.tintLocked = true;
        mat.name += TintSuffix(app.tint);
    }
    m.materials.push_back(mat);
    const int mi = 0;

    // Same vertex layout as LittleTiles CreateCube (UP,DOWN,EAST,WEST,NORTH,SOUTH)
    m.vertices = {
        minX, maxY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   minX, maxY, maxZ,
        minX, minY, maxZ,   maxX, minY, maxZ,   maxX, minY, minZ,   minX, minY, minZ,
        maxX, minY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   maxX, minY, maxZ,
        minX, minY, maxZ,   minX, maxY, maxZ,   minX, maxY, minZ,   minX, minY, minZ,
        minX, minY, minZ,   maxX, minY, minZ,   maxX, maxY, minZ,   minX, maxY, minZ,
        maxX, minY, maxZ,   minX, minY, maxZ,   minX, maxY, maxZ,   maxX, maxY, maxZ
    };
    m.uvCoordinates = {
        minX, minZ, maxX, minZ, maxX, maxZ, minX, maxZ,
        minX, maxZ, maxX, maxZ, maxX, minZ, minX, minZ,
        minZ, minY, minZ, maxY, maxZ, maxY, maxZ, minY,
        maxZ, minY, maxZ, maxY, minZ, maxY, minZ, minY,
        minX, minY, maxX, minY, maxX, maxY, minX, maxY,
        maxX, minY, minX, minY, minX, maxY, maxX, maxY
    };
    struct FaceInfo { FaceType type; std::array<int,4> vi; };
    const FaceInfo infos[] = {
        {FaceType::UP,    {0,1,2,3}},
        {FaceType::DOWN,  {4,5,6,7}},
        {FaceType::EAST,  {8,9,10,11}},
        {FaceType::WEST,  {12,13,14,15}},
        {FaceType::NORTH, {16,17,18,19}},
        {FaceType::SOUTH, {20,21,22,23}},
    };
    for (const auto& info : infos) {
        Face f{};
        f.faceDirection = info.type;
        f.materialIndex = mi;
        for (int i = 0; i < 4; ++i) {
            f.vertexIndices[i] = info.vi[i];
            f.uvIndices[i] = info.vi[i];
        }
        m.faces.push_back(f);
    }
    return m;
}

static int DoubleSplitAxis(const std::string& blockLocal) {
    // 0=X, 1=Y, 2=Z. Default Y (slab-like).
    std::string b = blockLocal;
    size_t br = b.find('[');
    if (br != std::string::npos) b = b.substr(0, br);
    if (b.find("panel") != std::string::npos) return 2; // Z split for panels
    if (b.find("slab") != std::string::npos || b.find("stairs") != std::string::npos) return 1;
    if (b.find("double") != std::string::npos) return 1;
    return 1;
}

} // namespace


static bool LoadTexturePngBytes(const std::string& texNs, const std::string& texPathNoPng,
                                std::vector<unsigned char>& out) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    std::string idxKey = "textures:" + texNs + ":" + texPathNoPng;
    auto it = GlobalCache::textureIndex.find(idxKey);
    if (it == GlobalCache::textureIndex.end()) return false;
    auto t = GlobalCache::textures.find(it->second);
    if (t == GlobalCache::textures.end()) return false;
    out = t->second;
    return !out.empty();
}

static bool ParseTexId(const std::string& id, std::string& ns, std::string& path) {
    size_t c = id.find(':');
    if (c == std::string::npos) {
        ns = "minecraft";
        path = id;
    } else {
        ns = id.substr(0, c);
        path = id.substr(c + 1);
    }
    if (path.size() > 4 && path.rfind(".png") == path.size() - 4)
        path = path.substr(0, path.size() - 4);
    if (path.rfind("textures/", 0) == 0) path = path.substr(9);
    return !path.empty();
}

static void EncodePngWrite(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(context);
    const auto* bytes = static_cast<const unsigned char*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

static std::vector<unsigned char> EncodeRgbaPng(const std::vector<unsigned char>& rgba, int w, int h) {
    std::vector<unsigned char> out;
    if (!stbi_write_png_to_func(EncodePngWrite, &out, w, h, 4, rgba.data(), w * 4))
        return {};
    return out;
}

// Athena ctm_textures → Continuity compact roles:
//   0 particle (unconnected), 1 empty (full), 2 vertical, 3 horizontal, 4 center (corners)
// Pillar / limited_pillar / pane_pillar → Continuity vertical (4 tiles):
//   Continuity SelectVerticalTile: up&&down→1, up→0, down→2, none→3
//   Athena indices: both→center, up-only→bottom, down-only→top, none→self
// Giant → Continuity repeat (width×height tiles keyed "1".."N")
static int RegisterAthenaFromBlockstates() {
    static const char* kCompactLoaders[] = {
        "athena:ctm", "athena:pane_ctm", "athena:carpet_ctm"
    };
    static const char* kPillarLoaders[] = {
        "athena:pillar", "athena:limited_pillar", "athena:pane_pillar"
    };
    int registered = 0;
    std::vector<std::pair<std::string, nlohmann::json>> entries;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        for (const auto& kv : GlobalCache::blockstateIndex) {
            const std::string& ik = kv.first;
            const std::string prefix = "blockstates:";
            if (ik.rfind(prefix, 0) != 0) continue;
            auto it = GlobalCache::blockstates.find(kv.second);
            if (it == GlobalCache::blockstates.end() || !it->second.is_object()) continue;
            entries.emplace_back(ik.substr(prefix.size()), it->second);
        }
    }
    for (const auto& e : entries) {
        const nlohmann::json& j = e.second;
        if (!j.contains("athena:loader") || !j["athena:loader"].is_string()) continue;
        const std::string loader = j["athena:loader"].get<std::string>();
        if (!j.contains("ctm_textures") || !j["ctm_textures"].is_object()) continue;
        const auto& ct = j["ctm_textures"];
        auto need = [&](const char* k) -> std::string {
            if (!ct.contains(k) || !ct[k].is_string()) return {};
            return ct[k].get<std::string>();
        };

        size_t colon = e.first.find(':');
        if (colon == std::string::npos) continue;
        std::string blockNs = e.first.substr(0, colon);
        std::string blockId = e.first.substr(colon + 1);
        std::vector<std::string> matchBlocks = { blockNs + ":" + blockId };

        bool isCompact = false;
        for (const char* l : kCompactLoaders) if (loader == l) { isCompact = true; break; }
        bool isPillar = false;
        for (const char* l : kPillarLoaders) if (loader == l) { isPillar = true; break; }
        bool isGiant = (loader == "athena:giant");

        if (isCompact) {
            std::string particle = need("particle");
            std::string emptyT = need("empty");
            std::string vertical = need("vertical");
            std::string horizontal = need("horizontal");
            std::string center = need("center");
            if (particle.empty() || emptyT.empty() || vertical.empty() ||
                horizontal.empty() || center.empty()) continue;
            std::string roles[5] = {particle, emptyT, vertical, horizontal, center};
            std::array<std::vector<unsigned char>, 5> pngs;
            bool allOk = true;
            for (int i = 0; i < 5; ++i) {
                std::string tns, tpath;
                if (!ParseTexId(roles[i], tns, tpath) || !LoadTexturePngBytes(tns, tpath, pngs[i])) {
                    allOk = false; break;
                }
            }
            if (!allOk) continue;
            std::string matchNs, matchPath;
            if (!ParseTexId(particle, matchNs, matchPath)) continue;
            std::string baseDir = "modct/athena/" + blockNs + "/" + blockId;
            if (RegisterSyntheticCompactCtmRule(blockNs, baseDir, matchBlocks, matchNs, matchPath, pngs))
                ++registered;
            continue;
        }

        if (isPillar) {
            // Continuity vertical tile order ← Athena textures
            std::string particle = need("particle");
            std::string selfT = need("self");
            std::string topT = need("top");
            std::string bottomT = need("bottom");
            std::string centerT = need("center");
            if (particle.empty() || selfT.empty() || topT.empty() ||
                bottomT.empty() || centerT.empty()) continue;
            // tiles: 0=up-only→bottom, 1=both→center, 2=down-only→top, 3=none→self
            std::string roles[4] = {bottomT, centerT, topT, selfT};
            std::vector<std::vector<unsigned char>> pngs(4);
            bool allOk = true;
            for (int i = 0; i < 4; ++i) {
                std::string tns, tpath;
                if (!ParseTexId(roles[i], tns, tpath) || !LoadTexturePngBytes(tns, tpath, pngs[i])) {
                    allOk = false; break;
                }
            }
            if (!allOk) continue;
            std::string matchNs, matchPath;
            if (!ParseTexId(particle, matchNs, matchPath)) continue;
            std::string baseDir = "modct/athena_pillar/" + blockNs + "/" + blockId;
            if (RegisterSyntheticCtmRule(CtmMethod::Vertical, blockNs, baseDir, matchBlocks,
                                        matchNs, matchPath, pngs))
                ++registered;
            continue;
        }

        if (isGiant) {
            int width = j.value("width", 0);
            int height = j.value("height", 0);
            if (width <= 0 || height <= 0) continue;
            std::string particle = need("particle");
            if (particle.empty()) continue;
            int n = width * height;
            std::vector<std::vector<unsigned char>> pngs(n);
            bool allOk = true;
            for (int i = 0; i < n; ++i) {
                std::string key = std::to_string(i + 1); // Athena keys "1".."N"
                std::string tex = need(key.c_str());
                if (tex.empty()) { allOk = false; break; }
                std::string tns, tpath;
                if (!ParseTexId(tex, tns, tpath) || !LoadTexturePngBytes(tns, tpath, pngs[i])) {
                    allOk = false; break;
                }
            }
            if (!allOk) continue;
            std::string matchNs, matchPath;
            if (!ParseTexId(particle, matchNs, matchPath)) continue;
            std::string baseDir = "modct/athena_giant/" + blockNs + "/" + blockId;
            if (RegisterSyntheticCtmRule(CtmMethod::Repeat, blockNs, baseDir, matchBlocks,
                                        matchNs, matchPath, pngs, width, height))
                ++registered;
            continue;
        }
    }
    return registered;
}

static int RegisterFusionPiecedFromMcmeta() {
    int registered = 0;
    std::vector<std::pair<std::string, nlohmann::json>> metas;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        for (const auto& entry : GlobalCache::mcmetaIndex) {
            auto it = GlobalCache::mcmetaCache.find(entry.second);
            if (it == GlobalCache::mcmetaCache.end() || !it->second.is_object()) continue;
            if (!it->second.contains("fusion") || !it->second["fusion"].is_object()) continue;
            metas.emplace_back(entry.first, it->second);
        }
    }
    for (const auto& m : metas) {
        const auto& fusion = m.second["fusion"];
        std::string type = fusion.value("type", "");
        std::string layout = fusion.value("layout", "");
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        std::transform(layout.begin(), layout.end(), layout.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        if (type != "connecting") continue;

        const std::string prefix = "mcmetas:";
        if (m.first.rfind(prefix, 0) != 0) continue;
        std::string nsPath = m.first.substr(prefix.size());
        size_t colon = nsPath.find(':');
        if (colon == std::string::npos) continue;
        std::string texNs = nsPath.substr(0, colon);
        std::string texPath = nsPath.substr(colon + 1);
        if (texPath.size() > 4 && texPath.rfind(".png") == texPath.size() - 4)
            texPath = texPath.substr(0, texPath.size() - 4);

        std::vector<unsigned char> sheetPng;
        if (!LoadTexturePngBytes(texNs, texPath, sheetPng)) continue;
        int w = 0, h = 0, ch = 0;
        unsigned char* px = stbi_load_from_memory(sheetPng.data(), (int)sheetPng.size(), &w, &h, &ch, 4);
        if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); continue; }

        auto sliceTiles = [&](int cols, int rows, std::vector<std::vector<unsigned char>>& out) -> bool {
            if (cols <= 0 || rows <= 0) return false;
            if (w % cols != 0 || h % rows != 0) return false;
            int tw = w / cols, th = h / rows;
            if (tw <= 0 || th <= 0) return false;
            out.assign((size_t)cols * rows, {});
            for (int row = 0; row < rows; ++row) {
                for (int col = 0; col < cols; ++col) {
                    std::vector<unsigned char> tilePx((size_t)tw * th * 4);
                    for (int y = 0; y < th; ++y) {
                        const unsigned char* src = px + ((size_t)(row * th + y) * w + (size_t)col * tw) * 4;
                        unsigned char* dst = tilePx.data() + (size_t)y * tw * 4;
                        std::memcpy(dst, src, (size_t)tw * 4);
                    }
                    out[(size_t)row * cols + col] = EncodeRgbaPng(tilePx, tw, th);
                    if (out[(size_t)row * cols + col].empty()) return false;
                }
            }
            return true;
        };

        std::vector<std::string> matchBlocks; // tile-only
        std::string baseDir = "modct/fusion/" + layout + "/" + texPath;

        if (layout == "pieced" || layout == "compact") {
            // 5 tiles horizontally → Continuity-compact roles
            int tile = h;
            if (tile <= 0 || w < tile * 5) { stbi_image_free(px); continue; }
            std::array<std::vector<unsigned char>, 5> pngs;
            bool ok = true;
            for (int i = 0; i < 5; ++i) {
                std::vector<unsigned char> tilePx((size_t)tile * tile * 4);
                for (int y = 0; y < tile; ++y) {
                    const unsigned char* src = px + ((size_t)y * w + (size_t)i * tile) * 4;
                    unsigned char* dst = tilePx.data() + (size_t)y * tile * 4;
                    std::memcpy(dst, src, (size_t)tile * 4);
                }
                pngs[i] = EncodeRgbaPng(tilePx, tile, tile);
                if (pngs[i].empty()) { ok = false; break; }
            }
            stbi_image_free(px);
            if (!ok) continue;
            if (RegisterSyntheticCompactCtmRule(texNs, baseDir, matchBlocks, texNs, texPath, pngs))
                ++registered;
            continue;
        }

        if (layout == "horizontal") {
            // Fusion H: 4×1 — Continuity horizontal uses 4 tiles (none,right,left,both) order
            // Continuity SelectHorizontal: L&&R→1? Check CTM.cpp...
            // Fusion: none→0, right→1, left→3, both→2  (getTilePos)
            // Continuity Horizontal typically: none=0, right=1, left=2, both=3 — remap.
            std::vector<std::vector<unsigned char>> grid;
            if (!sliceTiles(4, 1, grid)) { stbi_image_free(px); continue; }
            stbi_image_free(px);
            // Remap Fusion [none,right,both,left] → Continuity order used by SelectHorizontalTile
            // Inspect SelectHorizontalTile below after write.
            // Continuity H: right→0, both→1, left→2, none→3
            // Fusion H sheet: none=0, right=1, both=2, left=3
            std::vector<std::vector<unsigned char>> pngs = {
                grid[1], // Continuity 0 = right
                grid[2], // Continuity 1 = both
                grid[3], // Continuity 2 = left
                grid[0], // Continuity 3 = none
            };
            if (RegisterSyntheticCtmRule(CtmMethod::Horizontal, texNs, baseDir, matchBlocks,
                                        texNs, texPath, pngs))
                ++registered;
            continue;
        }

        if (layout == "vertical") {
            // Fusion V: 1×4 — none→(0,0), bottom→(0,1), both→(0,2), top→(0,3)
            // Continuity Vertical: up&&down→1, up→0, down→2, none→3
            std::vector<std::vector<unsigned char>> grid;
            if (!sliceTiles(1, 4, grid)) { stbi_image_free(px); continue; }
            stbi_image_free(px);
            std::vector<std::vector<unsigned char>> pngs = {
                grid[3], // up only ← Fusion top
                grid[2], // both
                grid[1], // down only ← Fusion bottom
                grid[0], // none
            };
            if (RegisterSyntheticCtmRule(CtmMethod::Vertical, texNs, baseDir, matchBlocks,
                                        texNs, texPath, pngs))
                ++registered;
            continue;
        }

        // full / overlay / simple: no packs in current jars; skip (honest defer)
        stbi_image_free(px);
    }
    return registered;
}


void InitializeModCT() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_blockConnect.clear();
    g_texConnect.clear();
    g_posCamo.clear();
    LoadConnectFromMcmeta();
    LoadConnectFromJson();
    int athena = RegisterAthenaFromBlockstates();
    int fusion = RegisterFusionPiecedFromMcmeta();
    std::cout << "[ModCT] connect textures=" << g_texConnect.size()
              << " blockEntries=" << g_blockConnect.size()
              << " providers=" << g_providers.size()
              << " athenaCT=" << athena
              << " fusionCT=" << fusion
              << " (pillar/giant/H/V enabled)" << std::endl;
}

void RegisterModCTProvider(IModCTProvider* provider) {
    if (!provider) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_providers.push_back(provider);
}

bool TryApplyModCTProviders(ModelData& model, const std::string& ns,
                            const std::string& blockName, int x, int y, int z) {
    std::vector<IModCTProvider*> snap;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        snap = g_providers;
    }
    for (auto* p : snap) {
        if (p && p->AppliesTo(ns, blockName)) {
            if (p->Apply(model, ns, blockName, x, y, z)) return true;
        }
    }
    return false;
}

bool ModCT_BlocksConnect(const std::string& curBaseName,
                         const std::string& neighborBaseName) {
    if (curBaseName == neighborBaseName) return true;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_blockConnect.find(curBaseName);
    if (it == g_blockConnect.end()) return false;
    return it->second.count(neighborBaseName) > 0;
}

bool ModCT_HasConnectGroupForTexture(const std::string& textureNs,
                                     const std::string& texturePath) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_texConnect.count(textureNs + ":" + texturePath) > 0;
}

bool ModCT_TextureConnectsTo(const std::string& textureNs,
                             const std::string& texturePath,
                             const std::string& neighborBaseName) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_texConnect.find(textureNs + ":" + texturePath);
    if (it == g_texConnect.end()) return false;
    return it->second.count(neighborBaseName) > 0;
}

void ModCT_AddConnectGroup(const std::vector<std::string>& blockIds) {
    std::lock_guard<std::mutex> lk(g_mu);
    AddGroup(g_blockConnect, blockIds);
}

void ModCT_SetPositionCamo(int x, int y, int z, const std::string& camoBlockId) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_posCamo[PackPos(x, y, z)] = camoBlockId;
}

std::string ModCT_GetPositionCamo(int x, int y, int z) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_posCamo.find(PackPos(x, y, z));
    return it == g_posCamo.end() ? std::string() : it->second;
}

void ModCT_ClearPositionCamos() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_posCamo.clear();
}

bool ApplyFramedCamoToBlockModel(ModelData& model, const NbtTagPtr& beNbt,
                                 const std::string& blockLocalName,
                                 int x, int y, int z) {
    if (!beNbt || model.materials.empty()) return false;

    auto camoTag = getChildByName(beNbt, "camo");
    CamoAppearance primary = ResolveCamoTag(camoTag);
    auto camoTwoTag = getChildByName(beNbt, "camo_two");
    CamoAppearance secondary = ResolveCamoTag(camoTwoTag);

    if (!primary.ok && !secondary.ok) return false;

    // Register primary camo for CT neighbor joining (Mek connect_to by camo id).
    if (primary.ok && !primary.isFluid) {
        ModCT_SetPositionCamo(x, y, z, primary.fullId);
    } else if (secondary.ok && !secondary.isFluid) {
        ModCT_SetPositionCamo(x, y, z, secondary.fullId);
    }

    // Double camo: rebuild as two half-cuboids when camo_two is present.
    // Framed double blocks ship cube_all placeholders; client splits at runtime.
    if (primary.ok && secondary.ok) {
        int axis = DoubleSplitAxis(blockLocalName);
        ModelData halfA, halfB;
        if (axis == 0) { // X
            halfA = MakeCamoCuboid(0, 0, 0, 0.5f, 1, 1, primary, "@camo~");
            halfB = MakeCamoCuboid(0.5f, 0, 0, 1, 1, 1, secondary, "@camo2~");
        } else if (axis == 2) { // Z
            halfA = MakeCamoCuboid(0, 0, 0, 1, 1, 0.5f, primary, "@camo~");
            halfB = MakeCamoCuboid(0, 0, 0.5f, 1, 1, 1, secondary, "@camo2~");
        } else { // Y
            halfA = MakeCamoCuboid(0, 0, 0, 1, 0.5f, 1, primary, "@camo~");
            halfB = MakeCamoCuboid(0, 0.5f, 0, 1, 1, 1, secondary, "@camo2~");
        }
        model = halfA;
        MergeModelsDirectly(model, halfB);
        return true;
    }

    if (primary.ok) {
        ApplyAppearanceToFramedMats(model, primary, "@camo~");
        return true;
    }
    // Only camo_two (unusual) — still apply.
    ApplyAppearanceToFramedMats(model, secondary, "@camo2~");
    return true;
}


static bool IsCopycatBaseTex(const std::string& tp) {
    return tp.find("copycat_base") != std::string::npos ||
           tp.find("/copycat/") != std::string::npos ||
           tp.find("copycats:") != std::string::npos;
}

static CamoAppearance ResolveCreateMaterialTag(const NbtTagPtr& beNbt) {
    CamoAppearance out;
    if (!beNbt) return out;
    auto matTag = getChildByName(beNbt, "Material");
    if (!matTag || matTag->type != TagType::COMPOUND) return out;
    auto nameTag = getChildByName(matTag, "Name");
    if (!nameTag || nameTag->type != TagType::STRING) return out;
    std::string full(nameTag->payload.begin(), nameTag->payload.end());
    if (full.empty()) return out;

    auto propsTag = getChildByName(matTag, "Properties");
    if (propsTag && propsTag->type == TagType::COMPOUND && !propsTag->children.empty()) {
        std::string props;
        for (const auto& p : propsTag->children) {
            if (!props.empty()) props += ",";
            props += p->name + "=" + std::string(p->payload.begin(), p->payload.end());
        }
        if (!props.empty()) full += "[" + props + "]";
    }

    out.ns = "minecraft";
    std::string local = full;
    size_t colon = full.find(':');
    if (colon != std::string::npos) {
        out.ns = full.substr(0, colon);
        local = full.substr(colon + 1);
    }
    out.local = local;
    size_t br0 = out.local.find('[');
    if (br0 != std::string::npos) out.local = out.local.substr(0, br0);
    out.fullId = out.ns + ":" + out.local;

    // Skip unresolved copycat_base placeholder
    if (out.fullId == "create:copycat_base" || out.fullId == "copycats:copycat_base")
        return out;

    ModelData camoModel = GetRandomModelFromCache(out.ns, out.local);
    if (camoModel.materials.empty() && local != out.local)
        camoModel = GetRandomModelFromCache(out.ns, local);
    // Do not ProcessBlockstate here — chunk load / export may be concurrent or frozen.
    const Material* src = nullptr;
    for (const auto& m : camoModel.materials) {
        if (m.texturePath.empty() || m.texturePath == "None") continue;
        src = &m;
        if (m.texturePath.find("/block/") != std::string::npos) break;
    }
    if (src) {
        out.mat = *src;
    } else {
        out.mat.name = out.fullId;
        out.mat.texturePath = "textures/" + out.ns + "/block/" + out.local + ".png";
        out.mat.textureKey = "all";
        if (!TextureExists(out.ns, "block/" + out.local)) return out;
    }
    out.ok = true;
    return out;
}

// ---- Create copycat_step / copycat_panel: CopycatStepModel / CopycatPanelModel port ----
// The material's block model is cropped to boxes and moved (BakedModelHelper.cropAndMove).
namespace {
struct CopyVert { float p[3]; float uv[2]; };
struct CopyPiece { float mn[3], mx[3], off[3]; int skip[2]; }; // skip: light dirs (0..5) or -1

// light dirs: 0=down 1=up 2=north 3=south 4=west 5=east
const int kDirVec[6][3] = {{0,-1,0},{0,1,0},{0,0,-1},{0,0,1},{-1,0,0},{1,0,0}};
const FaceType kDirFace[6] = {FaceType::DOWN, FaceType::UP, FaceType::NORTH,
                              FaceType::SOUTH, FaceType::WEST, FaceType::EAST};

int DirIndex(const std::string& d) {
    static const char* names[6] = {"down","up","north","south","west","east"};
    for (int i = 0; i < 6; ++i) if (d == names[i]) return i;
    return -1;
}

std::vector<CopyVert> ClipAxis(const std::vector<CopyVert>& in, int axis, float bound, bool keepAbove) {
    std::vector<CopyVert> out;
    const float eps = 1e-5f;
    auto inside = [&](const CopyVert& v) {
        return keepAbove ? v.p[axis] >= bound - eps : v.p[axis] <= bound + eps;
    };
    for (size_t i = 0; i < in.size(); ++i) {
        const CopyVert& a = in[i];
        const CopyVert& b = in[(i + 1) % in.size()];
        bool ia = inside(a), ib = inside(b);
        if (ia) out.push_back(a);
        if (ia != ib) {
            float t = (bound - a.p[axis]) / (b.p[axis] - a.p[axis]);
            CopyVert c;
            for (int k = 0; k < 3; ++k) c.p[k] = a.p[k] + (b.p[k] - a.p[k]) * t;
            for (int k = 0; k < 2; ++k) c.uv[k] = a.uv[k] + (b.uv[k] - a.uv[k]) * t;
            c.p[axis] = bound;
            out.push_back(c);
        }
    }
    return out;
}

int LightDir(const std::vector<CopyVert>& poly) {
    float n[3] = {0, 0, 0};
    for (size_t i = 0; i < poly.size(); ++i) {
        const float* a = poly[i].p;
        const float* b = poly[(i + 1) % poly.size()].p;
        n[0] += (a[1] - b[1]) * (a[2] + b[2]);
        n[1] += (a[2] - b[2]) * (a[0] + b[0]);
        n[2] += (a[0] - b[0]) * (a[1] + b[1]);
    }
    int axis = 0;
    for (int k = 1; k < 3; ++k) if (std::fabs(n[k]) > std::fabs(n[axis])) axis = k;
    if (std::fabs(n[axis]) < 1e-9f) return -1;
    if (axis == 1) return n[1] > 0 ? 1 : 0;
    if (axis == 2) return n[2] > 0 ? 3 : 2;
    return n[0] > 0 ? 5 : 4;
}

ModelData CropAndMovePieces(const ModelData& src, const std::vector<CopyPiece>& pieces) {
    ModelData out;
    out.materials = src.materials;
    for (const auto& face : src.faces) {
        std::vector<CopyVert> poly;
        for (int i = 0; i < 4; ++i) {
            int vi = face.vertexIndices[i], ti = face.uvIndices[i];
            if (vi < 0 || (size_t)(vi * 3 + 2) >= src.vertices.size()) continue;
            CopyVert v{};
            for (int k = 0; k < 3; ++k) v.p[k] = src.vertices[vi * 3 + k];
            if (ti >= 0 && (size_t)(ti * 2 + 1) < src.uvCoordinates.size()) {
                v.uv[0] = src.uvCoordinates[ti * 2];
                v.uv[1] = src.uvCoordinates[ti * 2 + 1];
            }
            if (!poly.empty() && std::memcmp(poly.back().p, v.p, sizeof(v.p)) == 0) continue;
            poly.push_back(v);
        }
        if (poly.size() > 1 && std::memcmp(poly.front().p, poly.back().p, sizeof(float) * 3) == 0) poly.pop_back();
        if (poly.size() < 3) continue;
        int dir = LightDir(poly);
        for (const auto& piece : pieces) {
            if (dir >= 0 && (dir == piece.skip[0] || dir == piece.skip[1])) continue;
            std::vector<CopyVert> c = poly;
            for (int axis = 0; axis < 3 && c.size() >= 3; ++axis) {
                c = ClipAxis(c, axis, piece.mn[axis], true);
                if (c.size() >= 3) c = ClipAxis(c, axis, piece.mx[axis], false);
            }
            if (c.size() < 3) continue;
            // Drop zero-area slivers (face lying on a clip plane edge-on).
            float area = 0;
            for (size_t i = 1; i + 1 < c.size(); ++i) {
                float u[3], w[3], x[3];
                for (int k = 0; k < 3; ++k) { u[k] = c[i].p[k] - c[0].p[k]; w[k] = c[i + 1].p[k] - c[0].p[k]; }
                x[0] = u[1] * w[2] - u[2] * w[1]; x[1] = u[2] * w[0] - u[0] * w[2]; x[2] = u[0] * w[1] - u[1] * w[0];
                area += std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
            }
            if (area < 1e-7f) continue;
            for (auto& v : c) for (int k = 0; k < 3; ++k) v.p[k] += piece.off[k];
            // Cull only faces that still sit on the block boundary they face.
            FaceType cull = FaceType::DO_NOT_CULL;
            if (dir >= 0) {
                int axis = kDirVec[dir][0] ? 0 : kDirVec[dir][1] ? 1 : 2;
                float edge = (kDirVec[dir][axis] > 0) ? 1.0f : 0.0f;
                bool onEdge = true;
                for (const auto& v : c) if (std::fabs(v.p[axis] - edge) > 1e-4f) { onEdge = false; break; }
                if (onEdge) cull = kDirFace[dir];
            }
            int base = (int)(out.vertices.size() / 3);
            int baseUv = (int)(out.uvCoordinates.size() / 2);
            for (const auto& v : c) {
                out.vertices.insert(out.vertices.end(), v.p, v.p + 3);
                out.uvCoordinates.insert(out.uvCoordinates.end(), v.uv, v.uv + 2);
            }
            for (size_t i = 1; i + 1 < c.size(); i += 2) {
                Face f = face;
                f.faceDirection = cull;
                size_t i2 = std::min(i + 2, c.size() - 1);
                int idx[4] = {0, (int)i, (int)i + 1, (int)i2};
                for (int k = 0; k < 4; ++k) {
                    f.vertexIndices[k] = base + idx[k];
                    f.uvIndices[k] = baseUv + idx[k];
                }
                out.faces.push_back(f);
            }
        }
    }
    return out;
}

// AABB.contract semantics: positive shrinks max, negative raises min.
void Contract(CopyPiece& p, const float v[3]) {
    for (int k = 0; k < 3; ++k) {
        if (v[k] > 0) p.mx[k] -= v[k];
        else if (v[k] < 0) p.mn[k] -= v[k];
    }
}

std::vector<CopyPiece> CopycatPanelPieces(int facing) {
    std::vector<CopyPiece> pieces;
    const int* n = kDirVec[facing];
    int opposite = facing ^ 1;
    for (bool front : {true, false}) {
        CopyPiece p{{0,0,0},{1,1,1},{0,0,0},{-1,-1}};
        float contract = (16.0f - (front ? 1.0f : 2.0f)) / 16.0f;
        float c[3] = {n[0] * contract, n[1] * contract, n[2] * contract};
        Contract(p, c);
        if (!front) for (int k = 0; k < 3; ++k) { p.mn[k] += n[k] * 14.0f / 16.0f; p.mx[k] += n[k] * 14.0f / 16.0f; }
        for (int k = 0; k < 3; ++k) p.off[k] = front ? 0.0f : n[k] * -13.0f / 16.0f;
        p.skip[0] = front ? facing : opposite;
        pieces.push_back(p);
    }
    return pieces;
}

std::vector<CopyPiece> CopycatStepPieces(int facing, bool upperHalf) {
    std::vector<CopyPiece> pieces;
    const int* n = kDirVec[facing];
    int opposite = facing ^ 1;
    for (bool top : {true, false}) {
        for (bool front : {true, false}) {
            CopyPiece p{{0,0,0},{1,1,1},{0,0,0},{-1,-1}};
            float c[3] = {-n[0] * 0.75f, 0.75f, -n[2] * 0.75f};
            Contract(p, c);
            float mv[3] = {0, 0, 0};
            if (front) for (int k = 0; k < 3; ++k) mv[k] += n[k] * -0.75f;
            if (top) mv[1] += 0.75f;
            for (int k = 0; k < 3; ++k) { p.mn[k] += mv[k]; p.mx[k] += mv[k]; }
            if (front) for (int k = 0; k < 3; ++k) p.off[k] += n[k] * 0.5f;
            if (top != upperHalf) p.off[1] += upperHalf ? 0.5f : -0.5f;
            p.skip[0] = front ? facing : opposite;
            p.skip[1] = top ? 0 : 1; // top piece drops DOWN quads, bottom drops UP
            pieces.push_back(p);
        }
    }
    return pieces;
}

std::string StateValue(const std::string& blockName, const std::string& key) {
    size_t b = blockName.find('[');
    if (b == std::string::npos) return "";
    std::string s = blockName.substr(b + 1);
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find_first_of(",]", pos);
        if (end == std::string::npos) end = s.size();
        std::string kv = s.substr(pos, end - pos);
        size_t eq = kv.find_first_of("=:");
        if (eq != std::string::npos && kv.substr(0, eq) == key) return kv.substr(eq + 1);
        pos = end + 1;
    }
    return "";
}

// Material block id (with state) from BE "Material"; empty copycats use copycat_base.
void CopycatMaterialId(const NbtTagPtr& beNbt, std::string& ns, std::string& localFull, std::string& local) {
    ns = "create"; local = localFull = "copycat_base";
    auto matTag = beNbt ? getChildByName(beNbt, "Material") : nullptr;
    auto nameTag = (matTag && matTag->type == TagType::COMPOUND) ? getChildByName(matTag, "Name") : nullptr;
    if (!nameTag || nameTag->type != TagType::STRING) return;
    std::string full(nameTag->payload.begin(), nameTag->payload.end());
    if (full.empty() || full == "minecraft:air") return;
    size_t colon = full.find(':');
    ns = colon == std::string::npos ? "minecraft" : full.substr(0, colon);
    local = colon == std::string::npos ? full : full.substr(colon + 1);
    localFull = local;
    auto propsTag = getChildByName(matTag, "Properties");
    if (propsTag && propsTag->type == TagType::COMPOUND && !propsTag->children.empty()) {
        std::string props;
        for (const auto& p : propsTag->children) {
            if (!props.empty()) props += ",";
            props += p->name + "=" + std::string(p->payload.begin(), p->payload.end());
        }
        localFull += "[" + props + "]";
    }
}
} // namespace

static std::mutex g_copycatWarmMu;
static std::unordered_set<std::string> g_copycatWarm; // "ns:local"

void ModCT_QueueCopycatWarm(const NbtTagPtr& beNbt) {
    std::string ns, localFull, local;
    CopycatMaterialId(beNbt, ns, localFull, local);
    std::lock_guard<std::mutex> lk(g_copycatWarmMu);
    g_copycatWarm.insert(ns + ":" + local);
    g_copycatWarm.insert("create:copycat_base");
}

void ModCT_FlushCopycatWarm() {
    std::unordered_set<std::string> pending;
    {
        std::lock_guard<std::mutex> lk(g_copycatWarmMu);
        pending.swap(g_copycatWarm);
    }
    for (const auto& id : pending) {
        size_t colon = id.find(':');
        ProcessBlockstate(id.substr(0, colon), { id.substr(colon + 1) });
    }
}

static bool ApplyCreateCopycatShape(ModelData& model, const NbtTagPtr& beNbt,
                                    const std::string& blockName, int x, int y, int z) {
    std::string base = blockName.substr(0, blockName.find('['));
    bool isStep = base == "copycat_step", isPanel = base == "copycat_panel";
    if (!isStep && !isPanel) return false;

    std::string ns, localFull, local;
    CopycatMaterialId(beNbt, ns, localFull, local);
    ModelData material = GetRandomModelFromCache(ns, localFull);
    if (material.faces.empty()) material = GetRandomModelFromCache(ns, local);
    if (material.faces.empty()) {
        CamoAppearance app = ResolveCreateMaterialTag(beNbt);
        if (!app.ok) return false;
        material = MakeCamoCuboid(0, 0, 0, 1, 1, 1, app, "@copycat~");
    }
    if (ns + ":" + local != "create:copycat_base") ModCT_SetPositionCamo(x, y, z, ns + ":" + local);

    std::vector<CopyPiece> pieces;
    if (isPanel) {
        int facing = DirIndex(StateValue(blockName, "facing"));
        pieces = CopycatPanelPieces(facing < 0 ? 1 : facing);
    } else {
        int facing = DirIndex(StateValue(blockName, "facing"));
        if (facing < 2) facing = 3;
        pieces = CopycatStepPieces(facing, StateValue(blockName, "half") == "top");
    }
    ModelData shaped = CropAndMovePieces(material, pieces);
    if (shaped.faces.empty()) return false;
    model = std::move(shaped);
    return true;
}

bool ApplyCopycatCamoToBlockModel(ModelData& model, const NbtTagPtr& beNbt,
                                  const std::string& ns,
                                  const std::string& blockLocalName,
                                  int x, int y, int z) {
    if (ns == "create" && ApplyCreateCopycatShape(model, beNbt, blockLocalName, x, y, z))
        return true;
    CamoAppearance app = ResolveCreateMaterialTag(beNbt);
    if (!app.ok) return false;

    ModCT_SetPositionCamo(x, y, z, app.fullId);

    // Empty / air placeholder models → full cube of material
    bool emptyGeom = model.vertices.empty() || model.faces.empty();
    if (!emptyGeom) {
        // Some copycats ship air model that still has zero faces after parse
        bool onlyMissing = true;
        for (const auto& mat : model.materials) {
            if (mat.texturePath.empty() || mat.texturePath == "None") continue;
            if (mat.texturePath.find("missing") != std::string::npos) continue;
            onlyMissing = false;
            break;
        }
        if (onlyMissing && model.faces.size() <= 6) emptyGeom = true;
    }

    if (emptyGeom) {
        {
        model = MakeCamoCuboid(0, 0, 0, 1, 1, 1, app, "@copycat~");
        if (!model.materials.empty())
            model.materials[0].name = std::string("copycat_camo@copycat~") + app.fullId;
    }
        return true;
    }

    // Retexture copycat_base / copycats frame materials in place
    bool any = false;
    for (auto& mat : model.materials) {
        const std::string& tp = mat.texturePath;
        if (!IsCopycatBaseTex(tp) && tp.find("create:") == std::string::npos) {
            // Also rewrite when material name looks like copycat
            if (mat.name.find("copycat") == std::string::npos) continue;
        }
        mat.texturePath = app.mat.texturePath;
        mat.type = app.mat.type;
        mat.aspectRatio = app.mat.aspectRatio;
        if (mat.name.find("@copycat~") == std::string::npos)
            mat.name += std::string("@copycat~") + app.fullId;
        any = true;
    }
    // If nothing matched but we have faces, force-apply first material (shape-only models)
    if (!any && !model.materials.empty()) {
        for (auto& mat : model.materials) {
            mat.texturePath = app.mat.texturePath;
            mat.type = app.mat.type;
            mat.name += std::string("@copycat~") + app.fullId;
        }
        any = true;
    }
    (void)blockLocalName;
    (void)ns;
    return any;
}

