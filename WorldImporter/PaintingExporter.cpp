#include "PaintingExporter.h"

#include "config.h"
#include "decompressor.h"
#include "GlobalCache.h"
#include "nbtutils.h"
#include "texture.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

// Defined in objExporter.cpp. Same directory region_models.obj is written to.
std::string getExecutableDir();

namespace {

constexpr double kHalfPixel = 1.0 / 32.0; // front/back sit 1/32 off Pos
constexpr float kStrip = 1.0f / 16.0f;    // edge quads sample a 1px strip of back.png

struct Basis {
    double ux, uz; // horizontal axis of increasing texture u
    double nx, nz; // facing (front) normal
};

// Rotate the south frame (u = +X, normal = +Z) around Y.
// 0 south, 1 west, 2 north, 3 east.
const Basis kBasis[4] = {
    { 1,  0,  0,  1},
    { 0,  1, -1,  0},
    {-1,  0,  0, -1},
    { 0, -1,  1,  0},
};

struct Vec3 {
    float x, y, z;
};

Vec3 At(double cx, double cy, double cz, const Basis& b, double alongU, double alongY, double depth) {
    return Vec3{
        static_cast<float>(cx + b.ux * alongU + b.nx * depth),
        static_cast<float>(cy + alongY),
        static_cast<float>(cz + b.uz * alongU + b.nz * depth)
    };
}

PaintingExporter::PaintingQuad MakeQuad(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
    float u0, float v0, float u1, float v1, float u2, float v2, float u3, float v3, bool front) {
    PaintingExporter::PaintingQuad q;
    q.x[0] = a.x; q.y[0] = a.y; q.z[0] = a.z; q.u[0] = u0; q.v[0] = v0;
    q.x[1] = b.x; q.y[1] = b.y; q.z[1] = b.z; q.u[1] = u1; q.v[1] = v1;
    q.x[2] = c.x; q.y[2] = c.y; q.z[2] = c.z; q.u[2] = u2; q.v[2] = v2;
    q.x[3] = d.x; q.y[3] = d.y; q.z[3] = d.z; q.u[3] = u3; q.v[3] = v3;
    q.front = front;
    return q;
}

const std::unordered_map<std::string, std::pair<int, int>>& BuiltinTable() {
    // width x height, in blocks. Names that only exist as data-driven variants
    // (1.21+) are intentionally absent and come from the jar JSON.
    static const std::unordered_map<std::string, std::pair<int, int>> table = {
        {"kebab", {1, 1}}, {"aztec", {1, 1}}, {"alban", {1, 1}}, {"aztec2", {1, 1}},
        {"bomb", {1, 1}}, {"plant", {1, 1}}, {"wasteland", {1, 1}},
        {"pool", {2, 1}}, {"courbet", {2, 1}}, {"sea", {2, 1}}, {"sunset", {2, 1}}, {"creebet", {2, 1}},
        {"wanderer", {1, 2}}, {"graham", {1, 2}},
        {"match", {2, 2}}, {"bust", {2, 2}}, {"stage", {2, 2}}, {"void", {2, 2}},
        {"skull_and_roses", {2, 2}}, {"wither", {2, 2}},
        {"earth", {2, 2}}, {"wind", {2, 2}}, {"water", {2, 2}}, {"fire", {2, 2}},
        {"fighters", {4, 2}},
        {"pointer", {4, 4}}, {"pigscene", {4, 4}}, {"burning_skull", {4, 4}},
        {"skeleton", {4, 3}}, {"donkey_kong", {4, 3}},
    };
    return table;
}

void SplitId(const std::string& id, std::string& ns, std::string& path) {
    const size_t colon = id.find(':');
    if (colon == std::string::npos) {
        ns = "minecraft";
        path = id;
    } else {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
}

std::string FitMaterialName(std::string name) {
    if (name.size() <= 63) return name;
    return name.substr(name.size() - 63);
}

std::string PaintingTextureId(const std::string& assetPath) {
    if (assetPath.rfind("painting/", 0) == 0) return assetPath;
    return "painting/" + assetPath;
}

uint32_t ReadBE32(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

int FloorDiv(int a, int b) {
    int q = a / b;
    const int r = a % b;
    if (r != 0 && ((a < 0) != (b < 0))) --q;
    return q;
}

bool GetNumber(const NbtTagPtr& tag, double& value) {
    if (!tag) return false;
    switch (tag->type) {
    case TagType::DOUBLE: value = bytesToDouble(tag->payload); return true;
    case TagType::FLOAT: value = bytesToFloat(tag->payload); return true;
    case TagType::INT: value = bytesToInt(tag->payload); return true;
    case TagType::SHORT: value = bytesToShort(tag->payload); return true;
    case TagType::BYTE: value = bytesToByte(tag->payload); return true;
    default: return false;
    }
}

bool ReadChunkNbt(const std::vector<char>& region, size_t slot, NbtTagPtr& root) {
    const size_t hp = slot * 4;
    if (hp + 4 > region.size()) return false;
    const uint32_t loc = ReadBE32(region.data() + hp);
    if (!loc) return false;
    const size_t offset = static_cast<size_t>(loc >> 8) * 4096;
    if (offset + 5 > region.size()) return false;
    const uint32_t length = ReadBE32(region.data() + offset);
    if (length < 2 || offset + 4 + length > region.size()) return false;
    const unsigned char compression = static_cast<unsigned char>(region[offset + 4]);
    std::vector<char> packed(region.begin() + offset + 5, region.begin() + offset + 4 + length);
    std::vector<char> raw;
    if (compression == 2) {
        if (!DecompressData(packed, raw)) return false;
    } else if (compression == 3) {
        raw = std::move(packed);
    } else if (compression == 1) {
        if (!DecompressGzip(packed, raw)) return false;
    } else {
        return false;
    }
    try {
        size_t index = 0;
        root = readTag(raw, index);
        return static_cast<bool>(root);
    } catch (...) {
        return false;
    }
}

fs::path EntitiesDirectory() {
    const fs::path world(config.worldPath);
    const std::string& dim = config.selectedDimension;
    auto isDir = [](const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p, ec);
    };
    auto dimensions = [&](const std::string& ns, const std::string& name) {
        return world / "dimensions" / ns / name / "entities";
    };
    if (dim == "minecraft:the_nether" || dim == "the_nether" || dim == "DIM-1") {
        const fs::path classic = world / "DIM-1" / "entities";
        if (isDir(classic)) return classic;
        return dimensions("minecraft", "the_nether");
    }
    if (dim == "minecraft:the_end" || dim == "the_end" || dim == "DIM1") {
        const fs::path classic = world / "DIM1" / "entities";
        if (isDir(classic)) return classic;
        return dimensions("minecraft", "the_end");
    }
    if (dim.empty() || dim == "minecraft:overworld" || dim == "overworld") {
        const fs::path classic = world / "entities";
        if (isDir(classic)) return classic;
        return dimensions("minecraft", "overworld");
    }
    std::string ns = "minecraft";
    std::string path = dim;
    const size_t colon = dim.find(':');
    if (colon != std::string::npos) {
        ns = dim.substr(0, colon);
        path = dim.substr(colon + 1);
    }
    fs::path result = world / "dimensions" / ns;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty()) result /= part;
    }
    return result / "entities";
}

bool PosInExportBox(double x, double y, double z) {
    // Block box is inclusive on the integer min/max. Pos is a double at the
    // painting centre, which sits a fraction past the wall-side block coordinate,
    // so the far edge is max+1 (same test EntityExporter uses).
    return x >= config.minX && x <= static_cast<double>(config.maxX) + 1.0 &&
           y >= config.minY && y <= static_cast<double>(config.maxY) + 1.0 &&
           z >= config.minZ && z <= static_cast<double>(config.maxZ) + 1.0;
}

bool IsPaintingId(const std::string& id) {
    return id == "minecraft:painting" || id == "painting" ||
           (id.size() >= 9 && id.compare(id.size() - 9, 9, ":painting") == 0);
}

bool LookupVariantJson(const std::string& ns, const std::string& name, nlohmann::json& out) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    const std::string indexKey = "painting_variants:" + ns + ":" + name;
    auto it = GlobalCache::paintingVariantIndex.find(indexKey);
    if (it != GlobalCache::paintingVariantIndex.end()) {
        auto found = GlobalCache::paintingVariants.find(it->second);
        if (found != GlobalCache::paintingVariants.end()) {
            out = found->second;
            return true;
        }
    }
    for (const auto& modId : GlobalCache::jarOrder) {
        auto found = GlobalCache::paintingVariants.find(modId + ":" + ns + ":" + name);
        if (found != GlobalCache::paintingVariants.end()) {
            out = found->second;
            return true;
        }
    }
    return false;
}

struct ResolvedVariant {
    std::string materialName;
    std::string textureNs;
    std::string texturePath; // "painting/<asset>"
    int width = 0;
    int height = 0;
    bool ok = false;
};

int JsonInt(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_number()) return 0;
    return j[key].get<int>();
}

ResolvedVariant ResolveVariantId(const std::string& variantId) {
    ResolvedVariant out;
    std::string ns, path;
    SplitId(variantId, ns, path);
    if (path.empty()) return out;

    int width = 0, height = 0;
    std::string assetId = variantId;
    nlohmann::json def;
    if (LookupVariantJson(ns, path, def)) {
        width = JsonInt(def, "width");
        height = JsonInt(def, "height");
        if (def.contains("asset_id") && def["asset_id"].is_string()) {
            const std::string asset = def["asset_id"].get<std::string>();
            if (!asset.empty()) assetId = asset;
        }
    }
    if (width <= 0 || height <= 0) {
        const PaintingExporter::PaintingSize builtin = PaintingExporter::BuiltinPaintingSize(path);
        if (builtin.known) {
            width = builtin.width;
            height = builtin.height;
        }
    }
    if (width <= 0 || height <= 0) return out;

    std::string assetNs, assetPath;
    SplitId(assetId, assetNs, assetPath);
    if (assetPath.empty()) {
        assetNs = ns;
        assetPath = path;
    }
    out.materialName = FitMaterialName(ns + ":painting/" + path);
    out.textureNs = assetNs;
    out.texturePath = PaintingTextureId(assetPath);
    out.width = width;
    out.height = height;
    out.ok = true;
    return out;
}

ResolvedVariant ResolveCompoundVariant(const NbtTagPtr& variant) {
    ResolvedVariant out;
    auto assetTag = getChildByName(variant, "asset_id");
    if (!assetTag || assetTag->type != TagType::STRING) return out;
    const std::string assetId = getStringTag(assetTag);
    double dw = 0, dh = 0;
    if (!GetNumber(getChildByName(variant, "width"), dw) ||
        !GetNumber(getChildByName(variant, "height"), dh)) return out;
    const int width = static_cast<int>(std::lround(dw));
    const int height = static_cast<int>(std::lround(dh));
    if (assetId.empty() || width <= 0 || height <= 0) return out;
    std::string ns, path;
    SplitId(assetId, ns, path);
    if (path.empty()) return out;
    out.materialName = FitMaterialName(ns + ":painting/" + path);
    out.textureNs = ns;
    out.texturePath = PaintingTextureId(path);
    out.width = width;
    out.height = height;
    out.ok = true;
    return out;
}

struct Collected {
    double x, y, z;
    int facing;
    ResolvedVariant variant;
};

void CollectEntity(const NbtTagPtr& entity, std::vector<Collected>& out, std::unordered_set<std::string>& unknown) {
    if (!entity || entity->type != TagType::COMPOUND) return;
    auto idTag = getChildByName(entity, "id");
    if (idTag && idTag->type == TagType::STRING && IsPaintingId(getStringTag(idTag))) {
        auto posTag = getChildByName(entity, "Pos");
        if (posTag && posTag->type == TagType::LIST && posTag->children.size() >= 3) {
            double x = 0, y = 0, z = 0;
            if (GetNumber(posTag->children[0], x) && GetNumber(posTag->children[1], y) &&
                GetNumber(posTag->children[2], z) &&
                std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
                PosInExportBox(x, y, z)) {
                auto facingTag = getChildByName(entity, "facing");
                if (!facingTag) facingTag = getChildByName(entity, "Facing");
                double facingValue = -1;
                if (GetNumber(facingTag, facingValue)) {
                    const int facing = static_cast<int>(std::lround(facingValue));
                    if (facing >= 0 && facing <= 3) {
                        ResolvedVariant variant;
                        auto variantTag = getChildByName(entity, "variant");
                        if (!variantTag) variantTag = getChildByName(entity, "Motive");
                        if (variantTag && variantTag->type == TagType::COMPOUND) {
                            variant = ResolveCompoundVariant(variantTag);
                        } else if (variantTag && variantTag->type == TagType::STRING) {
                            variant = ResolveVariantId(getStringTag(variantTag));
                        }
                        if (variant.ok) {
                            out.push_back(Collected{x, y, z, facing, std::move(variant)});
                        } else if (variantTag && variantTag->type == TagType::STRING) {
                            unknown.insert(getStringTag(variantTag));
                        } else {
                            unknown.insert("<compound>");
                        }
                    }
                }
            }
        }
    }
    auto passengers = getChildByName(entity, "Passengers");
    if (!passengers) passengers = getChildByName(entity, "passengers");
    if (passengers) {
        for (const auto& child : passengers->children) CollectEntity(child, out, unknown);
    }
}

std::string EnsureTexture(const std::string& ns, const std::string& path,
    std::unordered_map<std::string, std::string>& saved) {
    const std::string key = ns + ":" + path;
    auto it = saved.find(key);
    if (it != saved.end()) return it->second;
    std::string saveDir = "textures";
    std::string rel;
    if (SaveTextureToFile(ns, path, saveDir)) {
        rel = "textures/" + ns + "/" + path + ".png";
    } else {
        std::cerr << "[Paintings] Texture not found: " << ns << ":" << path << std::endl;
    }
    saved.emplace(key, rel);
    return rel;
}

void RemoveStale(const std::string& objPath, const std::string& mtlPath) {
    std::error_code ec;
    fs::remove(objPath, ec);
    fs::remove(mtlPath, ec);
}

void WriteMtl(const std::string& path, const std::vector<std::pair<std::string, std::string>>& materials) {
    std::ofstream mtl(path);
    if (!mtl) {
        std::cerr << "[Paintings] Failed to write " << path << std::endl;
        return;
    }
    mtl.imbue(std::locale::classic());
    mtl << std::fixed << std::setprecision(6);
    for (const auto& entry : materials) {
        mtl << "newmtl " << entry.first << "\n";
        mtl << "Ns 90.000000\n";
        mtl << "Kd 1.000000 1.000000 1.000000\n";
        mtl << "Ks 0.000000 0.000000 0.000000\n";
        mtl << "Ke 0.000000 0.000000 0.000000\n";
        mtl << "Ni 1.500000\n";
        mtl << "illum 1\n";
        if (!entry.second.empty()) mtl << "map_Kd " << entry.second << "\n";
        mtl << "\n";
    }
}

struct OutFace {
    int v[4];
    int vt[4];
    int material;
};

void WriteObj(const std::string& path, const std::string& mtlName,
    const std::vector<float>& vertices, const std::vector<float>& uvs,
    const std::vector<OutFace>& faces, const std::vector<std::string>& materialNames) {
    std::ofstream obj(path);
    if (!obj) {
        std::cerr << "[Paintings] Failed to write " << path << std::endl;
        return;
    }
    obj.imbue(std::locale::classic());
    obj << "mtllib " << mtlName << "\n";
    obj << "o paintings\n\n";
    obj << "# Vertices (" << vertices.size() / 3 << ")\n";
    obj << std::fixed << std::setprecision(6);
    for (size_t i = 0; i < vertices.size(); i += 3) {
        obj << "v " << vertices[i] << " " << vertices[i + 1] << " " << vertices[i + 2] << "\n";
    }
    obj << "\n# UVs (" << uvs.size() / 2 << ")\n";
    for (size_t i = 0; i < uvs.size(); i += 2) {
        obj << "vt " << uvs[i] << " " << uvs[i + 1] << "\n";
    }
    obj << "\n# Faces (" << faces.size() << ")\n";
    std::vector<std::vector<size_t>> groups(materialNames.size());
    for (size_t i = 0; i < faces.size(); ++i) {
        if (faces[i].material >= 0 && static_cast<size_t>(faces[i].material) < groups.size()) {
            groups[faces[i].material].push_back(i);
        }
    }
    for (size_t m = 0; m < groups.size(); ++m) {
        if (groups[m].empty()) continue;
        obj << "usemtl " << materialNames[m] << "\n";
        for (size_t faceIndex : groups[m]) {
            const OutFace& face = faces[faceIndex];
            obj << "f";
            for (int k = 0; k < 4; ++k) obj << " " << face.v[k] << "/" << face.vt[k];
            obj << "\n";
        }
    }
}

} // namespace

namespace PaintingExporter {

PaintingSize BuiltinPaintingSize(const std::string& variantId) {
    std::string name = variantId;
    const size_t colon = name.find(':');
    if (colon != std::string::npos) name = name.substr(colon + 1);
    const std::string prefix = "painting/";
    if (name.rfind(prefix, 0) == 0) name = name.substr(prefix.size());
    auto it = BuiltinTable().find(name);
    if (it == BuiltinTable().end()) return {};
    return PaintingSize{it->second.first, it->second.second, true};
}

std::vector<PaintingQuad> BuildPaintingMesh(double posX, double posY, double posZ,
    int facing, int width, int height) {
    std::vector<PaintingQuad> quads;
    if (facing < 0 || facing > 3 || width <= 0 || height <= 0) return quads;
    const Basis& b = kBasis[facing];
    const double halfW = width * 0.5;
    const double halfH = height * 0.5;
    const double frontD = kHalfPixel;
    const double backD = -kHalfPixel;

    // One front quad. u=0 is the viewer's left; obj v=1 is the texture top.
    // Order is CCW when seen along the facing normal (same outward winding as block faces).
    {
        const Vec3 p0 = At(posX, posY, posZ, b, -halfW, -halfH, frontD);
        const Vec3 p1 = At(posX, posY, posZ, b,  halfW, -halfH, frontD);
        const Vec3 p2 = At(posX, posY, posZ, b,  halfW,  halfH, frontD);
        const Vec3 p3 = At(posX, posY, posZ, b, -halfW,  halfH, frontD);
        quads.push_back(MakeQuad(p0, p1, p2, p3, 0, 0, 1, 0, 1, 1, 0, 1, true));
    }

    for (int i = 0; i < width; ++i) {
        for (int j = 0; j < height; ++j) {
            const double u0 = -halfW + i;
            const double u1 = u0 + 1.0;
            const double y0 = -halfH + j;
            const double y1 = y0 + 1.0;
            const Vec3 f00 = At(posX, posY, posZ, b, u0, y0, frontD);
            const Vec3 f10 = At(posX, posY, posZ, b, u1, y0, frontD);
            const Vec3 f11 = At(posX, posY, posZ, b, u1, y1, frontD);
            const Vec3 f01 = At(posX, posY, posZ, b, u0, y1, frontD);
            const Vec3 b00 = At(posX, posY, posZ, b, u0, y0, backD);
            const Vec3 b10 = At(posX, posY, posZ, b, u1, y0, backD);
            const Vec3 b11 = At(posX, posY, posZ, b, u1, y1, backD);
            const Vec3 b01 = At(posX, posY, posZ, b, u0, y1, backD);

            // Back, full texture once per block. u runs opposite the front so the
            // plank pattern matches PaintingRenderer after the facing rotation.
            quads.push_back(MakeQuad(b00, b01, b11, b10, 1, 0, 1, 1, 0, 1, 0, 0, false));

            // Top (+Y): texture-top of the 1px strip on the front edge.
            quads.push_back(MakeQuad(f01, f11, b11, b01, 0, 1, 1, 1, 1, 1.0f - kStrip, 0, 1.0f - kStrip, false));
            // Bottom (-Y): texture-top of the strip on the back edge.
            quads.push_back(MakeQuad(b00, b10, f10, f00, 0, 1, 1, 1, 1, 1.0f - kStrip, 0, 1.0f - kStrip, false));
            // u-small side (outward -U): strip u=0 on the front.
            quads.push_back(MakeQuad(f00, f01, b01, b00, 0, 0, 0, 1, kStrip, 1, kStrip, 0, false));
            // u-large side (outward +U): strip u=0 on the back.
            quads.push_back(MakeQuad(f10, b10, b11, f11, kStrip, 0, 0, 0, 0, 1, kStrip, 1, false));
        }
    }
    return quads;
}

size_t Export() {
    const std::string dir = getExecutableDir();
    const std::string objPath = dir + "paintings.obj";
    const std::string mtlPath = dir + "paintings.mtl";

    auto finishEmpty = [&]() -> size_t {
        RemoveStale(objPath, mtlPath);
        return 0;
    };

    const fs::path entitiesDir = EntitiesDirectory();
    if (!fs::is_directory(entitiesDir)) {
        std::cout << "[Paintings] Directory not found: " << entitiesDir.string() << std::endl;
        return finishEmpty();
    }

    const int rx0 = FloorDiv(config.minX, 512);
    const int rx1 = FloorDiv(config.maxX, 512);
    const int rz0 = FloorDiv(config.minZ, 512);
    const int rz1 = FloorDiv(config.maxZ, 512);

    std::vector<Collected> paintings;
    std::unordered_set<std::string> unknown;
    for (int rx = rx0; rx <= rx1; ++rx) {
        for (int rz = rz0; rz <= rz1; ++rz) {
            const fs::path regionPath = entitiesDir / ("r." + std::to_string(rx) + "." + std::to_string(rz) + ".mca");
            std::error_code ec;
            if (!fs::is_regular_file(regionPath, ec)) continue;
            std::ifstream in(regionPath, std::ios::binary);
            if (!in) {
                std::cerr << "[Paintings] Failed to open " << regionPath.string() << std::endl;
                continue;
            }
            std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            for (size_t slot = 0; slot < 1024; ++slot) {
                NbtTagPtr root;
                if (!ReadChunkNbt(bytes, slot, root)) continue;
                auto list = getChildByName(root, "Entities");
                if (!list) list = getChildByName(root, "entities");
                if (!list) continue;
                for (const auto& entity : list->children) CollectEntity(entity, paintings, unknown);
            }
        }
    }
    for (const auto& id : unknown) {
        std::cerr << "[Paintings] Skipping unknown variant: " << id << std::endl;
    }
    if (paintings.empty()) {
        std::cout << "[Paintings] No paintings in range" << std::endl;
        return finishEmpty();
    }

    std::unordered_map<std::string, std::string> savedTextures;
    const std::string backRel = EnsureTexture("minecraft", "painting/back", savedTextures);
    const std::string backName = "minecraft:painting/back";

    std::vector<std::string> materialNames;
    std::vector<std::pair<std::string, std::string>> materialFiles;
    std::unordered_map<std::string, int> materialIndex;
    auto materialOf = [&](const std::string& name, const std::string& rel) -> int {
        auto it = materialIndex.find(name);
        if (it != materialIndex.end()) return it->second;
        const int index = static_cast<int>(materialNames.size());
        materialNames.push_back(name);
        materialFiles.emplace_back(name, rel);
        materialIndex.emplace(name, index);
        return index;
    };
    const int backMaterial = materialOf(backName, backRel);

    std::vector<float> vertices;
    std::vector<float> uvs;
    std::vector<OutFace> faces;
    for (const Collected& painting : paintings) {
        const std::string frontRel = EnsureTexture(painting.variant.textureNs, painting.variant.texturePath, savedTextures);
        const int frontMaterial = materialOf(painting.variant.materialName, frontRel);
        const auto mesh = BuildPaintingMesh(painting.x, painting.y, painting.z,
            painting.facing, painting.variant.width, painting.variant.height);
        for (const auto& quad : mesh) {
            OutFace face;
            face.material = quad.front ? frontMaterial : backMaterial;
            for (int k = 0; k < 4; ++k) {
                face.v[k] = static_cast<int>(vertices.size() / 3) + 1;
                vertices.push_back(quad.x[k]);
                vertices.push_back(quad.y[k]);
                vertices.push_back(quad.z[k]);
                face.vt[k] = static_cast<int>(uvs.size() / 2) + 1;
                uvs.push_back(quad.u[k]);
                uvs.push_back(quad.v[k]);
            }
            faces.push_back(face);
        }
    }

    WriteMtl(mtlPath, materialFiles);
    WriteObj(objPath, "paintings.mtl", vertices, uvs, faces, materialNames);
    std::cout << "[Paintings] Exported " << paintings.size() << " paintings -> " << objPath << std::endl;
    return paintings.size();
}

} // namespace PaintingExporter
