#include "EntityModelExporter.h"
#include "EntityExporter.h"
#include "EntityModel.h"
#include "EntityNbt.h"
#include "ItemRender.h"
#include "GlobalCache.h"
#include "block.h"
#include "config.h"
#include "nbtutils.h"
#include "objExporter.h"
#include "texture.h"
#include "blocktint.h"
#include "include/stb_image.h"
#include "include/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

std::string getExecutableDir();  // objExporter.cpp

namespace fs = std::filesystem;

namespace {
constexpr double kPi = 3.14159265358979323846;

using namespace EntityNbt;

// minecraft:scale 属性 (1.20.5+: attributes[{id, base}]; 旧版 Attributes[{Name, Base}])
double ScaleAttribute(const NbtTagPtr& e) {
    for (const char* listName : { "attributes", "Attributes" }) {
        auto list = getChildByName(e, listName);
        if (!list || list->type != TagType::LIST) continue;
        for (const auto& a : list->children) {
            std::string id = Str(a, "id"); if (id.empty()) id = Str(a, "Name");
            if (id == "minecraft:scale" || id == "minecraft:generic.scale" || id == "generic.scale" || id == "scale") {
                double b = NumOr(a, "base", NumOr(a, "Base", 1.0));
                return std::clamp(b, 0.0625, 16.0);
            }
        }
    }
    return 1.0;
}

// ---------- output collection ----------
struct OutMaterial { std::string name, texture; };
struct OutObject { std::string name; ModelData mesh; };

struct Ctx {
    bool modern = false;
    std::vector<OutMaterial> materials;
    std::unordered_map<std::string, int> materialIndex;
    std::unordered_map<std::string, std::string> textureCache;  // ns:path -> relative png ("" = missing)
    std::vector<OutObject> objects;
    std::map<std::string, int> counters;
    std::map<std::string, int> exported;
    std::map<std::string, int> skippedItems;                    // 无法渲染的物品 (特殊渲染器等)
    std::map<std::string, int> skippedEntities;                 // 没有模型的实体 id
    std::map<std::string, TintResult> tints;                    // 材质名 -> 非固定色 (并入 tint.json)
    std::map<std::string, std::string> tintedTex;               // rel+color -> 烘焙后的 png
    std::map<std::string, std::string> paletteTex;              // mask|key|pal -> 烘焙后的 png
    std::set<std::pair<int, int>> blockChunks;                  // BlockName 载入的区块, 析构时释放

    Ctx() = default;
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    ~Ctx() {
        for (const auto& [cx, cz] : blockChunks) ClearSectionCacheForChunk(cx, cz);
    }
    // 实体所在位置的方块名; 同一区块只解析一次 (大量鱼时逐个载入/释放区块很慢)
    std::string BlockName(int x, int y, int z) {
        const int cx = x >> 4, cz = z >> 4;
        if (blockChunks.insert({ cx, cz }).second) LoadAndCacheBlockData(cx, cz);
        return GetBlockById(GetBlockId(x, y, z)).name;
    }

    // 贴图写到 textures/<ns>/<path>.png, 返回相对路径; 不存在返回 ""
    std::string Texture(const std::string& ns, const std::string& path) {
        std::string key = ns + ":" + path;
        auto it = textureCache.find(key);
        if (it != textureCache.end()) return it->second;
        std::string dir = "textures", rel;
        if (SaveTextureToFile(ns, path, dir)) {
            rel = "textures/" + ns + "/" + path + ".png";
            RegisterTexture(ns, path, rel);
        }
        textureCache[key] = rel;
        return rel;
    }
    // 实体材质落在 CI- 组, 插件没有 tint 输入。固定色乘进贴图副本 (失败则原路径)。
    std::string TintedTexture(const std::string& rel, uint32_t rgb) {
        if (rel.empty()) return rel;
        const uint32_t color = rgb & 0xFFFFFFu;
        std::ostringstream hex;
        hex << std::uppercase << std::hex << std::setw(6) << std::setfill('0') << color;
        const std::string cacheKey = rel + hex.str();
        if (auto it = tintedTex.find(cacheKey); it != tintedTex.end()) return it->second;

        const std::string tag = "_t" + hex.str();
        std::string out = rel;
        if (out.size() >= 4 && out.compare(out.size() - 4, 4, ".png") == 0)
            out.insert(out.size() - 4, tag);
        else
            out += tag;

        int w = 0, h = 0, n = 0;
        unsigned char* px = stbi_load(rel.c_str(), &w, &h, &n, 4);
        if (!px || w <= 0 || h <= 0) {
            if (px) stbi_image_free(px);
            tintedTex.emplace(cacheKey, rel);
            return rel;
        }
        const double sr = static_cast<double>((color >> 16) & 255) / 255.0;
        const double sg = static_cast<double>((color >> 8) & 255) / 255.0;
        const double sb = static_cast<double>(color & 255) / 255.0;
        auto mul = [](unsigned char v, double f) -> unsigned char {
            const long r = std::lround(static_cast<double>(v) * f);
            if (r <= 0) return 0;
            if (r >= 255) return 255;
            return static_cast<unsigned char>(r);
        };
        const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
        for (size_t i = 0; i < count; ++i) {
            unsigned char* p = px + i * 4;
            p[0] = mul(p[0], sr);
            p[1] = mul(p[1], sg);
            p[2] = mul(p[2], sb);
        }
        const int wrote = stbi_write_png(out.c_str(), w, h, 4, px, w * 4);
        stbi_image_free(px);
        if (!wrote) {
            tintedTex.emplace(cacheKey, rel);
            return rel;
        }
        tintedTex.emplace(cacheKey, out);
        return out;
    }
    // 纹饰调色板置换 (1.21 ABGR / 26.x ARGB 在字节序上都是 RGBA):
    // 键图像 alpha!=0 的不透明 RGB 映射到目标色; 源 alpha==0 保持; 输出 alpha = srcA * dstA / 255。
    std::string PaletteSwapped(const std::string& maskPath, const std::string& keyPath, const std::string& palPath) {
        const std::string cacheKey = maskPath + "|" + keyPath + "|" + palPath;
        if (auto it = paletteTex.find(cacheKey); it != paletteTex.end()) return it->second;
        auto fail = [&]() { paletteTex.emplace(cacheKey, ""); return std::string(); };
        std::vector<unsigned char> src, keyPx, palPx;
        int sw = 0, sh = 0, kw = 0, kh = 0, pw = 0, ph = 0;
        if (!LoadTexturePixels("minecraft", maskPath, src, sw, sh) || sw <= 0 || sh <= 0) return fail();
        if (!LoadTexturePixels("minecraft", keyPath, keyPx, kw, kh)) return fail();
        if (!LoadTexturePixels("minecraft", palPath, palPx, pw, ph)) return fail();
        const size_t kn = static_cast<size_t>(kw) * static_cast<size_t>(kh);
        const size_t pn = static_cast<size_t>(pw) * static_cast<size_t>(ph);
        if (kn == 0 || kn != pn || keyPx.size() < kn * 4 || palPx.size() < pn * 4) return fail();
        std::unordered_map<uint32_t, uint32_t> remap;
        for (size_t i = 0; i < kn; ++i) {
            const unsigned char* k = keyPx.data() + i * 4;
            if (k[3] == 0) continue;
            const uint32_t rgb = (static_cast<uint32_t>(k[0]) << 16) | (static_cast<uint32_t>(k[1]) << 8) | k[2];
            const unsigned char* t = palPx.data() + i * 4;
            remap[rgb] = (static_cast<uint32_t>(t[0]) << 24) | (static_cast<uint32_t>(t[1]) << 16) |
                         (static_cast<uint32_t>(t[2]) << 8) | t[3];
        }
        const size_t count = static_cast<size_t>(sw) * static_cast<size_t>(sh);
        if (src.size() < count * 4) return fail();
        for (size_t i = 0; i < count; ++i) {
            unsigned char* p = src.data() + i * 4;
            if (p[3] == 0) continue;
            const uint32_t rgb = (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
            auto it = remap.find(rgb);
            if (it == remap.end()) continue;
            const uint32_t t = it->second;
            const int ta = static_cast<int>(t & 255);
            p[0] = static_cast<unsigned char>((t >> 24) & 255);
            p[1] = static_cast<unsigned char>((t >> 16) & 255);
            p[2] = static_cast<unsigned char>((t >> 8) & 255);
            p[3] = static_cast<unsigned char>(static_cast<int>(p[3]) * ta / 255);
        }
        std::string full = "textures";
        if (!SaveTextureToFile("minecraft", maskPath, full)) return fail();
        std::string safe = palPath;
        for (char& c : safe) if (c == '/') c = '_';
        const std::filesystem::path dest = std::filesystem::path(full).parent_path() /
                                            (std::filesystem::path(full).stem().string() + "_pal_" + safe + ".png");
        if (!stbi_write_png(dest.string().c_str(), sw, sh, 4, src.data(), sw * 4)) return fail();
        std::string rel = "textures/minecraft/" + maskPath + ".png";
        rel.insert(rel.size() - 4, "_pal_" + safe);
        paletteTex.emplace(cacheKey, rel);
        return rel;
    }
    int Material(const std::string& name, const std::string& texture) {
        auto it = materialIndex.find(name);
        if (it != materialIndex.end()) return it->second;
        int idx = static_cast<int>(materials.size());
        materials.push_back({ name, texture });
        materialIndex[name] = idx;
        return idx;
    }
    OutObject& NewObject(const std::string& id) {
        std::string base = SplitId(id).second;
        int n = counters[base]++;
        objects.push_back({ base + "_" + std::to_string(n), ModelData() });
        exported[id]++;
        return objects.back();
    }
};

bool TextureExists(const std::string& ns, const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    return GlobalCache::textureIndex.count("textures:" + ns + ":" + path) > 0;
}
// Overlay sheets are sampled with the body mesh UVs, so the source pngs must be the same size.
// Read IHDR from the jar bytes (not the exported path) so the check does not depend on cwd.
bool CachedPngSize(const std::string& ns, const std::string& path, int& w, int& h) {
    std::vector<unsigned char> png;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto idx = GlobalCache::textureIndex.find("textures:" + ns + ":" + path);
        if (idx == GlobalCache::textureIndex.end()) return false;
        auto it = GlobalCache::textures.find(idx->second);
        if (it == GlobalCache::textures.end()) return false;
        png = it->second;
    }
    return GetPNGDimensions(png, w, h) && w > 0 && h > 0;
}
bool SameCachedPngSize(const std::string& nsA, const std::string& pathA, const std::string& nsB, const std::string& pathB) {
    int aw = 0, ah = 0, bw = 0, bh = 0;
    return CachedPngSize(nsA, pathA, aw, ah) && CachedPngSize(nsB, pathB, bw, bh) && aw == bw && ah == bh;
}

// 村民贴图 .png.mcmeta 的 {"villager": {"hat": "none|partial|full"}}
enum class Hat { None, Partial, Full };
Hat HatOf(const std::string& ns, const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    auto idx = GlobalCache::mcmetaIndex.find("mcmetas:" + ns + ":" + path);
    if (idx == GlobalCache::mcmetaIndex.end()) return Hat::None;
    auto it = GlobalCache::mcmetaCache.find(idx->second);
    if (it == GlobalCache::mcmetaCache.end()) return Hat::None;
    const auto& j = it->second;
    if (!j.is_object() || !j.contains("villager") || !j["villager"].is_object()) return Hat::None;
    std::string hat = j["villager"].value("hat", "none");
    if (hat == "full") return Hat::Full;
    if (hat == "partial") return Hat::Partial;
    return Hat::None;
}

// ---------- pose / placement ----------
struct Placement {
    double x = 0, y = 0, z = 0;
    double yaw = 0, pitch = 0;      // Rotation[0], Rotation[1]
    double scale = 1.0;             // minecraft:scale 属性
    bool sleeping = false;
    int bedDx = 0, bedDz = 0;       // 床朝向 (BedBlock FACING), 0/0 = 未知
};

// 床方块朝向 (SleepingX/Y/Z 或 26.x sleeping_pos 处的 *_bed[facing=...])
bool BedFacing(int bx, int by, int bz, int& dx, int& dz) {
    try {
        int cx = bx >> 4, cz = bz >> 4;
        LoadAndCacheBlockData(cx, cz);
        Block b = GetBlockById(GetBlockId(bx, by, bz));
        ClearSectionCacheForChunk(cx, cz);
        if (b.name.find("_bed") == std::string::npos) return false;
        size_t f = b.name.find("facing");  // 方块名属性写作 facing:south (调色板) 或 facing=south
        if (f == std::string::npos || f + 7 > b.name.size()) return false;
        std::string v = b.name.substr(f + 7);
        v = v.substr(0, v.find_first_of(",]"));
        if (v == "north") { dx = 0; dz = -1; } else if (v == "south") { dx = 0; dz = 1; }
        else if (v == "west") { dx = -1; dz = 0; } else if (v == "east") { dx = 1; dz = 0; }
        else return false;
        return true;
    } catch (...) { return false; }
}

Placement ReadPlacement(const NbtTagPtr& e) {
    Placement p;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() >= 3) { p.x = pos[0]; p.y = pos[1]; p.z = pos[2]; }
    auto rot = NumList(getChildByName(e, "Rotation"));
    if (rot.size() >= 1) p.yaw = rot[0];
    if (rot.size() >= 2) p.pitch = rot[1];
    p.scale = ScaleAttribute(e);
    int sx = 0, sy = 0, sz = 0;
    auto sp = getChildByName(e, "sleeping_pos");  // 26.x
    if (sp && sp->type == TagType::INT_ARRAY) {
        auto v = readIntArray(sp->payload);
        if (v.size() == 3) { p.sleeping = true; sx = v[0]; sy = v[1]; sz = v[2]; }
    } else if (getChildByName(e, "SleepingX")) {  // <=1.21.x
        p.sleeping = true;
        sx = static_cast<int>(NumOr(e, "SleepingX", 0));
        sy = static_cast<int>(NumOr(e, "SleepingY", 0));
        sz = static_cast<int>(NumOr(e, "SleepingZ", 0));
    }
    if (p.sleeping) BedFacing(sx, sy, sz, p.bedDx, p.bedDz);
    return p;
}

// LivingEntityRenderer.submit/render + setupRotations, 到 translate(0,-1.501,0) 为止
EntityAffine LivingBase(const Placement& p, double eyeHeight, double rendererScale) {
    using A = EntityAffine;
    A m = A::T(p.x, p.y, p.z);
    if (p.sleeping && (p.bedDx || p.bedDz)) {
        double off = eyeHeight - 0.1;
        m = m * A::T(-p.bedDx * off, 0.0, -p.bedDz * off);
    }
    m = m * A::S(p.scale);
    if (!p.sleeping) {
        m = m * A::RYDeg(180.0 - p.yaw);
    } else {
        double angle = p.yaw;  // sleepDirectionToRotation
        if (p.bedDx == 0 && p.bedDz == 1) angle = 90.0;
        else if (p.bedDx == -1) angle = 0.0;
        else if (p.bedDx == 0 && p.bedDz == -1) angle = 270.0;
        else if (p.bedDx == 1) angle = 180.0;
        m = m * A::RYDeg(angle) * A::RZDeg(90.0) * A::RYDeg(270.0);
    }
    return m * A::S(-1.0, -1.0, 1.0) * A::S(rendererScale) * A::T(0.0, -1.501, 0.0);
}

// ---------- equipment ----------
// <=1.21.4: ArmorItems [feet, legs, chest, head] + HandItems [main, off]; 1.21.5+: equipment{head, chest, ...}
struct Equipment {
    ItemStackInfo head, chest, legs, feet, mainhand, offhand;
    bool leftHanded = false;
};
Equipment ReadEquipment(const NbtTagPtr& e) {
    Equipment q;
    if (auto eq = getChildByName(e, "equipment"); eq && eq->type == TagType::COMPOUND) {
        q.head = ItemRender::ParseItemStack(getChildByName(eq, "head"));
        q.chest = ItemRender::ParseItemStack(getChildByName(eq, "chest"));
        q.legs = ItemRender::ParseItemStack(getChildByName(eq, "legs"));
        q.feet = ItemRender::ParseItemStack(getChildByName(eq, "feet"));
        q.mainhand = ItemRender::ParseItemStack(getChildByName(eq, "mainhand"));
        q.offhand = ItemRender::ParseItemStack(getChildByName(eq, "offhand"));
    }
    if (auto armor = getChildByName(e, "ArmorItems"); armor && armor->type == TagType::LIST) {
        const auto& c = armor->children;
        if (c.size() > 0) q.feet = ItemRender::ParseItemStack(c[0]);
        if (c.size() > 1) q.legs = ItemRender::ParseItemStack(c[1]);
        if (c.size() > 2) q.chest = ItemRender::ParseItemStack(c[2]);
        if (c.size() > 3) q.head = ItemRender::ParseItemStack(c[3]);
    }
    if (auto hands = getChildByName(e, "HandItems"); hands && hands->type == TagType::LIST) {
        const auto& c = hands->children;
        if (c.size() > 0) q.mainhand = ItemRender::ParseItemStack(c[0]);
        if (c.size() > 1) q.offhand = ItemRender::ParseItemStack(c[1]);
    }
    q.leftHanded = NumOr(e, "LeftHanded", 0) != 0;
    return q;
}

enum class Slot { None, Head, Chest, Legs, Feet };
struct ArmorInfo { Slot slot = Slot::None; std::string legacyName, asset; };
// 原版盔甲物品 -> 槽位 + 旧版贴图名 (models/armor/<name>_layer_N) + 1.21.2+ 装备资源 id
ArmorInfo ArmorOf(const ItemStackInfo& item) {
    ArmorInfo a;
    auto [ns, p] = SplitId(item.id);
    auto ends = [&](const char* s) { std::string t(s); return p.size() > t.size() && p.compare(p.size() - t.size(), t.size(), t) == 0; };
    if (ends("_helmet")) a.slot = Slot::Head;
    else if (ends("_chestplate")) a.slot = Slot::Chest;
    else if (ends("_leggings")) a.slot = Slot::Legs;
    else if (ends("_boots")) a.slot = Slot::Feet;
    else return a;
    std::string mat = p.substr(0, p.rfind('_'));
    if (mat == "golden") mat = "gold";
    a.legacyName = mat;
    a.asset = mat;
    if (p == "turtle_helmet") { a.legacyName = "turtle"; a.asset = "turtle_scute"; }
    if (!item.equipAsset.empty()) a.asset = item.equipAsset;
    else a.asset = ns + ":" + a.asset;
    if (ns != "minecraft" && item.equipAsset.empty()) a.slot = Slot::None;  // 模组盔甲贴图无从推断
    return a;
}

// Blender 材质名 63 字节安全化统一在 objExporter.cpp / objExporter.h（BlenderSafeName），
// 这里直接复用，保证实体与方块两条导出路径的材质名口径一致。
void RegisterEntityOverlay(const std::string& baseName, const std::vector<OverlayLayerInfo>& overlays) {
    RegisterOverlaySequence(BlenderSafeName(baseName), overlays);
}

// 网格 (ModelData + 自带材质) 并入实体对象。固定色烘焙进贴图; 其它 tint 记入 ctx.tints。
void AppendMesh(Ctx& ctx, OutObject& obj, const ModelData& m);

// 盔甲层材质: 基础层 + 叠加层 (皮革: 染色基础层 + 不染色 overlay)。
// nameSuffix / extra 用于纹饰: 材质名加上后缀, 纹饰和染色叠加放进同一次 RegisterOverlaySequence。
struct ArmorTexLayer { std::string texNs, texPath; int color = -1; };
int ArmorMaterial(Ctx& ctx, const std::vector<ArmorTexLayer>& layers, const std::string& nameSuffix = {},
                  const std::vector<OverlayLayerInfo>& extra = {}) {
    if (layers.empty()) return -1;
    auto nameOf = [&](const ArmorTexLayer& l) {
        TintResult t;
        if (l.color >= 0) { t.kind = TintKind::Fixed; t.color = static_cast<uint32_t>(l.color); }
        return std::make_pair(l.texNs + ":" + l.texPath + TintSuffix(t), t);
    };
    std::string baseTex = ctx.Texture(layers[0].texNs, layers[0].texPath);
    if (baseTex.empty()) return -1;
    auto [baseName, baseTint] = nameOf(layers[0]);
    baseName += nameSuffix;
    if (baseTint.kind == TintKind::Fixed) baseTex = ctx.TintedTexture(baseTex, baseTint.color);
    else if (baseTint.on()) ctx.tints[baseName] = baseTint;
    int mat = ctx.Material(baseName, baseTex);
    std::vector<OverlayLayerInfo> over;
    for (size_t i = 1; i < layers.size(); ++i) {
        std::string t = ctx.Texture(layers[i].texNs, layers[i].texPath);
        if (t.empty()) continue;
        OverlayLayerInfo info;
        auto [n, tint] = nameOf(layers[i]);
        info.name = n;
        if (tint.kind == TintKind::Fixed) info.texturePath = ctx.TintedTexture(t, tint.color);
        else { info.texturePath = t; info.tint = tint; }
        over.push_back(info);
    }
    for (const auto& e : extra) over.push_back(e);
    if (!over.empty()) RegisterEntityOverlay(baseName, over);
    return mat;
}

// 纹饰调色板后缀。1.21: 盔甲材料与纹饰材料相同则用 <mat>_darker。26.x: equipment trim_overrides。
std::string TrimPaletteId(const Ctx& ctx, const ArmorInfo& armor, const ItemStackInfo& item) {
    const std::string mat = SplitId(item.trimMaterial).second;
    if (mat.empty()) return {};
    if (!ctx.modern) {
        if (armor.legacyName == mat &&
            (mat == "iron" || mat == "gold" || mat == "diamond" || mat == "netherite"))
            return mat + "_darker";
        return mat;
    }
    std::string pal = "trim/" + mat;
    auto [ns, path] = SplitId(armor.asset);
    nlohmann::json def = ItemRender::AssetJson(ns, "equipment/" + path + ".json");
    if (def.is_object() && def.contains("trim_overrides") && def["trim_overrides"].is_array()) {
        for (const auto& o : def["trim_overrides"]) {
            if (!o.is_object() || !o.contains("when") || !o["when"].is_object()) continue;
            if (!o["when"].contains("material") || !o["when"]["material"].is_string()) continue;
            const std::string w = o["when"]["material"].get<std::string>();
            if (w != item.trimMaterial && SplitId(w).second != mat) continue;
            if (o.contains("palette") && o["palette"].is_string())
                pal = SplitId(o["palette"].get<std::string>()).second;
            break;
        }
    }
    return pal;
}
// 把图案蒙版按调色板烘焙成与盔甲同 UV 的叠加层。失败则不加纹饰 (盔甲本身照常)。
bool MakeTrimOverlay(Ctx& ctx, const ArmorInfo& armor, const ItemStackInfo& item, bool leggings,
                     const ArmorTexLayer& armorLayer, std::string& suffix, OverlayLayerInfo& info) {
    const std::string pattern = SplitId(item.trimPattern).second;
    const std::string pal = TrimPaletteId(ctx, armor, item);
    if (pattern.empty() || pal.empty()) return false;
    const std::string mask = ctx.modern
        ? std::string("trims/entity/") + (leggings ? "humanoid_leggings/" : "humanoid/") + pattern
        : std::string("trims/models/armor/") + pattern + (leggings ? "_leggings" : "");
    const std::string keyPath = ctx.modern ? "palettes/trim_base" : "trims/color_palettes/trim_palette";
    const std::string palPath = ctx.modern ? "palettes/" + pal : "trims/color_palettes/" + pal;
    if (!TextureExists("minecraft", mask) || !TextureExists("minecraft", keyPath) || !TextureExists("minecraft", palPath))
        return false;
    if (!SameCachedPngSize(armorLayer.texNs, armorLayer.texPath, "minecraft", mask)) return false;
    const std::string baked = ctx.PaletteSwapped(mask, keyPath, palPath);
    if (baked.empty()) return false;
    std::string safePal = pal;
    for (char& c : safePal) if (c == '/') c = '_';
    info.name = "minecraft:" + mask + "~" + safePal;
    info.texturePath = baked;
    suffix = "~trim_" + pattern + "_" + safePal;
    return true;
}

// 项圈: 基础贴图材质 + 染色项圈叠加层。整模再画一遍项圈贴图 (大半透明) 在 Blender 里是黑剪影。
int CollaredMaterial(Ctx& ctx, const std::string& baseMatName, const std::string& baseTex,
                     const std::string& collarNs, const std::string& collarPath, int collarRgb,
                     const char* tag = "collar") {
    if (baseTex.empty()) return -1;
    std::ostringstream hex;
    hex << std::uppercase << std::hex << std::setw(6) << std::setfill('0')
        << (static_cast<unsigned>(collarRgb) & 0xFFFFFFu);
    const std::string name = baseMatName + "~" + tag + "_" + hex.str();
    int mat = ctx.Material(name, baseTex);
    std::string ctex = ctx.Texture(collarNs, collarPath);
    if (!ctex.empty()) {
        TintResult t;
        t.kind = TintKind::Fixed;
        t.color = static_cast<uint32_t>(collarRgb) & 0xFFFFFFu;
        OverlayLayerInfo info;
        info.name = "minecraft:" + collarPath + TintSuffix(t);
        info.texturePath = ctx.TintedTexture(ctex, t.color);
        RegisterEntityOverlay(name, { info });
    }
    return mat;
}

// equipment/.../<type>/saddle -> <type>; any other path -> its file stem.
std::string OverlayStem(const std::string& path) {
    const size_t slash = path.rfind('/');
    std::string stem = slash == std::string::npos ? path : path.substr(slash + 1);
    if (stem == "saddle" && slash != std::string::npos && slash > 0) {
        const size_t prev = path.rfind('/', slash - 1);
        const size_t begin = prev == std::string::npos ? 0 : prev + 1;
        stem = path.substr(begin, slash - begin);
    }
    return stem;
}
// Coplanar decor (same triangles and UVs as the body). A second mesh of that geometry draws as a
// black silhouette, so the sheet is an overlay on the body material. The material name is
// base~stem so it does not collide with the unsaddled body. Layers whose png size differs from
// the body texture are skipped: an overlay is sampled with the body UVs.
std::string BodyWithOverlays(Ctx& ctx, std::string baseMat, const std::string& bodyNs, const std::string& bodyPath,
                             const std::string& baseTex, const std::vector<ArmorTexLayer>& layers) {
    if (baseTex.empty() || layers.empty()) return baseMat;
    std::vector<OverlayLayerInfo> overs;
    std::string suffix;
    for (const auto& l : layers) {
        if (!SameCachedPngSize(bodyNs, bodyPath, l.texNs, l.texPath)) continue;
        const std::string tex = ctx.Texture(l.texNs, l.texPath);
        if (tex.empty()) continue;
        OverlayLayerInfo info;
        if (l.color >= 0) {
            TintResult tint;
            tint.kind = TintKind::Fixed;
            tint.color = static_cast<uint32_t>(l.color) & 0xFFFFFFu;
            info.texturePath = ctx.TintedTexture(tex, tint.color);
            info.name = l.texNs + ":" + l.texPath + TintSuffix(tint);
        } else {
            info.texturePath = tex;
            info.name = l.texNs + ":" + l.texPath;
        }
        overs.push_back(std::move(info));
        suffix += "~" + OverlayStem(l.texPath);
    }
    if (overs.empty()) return baseMat;
    baseMat += suffix;
    RegisterEntityOverlay(baseMat, overs);
    return baseMat;
}

// HumanoidArmorLayer: kind = "minecraft:zombie_villager" 等; parent = 已摆好姿态的主模型
void RenderArmor(Ctx& ctx, OutObject& obj, const std::string& kind, const EntityModel& parent, const Equipment& eq,
                 const EntityAffine& base, const TopPre& pre, bool baby, bool armorStand) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    static const std::vector<std::string> kParts = { "head", "hat", "body", "right_arm", "left_arm", "right_leg", "left_leg" };
    const std::pair<Slot, const ItemStackInfo*> order[] = {
        { Slot::Chest, &eq.chest }, { Slot::Legs, &eq.legs }, { Slot::Feet, &eq.feet }, { Slot::Head, &eq.head } };
    for (const auto& [slot, item] : order) {
        if (item->empty()) continue;
        ArmorInfo a = ArmorOf(*item);
        if (a.slot != slot) continue;
        std::vector<ArmorTexLayer> layers;
        std::string layerId;
        if (!ctx.modern) {
            layerId = kind + (slot == Slot::Legs ? "#inner_armor" : "#outer_armor");
            std::string tex = "models/armor/" + a.legacyName + (slot == Slot::Legs ? "_layer_2" : "_layer_1");
            if (a.legacyName == "leather") {
                layers.push_back({ "minecraft", tex, item->dyeColor >= 0 ? item->dyeColor : 0xA06540 });
                layers.push_back({ "minecraft", tex + "_overlay", -1 });
            } else {
                layers.push_back({ "minecraft", tex, -1 });
            }
        } else {
            static const char* kSlotLayer[] = { "", "#helmet", "#chestplate", "#leggings", "#boots" };
            layerId = kind + (baby && !armorStand ? "_baby" : "") + kSlotLayer[static_cast<int>(slot)];
            std::string type = (baby && !armorStand) ? "humanoid_baby" : (slot == Slot::Legs ? "humanoid_leggings" : "humanoid");
            auto [ans, apath] = SplitId(a.asset);
            nlohmann::json def = ItemRender::AssetJson(ans, "equipment/" + apath + ".json");
            if (!def.is_object() || !def.contains("layers") || !def["layers"].contains(type)) continue;
            for (const auto& l : def["layers"][type]) {
                if (!l.contains("texture") || !l["texture"].is_string()) continue;
                auto [tns, tpath] = SplitId(l["texture"].get<std::string>());
                int color = -1;
                if (l.contains("dyeable") && l["dyeable"].is_object()) {
                    if (item->dyeColor >= 0) color = item->dyeColor;
                    else if (l["dyeable"].contains("color_when_undyed"))
                        color = static_cast<int>(l["dyeable"]["color_when_undyed"].get<int64_t>() & 0xFFFFFF);
                    else continue;  // 未染色且无默认色: 原版不画该层
                }
                layers.push_back({ tns, "entity/equipment/" + type + "/" + tpath, color });
            }
        }
        EntityModel am(fam, layerId);
        if (!am.Valid()) continue;
        am.CopyPoseFrom(parent, kParts, !ctx.modern);  // 旧版 copyPropertiesTo (位置+旋转); 26.x 同一 setupAnim
        if (!ctx.modern) {  // 旧版 setPartVisibility; 26.x 每个槽位是独立的层 (只含该槽位的方块)
            switch (slot) {
            case Slot::Head: am.HideAllBut({ "head", "hat" }); break;
            case Slot::Chest: am.HideAllBut({ "body", "right_arm", "left_arm" }); break;
            case Slot::Legs: am.HideAllBut({ "body", "right_leg", "left_leg" }); break;
            case Slot::Feet: am.HideAllBut({ "right_leg", "left_leg" }); break;
            default: break;
            }
        }
        std::string trimSuffix;
        std::vector<OverlayLayerInfo> trimOver;
        const bool babyHumanoid = ctx.modern && baby && !armorStand;
        if (!babyHumanoid && !layers.empty() && !item->trimPattern.empty()) {
            OverlayLayerInfo info;
            if (MakeTrimOverlay(ctx, a, *item, slot == Slot::Legs, layers[0], trimSuffix, info))
                trimOver.push_back(info);
        }
        int mat = ArmorMaterial(ctx, layers, trimSuffix, trimOver);
        if (mat < 0) continue;
        am.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
    }
}

void AppendMesh(Ctx& ctx, OutObject& obj, const ModelData& m) {
    std::vector<int> remap(m.materials.size(), -1);
    for (size_t i = 0; i < m.materials.size(); ++i) {
        const auto& mat = m.materials[i];
        if (mat.texturePath.empty()) continue;
        std::string tex = mat.texturePath;
        if (mat.tint.kind == TintKind::Fixed) tex = ctx.TintedTexture(tex, mat.tint.color);
        else if (mat.tint.on()) ctx.tints[mat.name] = mat.tint;
        remap[i] = ctx.Material(mat.name, tex);
    }
    ModelData& out = obj.mesh;
    int vBase = static_cast<int>(out.vertices.size() / 3), uBase = static_cast<int>(out.uvCoordinates.size() / 2);
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    out.uvCoordinates.insert(out.uvCoordinates.end(), m.uvCoordinates.begin(), m.uvCoordinates.end());
    for (auto f : m.faces) {
        if (f.materialIndex < 0 || f.materialIndex >= static_cast<int>(remap.size()) || remap[f.materialIndex] < 0) continue;
        for (auto& i : f.vertexIndices) i += vBase;
        for (auto& i : f.uvIndices) i += uBase;
        f.materialIndex = remap[f.materialIndex];
        f.faceDirection = DO_NOT_CULL;
        out.faces.push_back(f);
    }
}

void AppendItem(Ctx& ctx, OutObject& obj, const ItemStackInfo& item, ItemDisplay display, bool left, const EntityAffine& m) {
    ModelData mesh;
    if (!ItemRender::Build(item, display, left, mesh)) { ctx.skippedItems[item.id]++; return; }
    ItemRender::Transform(mesh, m);
    AppendMesh(ctx, obj, mesh);
}

// ItemInHandLayer (右手 = 主手, 除非 LeftHanded)
void RenderHeldItems(Ctx& ctx, OutObject& obj, const EntityModel& model, const Equipment& eq, const EntityAffine& base,
                     bool legacyYoung, bool modernBabyOffset) {
    using A = EntityAffine;
    const ItemStackInfo& right = eq.leftHanded ? eq.offhand : eq.mainhand;
    const ItemStackInfo& left = eq.leftHanded ? eq.mainhand : eq.offhand;
    for (int side = 0; side < 2; ++side) {
        const ItemStackInfo& item = side == 0 ? right : left;
        if (item.empty()) continue;
        bool isLeft = side == 1;
        int arm = model.Find(isLeft ? "left_arm" : "right_arm");
        if (arm < 0) continue;
        A b = base;
        if (legacyYoung) b = b * A::T(0.0, 0.75, 0.0) * A::S(0.5);
        A m = model.PartTransform(arm, b);
        double ox = modernBabyOffset ? 0.0 : 1.0, oy = modernBabyOffset ? 1.0 : 2.0, oz = modernBabyOffset ? -4.5 : -10.0;
        m = m * A::RX(-kPi / 2) * A::RY(kPi) * A::T((isLeft ? -ox : ox) / 16.0, oy / 16.0, oz / 16.0);
        AppendItem(ctx, obj, item, isLeft ? ItemDisplay::ThirdPersonLeftHand : ItemDisplay::ThirdPersonRightHand, isLeft, m);
    }
}

bool IsSkullItem(const std::string& id) {
    static const char* kSkulls[] = { "minecraft:skeleton_skull", "minecraft:wither_skeleton_skull", "minecraft:zombie_head",
                                     "minecraft:player_head", "minecraft:creeper_head", "minecraft:dragon_head",
                                     "minecraft:piglin_head" };
    for (auto s : kSkulls) if (id == s) return true;
    return false;
}

// CustomHeadLayer 的头颅分支 (不是 ItemDisplay::Head)。
// 1.21: S(1.1875,-1.1875,-1.1875) * 村民 T(0,0.0625,0); renderSkull 的翻转和 yaw 在 EmitSkullModel。
// 26.x: T(0, skullYOffset, 0) * S(1.1875), 无翻转。村民 skullYOffset = -0.07421875。
void RenderWornSkull(Ctx& ctx, OutObject& obj, const EntityModel& model, const ItemStackInfo& item,
                     const EntityAffine& base, bool legacyYoung, bool villagerLike) {
    using A = EntityAffine;
    int head = model.Find("head");
    if (head < 0) return;
    A m = base;
    if (!ctx.modern && legacyYoung) m = m * A::T(0.0, 0.03125, 0.0) * A::S(0.7) * A::T(0.0, 1.0, 0.0);
    m = model.PartTransform(head, m);
    if (!ctx.modern) {
        m = m * A::S(1.1875, -1.1875, -1.1875);
        if (villagerLike) m = m * A::T(0.0, 0.0625, 0.0);
    } else {
        const double skullY = villagerLike ? -0.07421875 : 0.0;
        m = m * A::T(0.0, skullY, 0.0) * A::S(1.1875);
    }
    ModelData mesh;
    if (!ItemRender::EmitSkullModel(item.id, m, ctx.modern, mesh)) { ctx.skippedItems[item.id]++; return; }
    AppendMesh(ctx, obj, mesh);
}

// CustomHeadLayer: 头部槽里非头盔的物品 (南瓜/方块/任意物品), HEAD 显示上下文
void RenderHeadItem(Ctx& ctx, OutObject& obj, const EntityModel& model, const Equipment& eq, const EntityAffine& base,
                    bool legacyYoung, bool villagerLike, double modernYOffset) {
    using A = EntityAffine;
    const ItemStackInfo& item = eq.head;
    if (item.empty() || ArmorOf(item).slot == Slot::Head) return;
    if (IsSkullItem(item.id)) {
        RenderWornSkull(ctx, obj, model, item, base, legacyYoung, villagerLike);
        return;
    }
    int head = model.Find("head");
    if (head < 0) return;
    A m = base;
    if (!ctx.modern && legacyYoung) m = m * A::T(0.0, 0.03125, 0.0) * A::S(0.7) * A::T(0.0, 1.0, 0.0);
    m = model.PartTransform(head, m);
    double y = -0.25 + (ctx.modern ? modernYOffset : 0.0);
    m = m * A::T(0.0, y, 0.0) * A::RY(kPi) * A::S(0.625, -0.625, -0.625);
    if (!ctx.modern && villagerLike) m = m * A::T(0.0, 0.1875, 0.0);
    AppendItem(ctx, obj, item, ItemDisplay::Head, false, m);
}

// ---------- villager / zombie villager ----------
// VillagerRenderer / ZombieVillagerRenderer + VillagerProfessionLayer (base + type + profession + level)
void RenderVillagerLike(Ctx& ctx, const NbtTagPtr& e, bool zombie) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = zombie ? NumOr(e, "IsBaby", 0) != 0 : NumOr(e, "Age", 0) < 0;
    const std::string kind = zombie ? "zombie_villager" : "villager";
    const std::string layer = "minecraft:" + kind + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, layer);
    if (!model.Valid()) return;

    std::string typeId = "minecraft:plains", profId = "minecraft:none";
    int level = 1;
    if (auto vd = getChildByName(e, "VillagerData")) {
        std::string t = Str(vd, "type"), pr = Str(vd, "profession");
        if (!t.empty()) typeId = t;
        if (!pr.empty()) profId = pr;
        level = static_cast<int>(NumOr(vd, "level", 1));
    }
    auto [typeNs, type] = SplitId(typeId);
    auto [profNs, prof] = SplitId(profId);
    Placement pl = ReadPlacement(e);

    // ---- pose (setupAnim, 静止相位) ----
    model.SetRot("head", static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
    if (zombie) {
        // HumanoidModel.setupAnim + AnimationUtils.animateZombieArms(aggressive=false, attack=0)
        const float armX = static_cast<float>(-kPi / 2.25);
        model.SetRot("right_arm", armX, -0.1f, 0.0f);
        model.SetRot("left_arm", armX, 0.1f, 0.0f);
        // bobModelPart: zRot += ±(cos(t*0.09)*0.05+0.05) 取均值
        model.AddRot("right_arm", 0.0f, 0.0f, 0.05f);
        model.AddRot("left_arm", 0.0f, 0.0f, -0.05f);
        model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
        model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
    }

    // ---- placement ----
    double eye = zombie ? 1.74 : 1.62;
    if (baby) eye *= 0.5;
    double rendererScale = 1.0;
    if (!zombie && !ctx.modern) rendererScale = 0.9375 * (baby ? 0.5 : 1.0);  // 旧版 VillagerRenderer.scale
    EntityAffine base = LivingBase(pl, eye, rendererScale);
    TopPre pre;
    int headTop = model.Find("head");
    if (zombie && !ctx.modern && baby) {
        // 旧版 HumanoidModel(AgeableListModel): 头 1.5/2 缩放 + 16px, 身体 1/2 缩放 + 24px
        pre = [](const std::string& top) {
            if (top == "head") return EntityAffine::S(0.75) * EntityAffine::T(0.0, 1.0, 0.0);
            return EntityAffine::S(0.5) * EntityAffine::T(0.0, 1.5, 0.0);
        };
    }

    // ---- textures & layered materials ----
    const std::string dir = "entity/" + kind + "/";
    std::string baseTex = ctx.Texture("minecraft", dir + kind + (ctx.modern && baby ? "_baby" : ""));
    if (baseTex.empty()) return;
    std::vector<OverlayLayerInfo> layersBody, layersHead;
    const std::string matPrefix = "minecraft:" + kind + "#";
    auto addLayer = [&](std::vector<OverlayLayerInfo>& v, const std::string& ns, const std::string& path,
                        const std::string& layerName) {
        std::string t = ctx.Texture(ns, path);
        if (t.empty()) return;
        OverlayLayerInfo info;
        info.name = matPrefix + layerName;
        info.texturePath = t;
        v.push_back(info);
    };
    Hat typeHat = HatOf(typeNs, dir + "type/" + type);
    Hat profHat = HatOf(profNs, dir + "profession/" + prof);
    bool typeHatVisible = profHat == Hat::None || (profHat == Hat::Partial && typeHat != Hat::Full);
    std::string typePath = dir + (ctx.modern && baby ? "baby/" : "type/") + type;
    addLayer(layersBody, typeNs, typePath, std::string(ctx.modern && baby ? "baby~" : "type~") + type);
    if (typeHatVisible) layersHead = layersBody;
    if (prof != "none" && !baby) {
        addLayer(layersBody, profNs, dir + "profession/" + prof, "profession~" + prof);
        addLayer(layersHead, profNs, dir + "profession/" + prof, "profession~" + prof);
        if (prof != "nitwit") {
            static const char* kLevels[] = { "stone", "iron", "gold", "emerald", "diamond" };
            std::string lv = kLevels[std::clamp(level, 1, 5) - 1];
            addLayer(layersBody, "minecraft", dir + "profession_level/" + lv, "level~" + lv);
            addLayer(layersHead, "minecraft", dir + "profession_level/" + lv, "level~" + lv);
        }
    }
    std::string key = type + "_" + prof + (prof != "none" && prof != "nitwit" && !baby ? "_" + std::to_string(std::clamp(level, 1, 5)) : "") +
                      (baby ? "_baby" : "");
    if (typeNs != "minecraft") key = typeNs + "_" + key;
    std::string bodyName = matPrefix + key;
    int bodyMat = ctx.Material(bodyName, baseTex);
    RegisterEntityOverlay(bodyName, layersBody);
    int headMat = bodyMat;
    if (!typeHatVisible) {
        std::string headName = bodyName + "_nohat";
        headMat = ctx.Material(headName, baseTex);
        RegisterEntityOverlay(headName, layersHead);
    }

    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    model.Emit(obj.mesh, base, [&](int part) {
        return (headTop >= 0 && model.InSubtree(part, headTop)) ? headMat : bodyMat;
    }, pre);

    // ---- equipment layers ----
    using A = EntityAffine;
    Equipment eq = ReadEquipment(e);
    if (zombie) {
        RenderArmor(ctx, obj, "minecraft:zombie_villager", model, eq, base, pre, baby, false);
        RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && baby, ctx.modern && baby);
    } else if (!eq.mainhand.empty()) {
        // CrossedArmsItemLayer (GROUND 上下文)
        A m;
        if (!ctx.modern) {
            m = base * A::T(0.0, 0.4, -0.4) * A::RX(kPi);
        } else {
            int arms = model.Find("arms");
            m = (arms >= 0 ? model.PartTransform(arms, base) : base) * A::RX(0.75) * A::S(1.07) *
                A::T(0.0, 0.13, -0.34) * A::RX(kPi);
        }
        AppendItem(ctx, obj, eq.mainhand, ItemDisplay::Ground, false, m);
    }
    // VillagerRenderer.CUSTOM_HEAD_TRANSFORMS (26.x) / 旧版 villager 偏移
    RenderHeadItem(ctx, obj, model, eq, base, zombie && baby, true, -0.1171875);
}



// ---------- common mob helpers ----------
bool IsBaby(const NbtTagPtr& e) {
    if (NumOr(e, "IsBaby", 0) != 0) return true;
    if (getChildByName(e, "Age") && NumOr(e, "Age", 0) < 0) return true;
    return false;
}
bool IsSitting(const NbtTagPtr& e) {
    return NumOr(e, "Sitting", 0) != 0 || NumOr(e, "sitting", 0) != 0;
}

// 旧版 AgeableListModel.renderToBuffer 幼体参数 (各模型构造函数)
struct AgeableParams {
    bool scaleHead; float yHeadOffset, zHeadOffset, headScale, bodyScale, bodyYOffset;
    std::vector<std::string> headParts;
};
const AgeableParams kAgeHumanoid { true, 16.0f, 0.0f, 2.0f, 2.0f, 24.0f, { "head", "hat" } };
const AgeableParams kAgePig      { false, 4.0f, 4.0f, 2.0f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgeCow      { false, 10.0f, 4.0f, 2.0f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgeSheep    { false, 8.0f, 4.0f, 2.0f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgeDefault  { false, 5.0f, 2.0f, 2.0f, 2.0f, 24.0f, { "head" } };              // WolfModel
const AgeableParams kAgeChicken  { false, 5.0f, 2.0f, 2.0f, 2.0f, 24.0f, { "head", "beak", "red_thing" } };
const AgeableParams kAgeOcelot   { true, 10.0f, 4.0f, 2.0f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgeHorse    { true, 16.2f, 1.36f, 2.7272f, 2.0f, 20.0f, { "head_parts" } };
const AgeableParams kAgeFox      { true, 8.0f, 3.35f, 2.0f, 2.0f, 24.0f, { "head" } };          // FoxModel
const AgeableParams kAgeBee      { false, 24.0f, 0.0f, 2.0f, 2.0f, 24.0f, {} };                  // BeeModel, headParts empty
const AgeableParams kAgeGoat     { true, 19.0f, 1.0f, 2.5f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgePolarBear{ true, 16.0f, 4.0f, 2.25f, 2.0f, 24.0f, { "head" } };
const AgeableParams kAgePanda    { true, 23.0f, 4.8f, 2.7f, 3.0f, 49.0f, { "head" } };
const AgeableParams kAgeTurtle   { true, 120.0f, 0.0f, 9.0f, 6.0f, 120.0f, { "head" } };

TopPre LegacyAgeablePre(const AgeableParams& p) {
    return [p](const std::string& top) {
        using A = EntityAffine;
        if (std::find(p.headParts.begin(), p.headParts.end(), top) != p.headParts.end()) {
            A m = p.scaleHead ? A::S(1.5 / p.headScale) : A();
            return m * A::T(0.0, p.yHeadOffset / 16.0, p.zHeadOffset / 16.0);
        }
        return A::S(1.0 / p.bodyScale) * A::T(0.0, p.bodyYOffset / 16.0, 0.0);
    };
}
TopPre LegacyBabyPre(const EntityModel&) { return LegacyAgeablePre(kAgeHumanoid); }
// 旧版 AgeableHierarchicalModel.renderToBuffer (及 CamelModel): 幼体整体 scale(s) 后 translate(0, yOffset/16, 0)
TopPre LegacyWholeBabyPre(float scale, float yOffset) {
    return [scale, yOffset](const std::string&) {
        return EntityAffine::S(scale) * EntityAffine::T(0.0, yOffset / 16.0, 0.0);
    };
}

void EmitTextured(Ctx& ctx, OutObject& obj, EntityModel& model, const EntityAffine& base,
                  const TopPre& pre, const std::string& matName, const std::string& tex) {
    if (tex.empty()) return;
    int mat = ctx.Material(matName, tex);
    model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
}

void ApplyHeadLook(EntityModel& model, const Placement& pl, const char* headPart = "head") {
    // netHeadYaw relative to body; NoAI entities keep head yRot = 0
    model.SetRot(headPart, static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
}

void AddPos(EntityModel& model, const char* part, float dx, float dy, float dz) {
    int i = model.Find(part); if (i < 0) return;
    float* q = model.Pose(i); q[0] += dx; q[1] += dy; q[2] += dz;
}
void SetXRot(EntityModel& model, const char* part, float x) {
    int i = model.Find(part); if (i < 0) return;
    model.Pose(i)[3] = x;
}
float HeadPitch(const Placement& pl) { return static_cast<float>(pl.pitch * kPi / 180.0); }

// 26.x baby layer when it exists, otherwise the adult layer of this family.
std::string MobLayer(const Ctx& ctx, const std::string& kind, bool baby, const std::string& sub = "#main") {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    if (ctx.modern && baby) {
        const std::string b = "minecraft:" + kind + "_baby" + sub;
        if (EntityModel::Exists(fam, b)) return b;
    }
    return "minecraft:" + kind + sub;
}
std::string PickTex(std::initializer_list<const char*> paths) {
    std::string fallback;
    for (const char* p : paths) {
        if (!p || !*p) continue;
        if (fallback.empty()) fallback = p;
        if (TextureExists("minecraft", p)) return p;
    }
    return fallback;
}
// Anger: legacy AngerTime (ticks left), 1.21.5+ anger_end_time (game tick; -1 = calm).
bool IsAngry(const NbtTagPtr& e) {
    return NumOr(e, "Angry", 0) != 0 || NumOr(e, "AngerTime", 0) > 0 || NumOr(e, "anger_end_time", -1) > 0;
}

bool NbtFlag(const NbtTagPtr& e, const char* name, bool def) {
    auto t = getChildByName(e, name);
    if (!t) return def;
    double v;
    return Num(t, v) ? v != 0.0 : def;
}

// DyeColor.textureDiffuseColor (1.21) — sheep wool / wolf collar
int DyeRgb(int id) {
    static const int k[] = {
        0xF9FFFF, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
        0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21 };
    return k[std::clamp(id, 0, 15)];
}
// 羊毛色 (1.21 Sheep.createSheepColor / 26.x ColorLerper.Type.SHEEP): 白是常量 0xE6E6E6, 其余 floor(textureDiffuse * 0.75)。
// 非白的 textureDiffuse 与上面这张表一致。
int SheepWoolRgb(int id) {
    id = std::clamp(id, 0, 15);
    if (id == 0) return 0xE6E6E6;
    const int c = DyeRgb(id);
    auto ch = [](int v) { return (v * 3) / 4; };  // floor(v * 0.75), v ∈ [0,255]
    return (ch((c >> 16) & 255) << 16) | (ch((c >> 8) & 255) << 8) | ch(c & 255);
}

// Wolf.getTailAngle: 愤怒 1.5393804, 驯服 (0.55 - 0.4 * 损失血量比) * π, 野生 π/5
float WolfTailAngle(const NbtTagPtr& e, bool tame, bool angry) {
    if (angry) return 1.5393804f;
    if (!tame) return static_cast<float>(kPi / 5);
    double hp = NumOr(e, "Health", 20.0);
    double maxHp = 20.0;
    for (const char* key : { "attributes", "Attributes" }) {
        auto list = getChildByName(e, key);
        if (!list) continue;
        for (auto& a : list->children) {
            std::string id = Str(a, "id"); if (id.empty()) id = Str(a, "Name");
            if (id.find("max_health") != std::string::npos) maxHp = NumOr(a, "base", NumOr(a, "Base", maxHp));
        }
    }
    double lost = maxHp > 0 ? std::clamp((maxHp - hp) / maxHp, 0.0, 1.0) : 0.0;
    return static_cast<float>((0.55 - lost * 0.4) * kPi);
}

// WolfModel.prepareMobModel (旧版, 绝对坐标) / WolfModel.setSittingPose (26.x, 相对偏移 * ageScale)
void PoseWolf(EntityModel& model, bool sitting, bool modernBaby, float tailAngle) {
    model.SetRot("tail", tailAngle, 0.0f, 0.0f);
    if (!sitting) return;  // standing: baked pose (body/upper_body xRot = π/2)
    auto add = [&](const char* p, float dx, float dy, float dz) {
        int i = model.Find(p); if (i < 0) return;
        float* q = model.Pose(i); q[0] += dx; q[1] += dy; q[2] += dz;
    };
    auto rotX = [&](const char* p, float r) { int i = model.Find(p); if (i >= 0) model.Pose(i)[3] = r; };
    const float s = modernBaby ? 0.5f : 1.0f;  // LivingEntity.getAgeScale
    add("body", 0, 4.0f * s, -2.0f * s);
    rotX("body", static_cast<float>(kPi / 4) - (modernBaby ? 1.0f : 0.0f));  // BabyWolfModel: body.xRot--
    add("tail", 0, 9.0f * s, -2.0f * s);
    add("right_hind_leg", 0, 6.7f * s, -5.0f * s);
    add("left_hind_leg", 0, 6.7f * s, -5.0f * s);
    rotX("right_hind_leg", static_cast<float>(kPi * 3.0 / 2.0));
    rotX("left_hind_leg", static_cast<float>(kPi * 3.0 / 2.0));
    add("right_front_leg", 0.01f * s, 1.0f * s, 0);
    add("left_front_leg", -0.01f * s, 1.0f * s, 0);
    rotX("right_front_leg", 5.811947f);
    rotX("left_front_leg", 5.811947f);
    if (!modernBaby) {  // AdultWolfModel / 旧版 upperBody (-1, 16, -3)
        add("upper_body", 0, 2.0f, 0);
        rotX("upper_body", static_cast<float>(kPi * 2.0 / 5.0));
    }
}

// OcelotModel/CatModel (旧版) 与 AdultFelineModel / BabyFelineModel (26.x)
// 烘焙姿态 = prepareMobModel 基准坐标; 站立时 setupAnim 设置 tail2.xRot = 1.7278761
void PoseCat(EntityModel& model, bool sitting, bool modernBaby) {
    auto add = [&](const char* p, float dy, float dz) {
        int i = model.Find(p); if (i < 0) return;
        float* q = model.Pose(i); q[1] += dy; q[2] += dz;
    };
    auto rotX = [&](const char* p, float r, bool relative = false) {
        int i = model.Find(p); if (i < 0) return;
        model.Pose(i)[3] = relative ? model.Pose(i)[3] + r : r;
    };
    if (modernBaby) {
        if (!sitting) { rotX("tail2", 1.7278761f); return; }
        rotX("body", -0.43633232f, true);
        add("body", 1.0f, 0.0f);
        add("head", 0.0f, 0.75f);
        rotX("tail1", 0.5454154f, true);
        add("tail1", 4.0f, -0.9f);
        add("left_hind_leg", 0.0f, -0.9f);
        add("right_hind_leg", 0.0f, -0.9f);
        return;
    }
    if (!sitting) { rotX("tail2", 1.7278761f); return; }
    rotX("body", static_cast<float>(kPi / 4));
    add("body", -4.0f, 5.0f);
    add("head", -3.3f, 1.0f);
    add("tail1", 8.0f, -2.0f);
    add("tail2", 2.0f, -0.8f);
    rotX("tail1", 1.7278761f);
    rotX("tail2", 2.670354f);
    for (const char* leg : { "left_front_leg", "right_front_leg" }) { add(leg, 2.0f, -2.0f); rotX(leg, static_cast<float>(-kPi / 20)); }
    for (const char* leg : { "left_hind_leg", "right_hind_leg" }) { add(leg, 3.0f, -4.0f); rotX(leg, static_cast<float>(-kPi / 2)); }
}

void PoseZombieArms(EntityModel& model) {
    const float armX = static_cast<float>(-kPi / 2.25);
    model.SetRot("right_arm", armX, -0.1f, 0.05f);
    model.SetRot("left_arm", armX, 0.1f, -0.05f);
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
}

void PoseSkeletonIdle(EntityModel& model) {
    // HumanoidModel idle bob mean: arms zRot ±0.05, legs ±0.005
    model.SetRot("right_arm", 0.0f, 0.0f, 0.05f);
    model.SetRot("left_arm", 0.0f, 0.0f, -0.05f);
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
}

// Humanoid mob + optional clothing overlay + armor + held items.
// legacyScale is LivingEntityRenderer.scale (husk 1.0625, wither skeleton 1.2). 26.x bakes that
// into the layer root, so it is applied only for the legacy family.
// outerLayer is a suffix such as "#outer" (empty = none). The overlay is emitted only when that
// layer exists for this family and outerTex is present; its pose is copied from the main model.
void RenderHumanoidMob(Ctx& ctx, const NbtTagPtr& e, const std::string& kind, const std::string& texPath,
                       double eyeHeight, bool zombieArms, double legacyScale = 1.0,
                       const std::string& outerLayer = {}, const std::string& outerTex = {}) {
    const std::string objId = "minecraft:" + kind;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    // 26.x baby layers exist for zombie / husk / drowned only. Other kinds keep the adult layer.
    bool modernBaby = false;
    std::string layer = "minecraft:" + kind + "#main";
    if (ctx.modern && baby) {
        const std::string babyLayer = "minecraft:" + kind + "_baby#main";
        if (EntityModel::Exists(fam, babyLayer)) {
            layer = babyLayer;
            modernBaby = true;
        }
    }
    EntityModel model(fam, layer);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    if (zombieArms) PoseZombieArms(model); else PoseSkeletonIdle(model);
    double eye = eyeHeight * (baby ? 0.5 : 1.0);
    const double rs = ctx.modern ? 1.0 : legacyScale;
    EntityAffine base = LivingBase(pl, eye, rs);
    TopPre pre;
    if (!ctx.modern && baby) pre = LegacyBabyPre(model);
    OutObject& obj = ctx.NewObject(objId);
    std::string tex = ctx.Texture("minecraft", texPath);
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, tex);
    if (!outerLayer.empty()) {
        const std::string oid = "minecraft:" + kind + (modernBaby ? "_baby" : "") + outerLayer;
        if (EntityModel::Exists(fam, oid)) {
            std::string otex = ctx.Texture("minecraft", outerTex);
            if (!otex.empty()) {
                EntityModel outer(fam, oid);
                outer.CopyPoseFrom(model,
                                   { "head", "hat", "body", "right_arm", "left_arm", "right_leg", "left_leg" }, true);
                EmitTextured(ctx, obj, outer, base, pre, "minecraft:" + outerTex, otex);
            }
        }
    }
    const bool young = ctx.modern ? modernBaby : baby;
    Equipment eq = ReadEquipment(e);
    RenderArmor(ctx, obj, "minecraft:" + kind, model, eq, base, pre, young, false);
    RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && young, ctx.modern && young);
    RenderHeadItem(ctx, obj, model, eq, base, young, false, 0.0);
}

void RenderCreeper(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:creeper#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 1.7, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:creeper");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:entity/creeper/creeper",
                 ctx.Texture("minecraft", "entity/creeper/creeper"));
}

// 1.21.5+ climate variant. Missing or unknown -> temperate (the 1.21.1 default body).
std::string ClimateVariant(const NbtTagPtr& e) {
    std::string v = Str(e, "variant");
    if (v.empty()) v = Str(e, "Variant");
    if (v.empty()) return "temperate";
    v = SplitId(v).second;
    const size_t slash = v.rfind('/');
    if (slash != std::string::npos) v = v.substr(slash + 1);
    if (v.size() > 5 && v.compare(v.size() - 5, 5, "_baby") == 0) v = v.substr(0, v.size() - 5);
    for (const char* p : { "pig_", "cow_", "chicken_" }) {
        const size_t n = std::char_traits<char>::length(p);
        if (v.size() > n && v.compare(0, n, p) == 0) { v = v.substr(n); break; }
    }
    if (v == "warm" || v == "cold" || v == "temperate") return v;
    return "temperate";
}
// entity/<dir>/<base>_<variant>[_baby] when that png exists, else the 1.21.1 name
// (entity/pig/pig, entity/cow/cow, entity/chicken). Baby sheets are not swapped onto the adult mesh.
std::string FarmTexture(const std::string& dir, const std::string& base, const std::string& variant,
                        bool baby, const std::string& legacy) {
    const std::string adult = "entity/" + dir + "/" + base + "_" + variant;
    if (baby) {
        const std::string b = adult + "_baby";
        if (TextureExists("minecraft", b)) return b;
        return legacy;
    }
    if (TextureExists("minecraft", adult)) return adult;
    return legacy;
}
// 26.x: cold_pig / cold_chicken (adult only; babies reuse pig_baby / chicken_baby),
// warm_cow / cold_cow and their baby layers. Legacy is always the single adult layer.
std::string FarmLayer(const Ctx& ctx, const std::string& kind, const std::string& variant, bool baby) {
    if (ctx.modern && variant != "temperate") {
        const std::string spec = "minecraft:" + variant + "_" + kind + (baby ? "_baby" : "") + "#main";
        if (EntityModel::Exists("modern", spec)) return spec;
    }
    return MobLayer(ctx, kind, baby);
}

void RenderQuadruped(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& layerId,
                     const std::string& texPath, double eye, const AgeableParams& age, double rendererScale = 1.0) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    // layerId is already the baby layer when this family has one.
    EntityModel model(fam, layerId);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    // 渲染器不对幼体额外缩放; 旧版幼体缩放全部来自 AgeableListModel.renderToBuffer
    EntityAffine base = LivingBase(pl, eye * (baby ? 0.5 : 1.0), rendererScale);
    TopPre pre;
    if (!ctx.modern && baby) pre = LegacyAgeablePre(age);
    OutObject& obj = ctx.NewObject(id);
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

void RenderClimateAnimal(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& kind,
                         const std::string& legacyTex, double eye, const AgeableParams& age) {
    const bool baby = IsBaby(e);
    const std::string variant = ClimateVariant(e);
    const std::string layer = FarmLayer(ctx, kind, variant, baby);
    const std::string tex = FarmTexture(kind, kind, variant, ctx.modern && baby, legacyTex);
    RenderQuadruped(ctx, e, id, layer, tex, eye, age);
}
void RenderClimateCow(Ctx& ctx, const NbtTagPtr& e) {
    RenderClimateAnimal(ctx, e, "minecraft:cow", "cow", "entity/cow/cow", 1.4, kAgeCow);
}
void RenderClimateChicken(Ctx& ctx, const NbtTagPtr& e) {
    // 1.21.1 texture is textures/entity/chicken.png, not entity/chicken/chicken.png.
    RenderClimateAnimal(ctx, e, "minecraft:chicken", "chicken", "entity/chicken", 0.7, kAgeChicken);
}
// MushroomCowRenderer: temperate cow mesh (MOOSHROOM / MOOSHROOM_BABY == cow / BabyCowModel).
// 26.x textures entity/cow/mooshroom_<red|brown>[_baby]. 1.21.1 and 26.x both store Type "red"/"brown";
// older worlds fall back to entity/cow/red_mooshroom and brown_mooshroom.
void RenderMooshroom(Ctx& ctx, const NbtTagPtr& e) {
    const bool baby = IsBaby(e);
    std::string color = SplitId(Str(e, "Type")).second;
    if (color.empty()) color = SplitId(Str(e, "type")).second;
    if (color != "red" && color != "brown") color = "red";
    std::string tex = "entity/cow/" + color + "_mooshroom";
    const std::string modern = "entity/cow/mooshroom_" + color;
    if (ctx.modern && baby && TextureExists("minecraft", modern + "_baby")) tex = modern + "_baby";
    else if (TextureExists("minecraft", modern)) tex = modern;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const std::string layer = FarmLayer(ctx, "cow", "temperate", baby);
    EntityModel model(fam, layer);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 1.4 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre;
    if (!ctx.modern && baby) pre = LegacyAgeablePre(kAgeCow);
    OutObject& obj = ctx.NewObject("minecraft:mooshroom");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + tex, ctx.Texture("minecraft", tex));
    // MushroomCowMushroomLayer: 成年, 实体 pose 上两朵背菇 + head.translateAndRotate 上的头菇。
    // 1.21 与 26.x 都画植株 RED_MUSHROOM / BROWN_MUSHROOM (block/red_mushroom 十字), 不是蘑菇方块。
    // 方块模型原点在角上; 末尾 T(-0.5) 即 renderSingleBlock 前的 translate(-0.5)。
    if (baby) return;
    using A = EntityAffine;
    const std::string shroom = (color == "brown") ? "block/brown_mushroom" : "block/red_mushroom";
    const double d = kPi / 180.0;
    const A flip = A::S(-1.0, -1.0, 1.0);
    const A center = A::T(-0.5, -0.5, -0.5);
    auto put = [&](const A& m) {
        ModelData mesh = ProcessModelJson("minecraft", shroom, 0, 0, false);
        if (mesh.faces.empty()) return;
        ItemRender::Transform(mesh, m);
        AppendMesh(ctx, obj, mesh);
    };
    put(base * A::T(0.2, -0.35, 0.5) * A::RY(-48.0 * d) * flip * center);
    put(base * A::T(0.2, -0.35, 0.5) * A::RY(42.0 * d) * A::T(0.1, 0.0, -0.6) * A::RY(-48.0 * d) * flip * center);
    int head = model.Find("head");
    if (head >= 0)
        put(model.PartTransform(head, base) * A::T(0.0, -0.7, -0.2) * A::RY(-78.0 * d) * flip * center);
}

// 纯文本 / "name" / {"text":"name"} (无 extra)。兔子 Toast 与羊 jeb_ 共用。
bool PlainCustomName(const std::string& raw, const char* name) {
    if (raw.empty() || !name || !*name) return false;
    if (raw == name) return true;
    const size_t n = std::char_traits<char>::length(name);
    if (raw.size() == n + 2 && raw.front() == '"' && raw.back() == '"' && raw.compare(1, n, name) == 0) return true;
    const auto k = raw.find("\"text\"");
    if (k == std::string::npos) return false;
    const auto c = raw.find(':', k);
    if (c == std::string::npos) return false;
    const auto q1 = raw.find('"', c + 1);
    if (q1 == std::string::npos) return false;
    const auto q2 = raw.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    return raw.compare(q1 + 1, q2 - q1 - 1, name) == 0 && raw.find("\"extra\"") == std::string::npos;
}

void EmitDyedLayer(Ctx& ctx, OutObject& obj, EntityModel& model, const EntityAffine& base, const TopPre& pre,
                   const std::string& texPath, uint32_t rgb) {
    std::string tex = ctx.Texture("minecraft", texPath);
    if (tex.empty()) return;
    TintResult t; t.kind = TintKind::Fixed; t.color = rgb & 0xFFFFFFu;
    int mat = ctx.Material("minecraft:" + texPath + TintSuffix(t), ctx.TintedTexture(tex, t.color));
    model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
}

void RenderSheep(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    bool sheared = NumOr(e, "Sheared", 0) != 0;
    int color = static_cast<int>(NumOr(e, "Color", NumOr(e, "color", 0)));
    // 26.x SheepRenderer: 幼体身体是 sheep_baby.png
    std::string bodyTex = "entity/sheep/sheep";
    if (ctx.modern && baby && TextureExists("minecraft", "entity/sheep/sheep_baby"))
        bodyTex = "entity/sheep/sheep_baby";
    std::string lid = std::string("minecraft:sheep") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 1.3 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeSheep) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:sheep");
    // jeb_: 26.x getWoolColor = ColorLerper.getLerpedColor(SHEEP, ageInTicks)。
    // 导出没有渲染 tick, 取 ageInTicks = 0: floor(0)/25 = 0, 插值系数 0, 即 DyeColor[0]=白 的 0xE6E6E6。
    // 1.21 的 jeb_ 还加了实体 id, 静态导出不稳定, 仍用羊毛自身颜色。
    const bool jeb = ctx.modern && PlainCustomName(Str(e, "CustomName"), "jeb_");
    const int rgb = jeb ? 0xE6E6E6 : SheepWoolRgb(color);
    const bool white = std::clamp(color, 0, 15) == 0;
    // 26.x 的 sheep#wool_undercoat 和 sheep_baby#wool 与身体几何/UV 完全相同 (共面),
    // 整模再画一遍在 Blender 里是黑剪影, 所以做成身体材质的染色叠加层 (同项圈)。
    // SheepWoolUndercoatLayer: 未隐形、非幼体、且 (jeb_ 或颜色不是白)。剪过毛也画。
    const std::string bodyFile = ctx.Texture("minecraft", bodyTex);
    std::string coplanar;
    const char* tag = nullptr;
    if (ctx.modern && !baby && (jeb || !white)) { coplanar = "entity/sheep/sheep_wool_undercoat"; tag = "undercoat"; }
    else if (ctx.modern && baby && !sheared) { coplanar = "entity/sheep/sheep_wool_baby"; tag = "wool"; }
    if (!coplanar.empty() && !bodyFile.empty()) {
        int mat = CollaredMaterial(ctx, "minecraft:" + bodyTex, bodyFile, "minecraft", coplanar, rgb, tag);
        model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
    } else {
        EmitTextured(ctx, obj, model, base, pre, "minecraft:" + bodyTex, bodyFile);
    }
    if (!sheared && !(ctx.modern && baby)) {
        std::string furId = std::string("minecraft:sheep") + (ctx.modern ? "#wool" : "#fur");
        std::string woolTex = ctx.modern ? "entity/sheep/sheep_wool" : "entity/sheep/sheep_fur";
        EntityModel fur(fam, furId);
        if (fur.Valid()) {
            fur.CopyPoseFrom(model, { "head", "body", "right_front_leg", "left_front_leg", "right_hind_leg", "left_hind_leg" }, true);
            EmitDyedLayer(ctx, obj, fur, base, pre, woolTex, static_cast<uint32_t>(rgb));
        }
    }
}

void RenderWolf(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = std::string("minecraft:wolf") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    bool angry = IsAngry(e);
    bool tame = getChildByName(e, "Owner") != nullptr || getChildByName(e, "OwnerUUID") != nullptr;
    PoseWolf(model, IsSitting(e), ctx.modern && baby, WolfTailAngle(e, tame, angry));
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 0.85 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeDefault) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:wolf");
    // 1.20.5+: variant string; older: always wolf.png / wolf_tame / wolf_angry
    std::string variant = Str(e, "variant");
    if (variant.empty()) variant = Str(e, "Variant");
    std::string texPath = "entity/wolf/wolf";
    if (!variant.empty() && variant.find(':') != std::string::npos) {
        auto [ns, v] = SplitId(variant);
        texPath = "entity/wolf/" + v;
        if (angry) texPath += "_angry";
        else if (tame) texPath += "_tame";
        // if missing fall back below
        if (!TextureExists(ns, texPath)) texPath = "entity/wolf/wolf";
    } else {
        if (angry) texPath = "entity/wolf/wolf_angry";
        else if (tame) texPath = "entity/wolf/wolf_tame";
    }
    std::string baseTex = ctx.Texture("minecraft", texPath);
    if (tame) {
        int collar = static_cast<int>(NumOr(e, "CollarColor", 14));  // default red
        int mat = CollaredMaterial(ctx, "minecraft:" + texPath, baseTex,
                                   "minecraft", "entity/wolf/wolf_collar", DyeRgb(collar));
        if (mat >= 0) model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
    } else {
        EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, baseTex);
    }
}

void RenderCat(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool baby = IsBaby(e);
    std::string lid = std::string("minecraft:cat") + (ctx.modern && baby ? "_baby" : "") + "#main";
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseCat(model, IsSitting(e), ctx.modern && baby);
    ApplyHeadLook(model, pl);
    // CatRenderer.scale 0.8 (旧版, 幼体同样 0.8); 26.x 烘焙进 root
    double rs = ctx.modern ? 1.0 : 0.8;
    EntityAffine base = LivingBase(pl, 0.7 * (baby ? 0.5 : 1.0), rs);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeOcelot) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:cat");
    static const char* kCatType[] = { "tabby", "black", "red", "siamese", "british_shorthair", "calico",
                                      "persian", "ragdoll", "white", "jellie", "all_black" };
    std::string variant = Str(e, "variant");
    if (variant.empty()) {
        int t = static_cast<int>(NumOr(e, "CatType", 0));
        variant = std::string("minecraft:") + kCatType[std::clamp(t, 0, 10)];
    }
    auto [ns, v] = SplitId(variant);
    if (v.rfind("cat/", 0) == 0) v = v.substr(4);
    // 1.21: entity/cat/<variant>. 26.x cat_variant.asset_id: entity/cat/cat_<variant>[_baby]。
    std::string texPath = "entity/cat/" + v;
    if (ctx.modern) {
        const std::string prefixed = "entity/cat/cat_" + v + (baby ? "_baby" : "");
        if (TextureExists("minecraft", prefixed)) texPath = prefixed;
        else if (baby && TextureExists("minecraft", texPath + "_baby")) texPath += "_baby";
    }
    std::string baseTex = ctx.Texture(ns, texPath);
    bool tame = getChildByName(e, "Owner") || getChildByName(e, "OwnerUUID");
    std::string collarPath = "entity/cat/cat_collar";
    if (ctx.modern && baby && TextureExists("minecraft", "entity/cat/cat_collar_baby"))
        collarPath = "entity/cat/cat_collar_baby";
    if (tame) {
        int collar = static_cast<int>(NumOr(e, "CollarColor", 14));
        int mat = CollaredMaterial(ctx, "minecraft:" + texPath, baseTex,
                                   "minecraft", collarPath, DyeRgb(collar));
        if (mat >= 0) model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
    } else {
        EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, baseTex);
    }
}

// equipment.saddle (1.21.5+), SaddleItem compound (horse 1.20.5-1.21.4), or Saddle:1b (pig/strider steering, older horses).
ItemStackInfo SaddleStack(const NbtTagPtr& e) {
    if (auto eq = getChildByName(e, "equipment")) {
        ItemStackInfo s = ItemRender::ParseItemStack(getChildByName(eq, "saddle"));
        if (!s.empty()) return s;
    }
    ItemStackInfo s = ItemRender::ParseItemStack(getChildByName(e, "SaddleItem"));
    if (!s.empty()) return s;
    if (NbtFlag(e, "Saddle", false)) {
        ItemStackInfo syn;
        syn.id = "minecraft:saddle";
        syn.count = 1;
        return syn;
    }
    return {};
}
// equipment.body (1.21.5+), body_armor_item (1.20.5-1.21.4), ArmorItem (older).
ItemStackInfo BodyArmorStack(const NbtTagPtr& e) {
    if (auto eq = getChildByName(e, "equipment")) {
        ItemStackInfo s = ItemRender::ParseItemStack(getChildByName(eq, "body"));
        if (!s.empty()) return s;
    }
    ItemStackInfo s = ItemRender::ParseItemStack(getChildByName(e, "body_armor_item"));
    if (!s.empty()) return s;
    return ItemRender::ParseItemStack(getChildByName(e, "ArmorItem"));
}
bool HasPassengers(const NbtTagPtr& e) {
    for (const char* key : { "Passengers", "passengers" }) {
        auto p = getChildByName(e, key);
        if (p && p->type == TagType::LIST && !p->children.empty()) return true;
    }
    return false;
}
// 26.x equipment/<asset>.json layers[type] -> entity/equipment/<type>/<texture>. Empty if the JSON has no such type.
std::vector<ArmorTexLayer> EquipmentTexLayers(const ItemStackInfo& item, const std::string& assetId, const std::string& type) {
    std::vector<ArmorTexLayer> layers;
    auto [ns, path] = SplitId(assetId);
    nlohmann::json def = ItemRender::AssetJson(ns, "equipment/" + path + ".json");
    if (!def.is_object() || !def.contains("layers") || !def["layers"].is_object() || !def["layers"].contains(type) ||
        !def["layers"][type].is_array())
        return layers;
    for (const auto& l : def["layers"][type]) {
        if (!l.contains("texture") || !l["texture"].is_string()) continue;
        auto [tns, tpath] = SplitId(l["texture"].get<std::string>());
        int color = -1;
        if (l.contains("dyeable") && l["dyeable"].is_object()) {
            if (item.dyeColor >= 0) color = item.dyeColor;
            else if (l["dyeable"].contains("color_when_undyed") && l["dyeable"]["color_when_undyed"].is_number())
                color = static_cast<int>(l["dyeable"]["color_when_undyed"].get<int64_t>() & 0xFFFFFF);
            else continue;
        }
        layers.push_back({ tns, "entity/equipment/" + type + "/" + tpath, color });
    }
    return layers;
}
// iron_horse_armor -> iron, golden_horse_armor -> gold. Empty if this is not a known horse-armor item.
std::string HorseArmorMaterial(const ItemStackInfo& item) {
    const std::string p = SplitId(item.id).second;
    const std::string suf = "_horse_armor";
    if (p.size() <= suf.size() || p.compare(p.size() - suf.size(), suf.size(), suf) != 0) return {};
    std::string mat = p.substr(0, p.size() - suf.size());
    if (mat == "golden") mat = "gold";
    static const char* kKnown[] = { "iron", "gold", "diamond", "leather", "copper", "netherite" };
    for (const char* k : kKnown)
        if (mat == k) return mat;
    return {};
}
bool IsEquineSaddlePart(const std::string& name) {
    return name == "saddle" || name == "left_saddle_mouth" || name == "right_saddle_mouth" || name == "left_saddle_line" ||
           name == "right_saddle_line" || name == "head_saddle" || name == "mouth_saddle_wrap";
}
void SetPartZ(EntityModel& model, const char* part, float z) {
    int i = model.Find(part);
    if (i >= 0) model.Pose(i)[2] = z;
}

// HorseModel.prepareMobModel + setupAnim at rest (limbSwing 0, ageInTicks 0, stand/eat 0).
// Legacy bake stores front-leg z=-12; prepareMobModel moves them to -10. setupAnim then forces body.y=11
// (already baked, including over the baby 10.8). head_parts.xRot = pi/6 + pitch.
// 26.x adult meshes already bake z=-10 and pi/6. BabyHorseModel bakes 0.6109 and tail -0.7418, then
// setupAnim overwrites head to pi/6+pitch and tail to -pi/2+pi/6. BabyDonkeyModel forces state.xRot=-30
// after super, so the rest head is 0 (entity pitch is ignored) and the empty tail parent is pi/6.
void PoseEquine(EntityModel& model, float pitch, bool legacy, bool baby, bool babyDonkey) {
    const float pi6 = static_cast<float>(kPi / 6.0);
    if (babyDonkey) {
        SetXRot(model, "tail", pi6);
        return;
    }
    model.SetRot("head_parts", pi6 + pitch, 0.0f, 0.0f);
    if (!legacy && baby) SetXRot(model, "tail", static_cast<float>(-kPi / 2.0) + pi6);
    if (legacy) {
        for (const char* p : { "left_front_leg", "right_front_leg", "left_front_baby_leg", "right_front_baby_leg" })
            SetPartZ(model, p, -10.0f);
    }
}
// Saddle parts (and reins) follow HorseModel.setupAnim. Baby legs exist only on the legacy mesh.
// Chests follow hasChest with no baby check (DonkeyRenderer). Modern baby chest parts are empty cubes.
void ApplyEquineVis(EntityModel& model, bool legacy, bool baby, bool saddled, bool ridden, bool chest) {
    for (const char* p : { "saddle", "left_saddle_mouth", "right_saddle_mouth", "head_saddle", "mouth_saddle_wrap" })
        model.SetVisible(p, saddled);
    const bool lines = saddled && ridden;
    model.SetVisible("left_saddle_line", lines);
    model.SetVisible("right_saddle_line", lines);
    const bool babyLegs = legacy && baby;
    for (const char* p : { "left_hind_baby_leg", "right_hind_baby_leg", "left_front_baby_leg", "right_front_baby_leg" })
        model.SetVisible(p, babyLegs);
    if (babyLegs) {
        for (const char* p : { "left_hind_leg", "right_hind_leg", "left_front_leg", "right_front_leg" })
            model.SetVisible(p, false);
    }
    model.SetVisible("left_chest", chest);
    model.SetVisible("right_chest", chest);
}

// HorseMarkingLayer. Markings.byId wraps with positiveModulo over the 5 ids.
// NONE draws nothing. 1.21.1 and 26.3 Horse.addAdditionalSaveData both write one
// packed int: Variant = color | markings<<8 (bits 8..15). A separate markings/Markings
// tag, if a world has one, wins: an int id, or a name
// (none / white / white_field|whitefield / white_dots|whitedots / black_dots|blackdots).
int HorseMarkingId(const NbtTagPtr& e) {
    auto separate = getChildByName(e, "markings");
    if (!separate) separate = getChildByName(e, "Markings");
    if (separate) {
        if (separate->type == TagType::STRING) {
            std::string s = SplitId(getStringTag(separate)).second;
            for (char& c : s)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (s == "none") return 0;
            if (s == "white") return 1;
            if (s == "white_field" || s == "whitefield") return 2;
            if (s == "white_dots" || s == "whitedots") return 3;
            if (s == "black_dots" || s == "blackdots") return 4;
        } else {
            double v;
            if (Num(separate, v)) return static_cast<int>(v);
        }
    }
    auto packed = getChildByName(e, "Variant");
    if (!packed) packed = getChildByName(e, "variant");
    double v;
    if (packed && Num(packed, v)) {
        const auto bits = static_cast<unsigned>(static_cast<int>(v));
        return static_cast<int>((bits & 0xFF00u) >> 8);
    }
    return 0;
}
// Texture stem for a marking id. Empty string is NONE (or a wrapped NONE).
const char* HorseMarkingStem(int id) {
    int m = id % 5;
    if (m < 0) m += 5;
    static const char* k[] = { "", "white", "whitefield", "whitedots", "blackdots" };
    return k[m];
}

enum class EquineKind { Horse, Donkey, Mule, Skeleton, Zombie };

void RenderEquine(Ctx& ctx, const NbtTagPtr& e, EquineKind kind) {
    struct Info {
        const char* id;
        double legacyScale;
        bool chest;
        bool undead;             // 1.21 UndeadHorseRenderer has no armor layer; 26.x uses undead_horse_armor
        const char* armorLayer;  // modern model id without #main; legacy horse armor is always horse_armor
        const char* saddleType;  // 26.x equipment layer type
        const char* tex;         // empty = horse variant texture
    };
    static const Info kInfo[] = {
        { "horse", 1.1, false, false, "horse_armor", "horse_saddle", "" },
        { "donkey", 0.87, true, false, "", "donkey_saddle", "entity/horse/donkey" },
        { "mule", 0.92, true, false, "", "mule_saddle", "entity/horse/mule" },
        { "skeleton_horse", 1.0, false, true, "undead_horse_armor", "skeleton_horse_saddle", "entity/horse/horse_skeleton" },
        { "zombie_horse", 1.0, false, true, "undead_horse_armor", "zombie_horse_saddle", "entity/horse/horse_zombie" },
    };
    const Info& info = kInfo[static_cast<int>(kind)];
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool babyDonkey = ctx.modern && baby && info.chest;
    EntityModel model(fam, MobLayer(ctx, info.id, baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float pitch = HeadPitch(pl);
    const ItemStackInfo saddleItem = SaddleStack(e);
    const bool saddled = !saddleItem.empty();
    const bool ridden = HasPassengers(e);
    const bool chest = info.chest && (NbtFlag(e, "ChestedHorse", false) || NbtFlag(e, "Chested", false));
    PoseEquine(model, pitch, !ctx.modern, baby, babyDonkey);
    ApplyEquineVis(model, !ctx.modern, baby, saddled, ridden, chest);
    // Legacy renderer scales (AbstractHorseRenderer 1.1, donkey 0.87, mule 0.92, undead 1). 26.x bakes them into the adult root.
    const double rs = ctx.modern ? 1.0 : info.legacyScale;
    EntityAffine base = LivingBase(pl, 2.3 * (baby ? 0.5 : 1.0), rs);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeHorse) : nullptr;
    OutObject& obj = ctx.NewObject(std::string("minecraft:") + info.id);
    std::string texPath;
    if (info.tex[0] == '\0') {
        static const char* kVar[] = { "white", "creamy", "chestnut", "brown", "black", "gray", "darkbrown" };
        std::string vStr = Str(e, "variant");
        if (!vStr.empty()) {
            std::string v = SplitId(vStr).second;
            if (v.size() > 5 && v.compare(v.size() - 5, 5, "_baby") == 0) v = v.substr(0, v.size() - 5);
            if (v.rfind("horse_", 0) == 0) v = v.substr(6);
            texPath = (v.find('/') != std::string::npos) ? v : ("entity/horse/horse_" + v);
        } else {
            int variant = static_cast<int>(NumOr(e, "Variant", 0));
            texPath = std::string("entity/horse/horse_") + kVar[std::clamp(variant & 0xFF, 0, 6)];
        }
    } else {
        texPath = info.tex;
    }
    if (ctx.modern && baby) {
        const std::string babyTex = texPath + "_baby";
        if (TextureExists("minecraft", babyTex)) texPath = babyTex;
    }
    // HorseMarkingLayer: same mesh UVs, translucent marking sheet. A second full-mesh
    // emit of that mostly-transparent sheet draws as a black silhouette, so the sheet
    // is an overlay on the body material (same path as the wolf collar). Only the horse
    // has this layer. 26.x babies use horse_markings_<stem>_baby when that png exists.
    std::string bodyMat = "minecraft:" + texPath;
    const std::string baseTex = ctx.Texture("minecraft", texPath);
    if (kind == EquineKind::Horse && !baseTex.empty()) {
        const char* stem = HorseMarkingStem(HorseMarkingId(e));
        if (stem[0] != '\0') {
            std::string markPath = std::string("entity/horse/horse_markings_") + stem;
            if (ctx.modern && baby) {
                const std::string babyMark = markPath + "_baby";
                if (TextureExists("minecraft", babyMark)) markPath = babyMark;
            }
            const std::string markTex = ctx.Texture("minecraft", markPath);
            if (!markTex.empty()) {
                bodyMat += "~" + markPath.substr(markPath.rfind('/') + 1);
                OverlayLayerInfo info;
                info.name = "minecraft:" + markPath;
                info.texturePath = markTex;
                RegisterEntityOverlay(bodyMat, { info });
            }
        }
    }
    EmitTextured(ctx, obj, model, base, pre, bodyMat, baseTex);

    // Body armor. Legacy: HorseArmorLayer, only the plain horse, inflated horse_armor mesh (babies included).
    // 26.x: SimpleEquipmentLayer HORSE_BODY. babyModel is null, so babies wear nothing. Undead horses use the unscaled armor mesh.
    const ItemStackInfo armorItem = BodyArmorStack(e);
    const bool armorOk = !armorItem.empty() && !(info.undead && !ctx.modern) && !(ctx.modern && baby) && info.armorLayer[0] != '\0';
    if (armorOk) {
        const std::string matName = HorseArmorMaterial(armorItem);
        const std::string asset = !armorItem.equipAsset.empty() ? armorItem.equipAsset
                                  : (matName.empty() ? std::string() : "minecraft:" + matName);
        if (!asset.empty() && (ctx.modern || !matName.empty())) {
            std::vector<ArmorTexLayer> layers;
            if (!ctx.modern) {
                int color = -1;
                if (matName == "leather") color = armorItem.dyeColor >= 0 ? armorItem.dyeColor : 0xA06540;
                layers.push_back({ "minecraft", "entity/horse/armor/horse_armor_" + matName, color });
            } else {
                layers = EquipmentTexLayers(armorItem, asset, "horse_body");
                if (layers.empty()) {
                    const std::string texName = SplitId(asset).second;
                    if (texName == "leather" || matName == "leather") {
                        int color = armorItem.dyeColor >= 0 ? armorItem.dyeColor : 0xA06540;
                        layers.push_back({ "minecraft", "entity/equipment/horse_body/leather", color });
                        layers.push_back({ "minecraft", "entity/equipment/horse_body/leather_overlay", -1 });
                    } else if (!texName.empty()) {
                        layers.push_back({ "minecraft", "entity/equipment/horse_body/" + texName, -1 });
                    }
                }
            }
            const std::string armorId = ctx.modern ? (std::string("minecraft:") + info.armorLayer + "#main")
                                                   : "minecraft:horse_armor#main";
            EntityModel armor(fam, armorId);
            if (armor.Valid()) {
                PoseEquine(armor, pitch, !ctx.modern, baby, false);
                ApplyEquineVis(armor, !ctx.modern, baby, saddled, ridden, false);
                int mat = ArmorMaterial(ctx, layers);
                if (mat >= 0) armor.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
            }
        }
    }

    // 26.x saddle is a separate equipment layer (full body mesh + saddle parts, scale baked in).
    // Non-saddle polys are skipped: the saddle texture is mostly transparent and would redraw the horse black.
    // Legacy saddle parts live on the body model and are toggled above. No baby saddle layer exists.
    if (saddled && ctx.modern && !baby) {
        const std::string asset = saddleItem.equipAsset.empty() ? "minecraft:saddle" : saddleItem.equipAsset;
        std::vector<ArmorTexLayer> layers = EquipmentTexLayers(saddleItem, asset, info.saddleType);
        if (layers.empty())
            layers.push_back({ "minecraft", std::string("entity/equipment/") + info.saddleType + "/saddle", -1 });
        EntityModel saddle(fam, std::string("minecraft:") + info.id + "#saddle");
        if (saddle.Valid()) {
            PoseEquine(saddle, pitch, false, false, false);
            ApplyEquineVis(saddle, false, false, true, ridden, chest);
            int mat = ArmorMaterial(ctx, layers);
            if (mat >= 0) {
                saddle.Emit(obj.mesh, base, [&](int part) { return IsEquineSaddlePart(saddle.Name(part)) ? mat : -1; }, nullptr);
            }
        }
    }
}

void RenderPig(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const std::string variant = ClimateVariant(e);
    // Cold adults use ColdPigModel. Warm uses the temperate mesh. Babies of every climate use pig_baby
    // (PigRenderer's cold baby model is PIG_BABY). 1.21.1 has only pig#main.
    const std::string lid = FarmLayer(ctx, "pig", variant, baby);
    EntityModel model(fam, lid);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 0.9 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgePig) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:pig");
    const std::string texPath = FarmTexture("pig", "pig", variant, ctx.modern && baby, "entity/pig/pig");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    // SaddleLayer / PIG_SADDLE is PigModel with CubeDeformation(0.5) in both 1.21.1 and 26.x, so the
    // saddle sits off the body and stays a mesh. (Strider's saddle layer is a copy of the body and is
    // an overlay instead.) 26.x babyModel is null. The saddle mesh stays the temperate pig even on a
    // cold body, matching PigRenderer. Saddle pngs match the saddle model's UV size (64x32, 64x64).
    const ItemStackInfo saddleItem = SaddleStack(e);
    if (saddleItem.empty() || (ctx.modern && baby)) return;
    EntityModel saddle(fam, "minecraft:pig#saddle");
    if (!saddle.Valid()) return;
    saddle.CopyPoseFrom(model, { "head", "body", "right_hind_leg", "left_hind_leg", "right_front_leg", "left_front_leg" }, true);
    std::vector<ArmorTexLayer> layers;
    if (!ctx.modern) {
        layers.push_back({ "minecraft", "entity/pig/pig_saddle", -1 });
    } else {
        const std::string asset = saddleItem.equipAsset.empty() ? "minecraft:saddle" : saddleItem.equipAsset;
        layers = EquipmentTexLayers(saddleItem, asset, "pig_saddle");
        if (layers.empty()) layers.push_back({ "minecraft", "entity/equipment/pig_saddle/saddle", -1 });
    }
    int mat = ArmorMaterial(ctx, layers);
    if (mat >= 0) saddle.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
}

// StriderModel.setupAnim at rest. Cold/suffocating is not written by addAdditionalSaveData, so the normal texture is used.
// Legacy babies are the adult mesh with renderer scale 0.5 (getAgeScale). 26.x swaps in strider_baby and sets body.y=17.25.
void PoseStrider(EntityModel& model, float pitch, bool ridden, bool modernBaby) {
    int body = model.Find("body");
    if (body >= 0) model.Pose(body)[1] = modernBaby ? 17.25f : 2.0f;
    if (!ridden) model.SetRot("body", pitch, 0.0f, 0.0f);
    else model.SetRot("body", 0.0f, 0.0f, 0.0f);
}

void RenderStrider(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "strider", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const bool ridden = HasPassengers(e);
    PoseStrider(model, HeadPitch(pl), ridden, ctx.modern && baby);
    const double rs = (!ctx.modern && baby) ? 0.5 : 1.0;
    EntityAffine base = LivingBase(pl, 1.7 * (baby ? 0.5 : 1.0), rs);
    OutObject& obj = ctx.NewObject("minecraft:strider");
    std::string texPath = "entity/strider/strider";
    if (ctx.modern && baby && TextureExists("minecraft", "entity/strider/strider_baby"))
        texPath = "entity/strider/strider_baby";
    // STRIDER_SADDLE is the same LayerDefinition as STRIDER (both versions), so a second mesh is
    // exactly coplanar and renders as a black silhouette. The saddle sheet uses the body UVs
    // (1.21.1 strider_saddle.png and 26.x equipment/strider_saddle/saddle.png are both 64x128,
    // matching strider.png). 26.x SimpleEquipmentLayer babyModel is null.
    std::string bodyMat = "minecraft:" + texPath;
    const std::string baseTex = ctx.Texture("minecraft", texPath);
    const ItemStackInfo saddleItem = SaddleStack(e);
    if (!saddleItem.empty() && !(ctx.modern && baby)) {
        std::vector<ArmorTexLayer> layers;
        if (!ctx.modern) {
            layers.push_back({ "minecraft", "entity/strider/strider_saddle", -1 });
        } else {
            const std::string asset = saddleItem.equipAsset.empty() ? "minecraft:saddle" : saddleItem.equipAsset;
            layers = EquipmentTexLayers(saddleItem, asset, "strider_saddle");
            if (layers.empty()) layers.push_back({ "minecraft", "entity/equipment/strider_saddle/saddle", -1 });
        }
        bodyMat = BodyWithOverlays(ctx, bodyMat, "minecraft", texPath, baseTex, layers);
    }
    EmitTextured(ctx, obj, model, base, nullptr, bodyMat, baseTex);
}

// ---------- spider / cave spider ----------
// SpiderModel.setupAnim (limbSwing 0): static splayed legs. Legacy bake stores rot 0, so they must be set.
// 26.x bakes the same angles; setting them again matches. EyesLayer re-emits the posed model.
void PoseSpider(EntityModel& model) {
    const float q = static_cast<float>(kPi / 4.0);
    const float e = static_cast<float>(kPi / 8.0);
    struct Leg { const char* n; float y, z; };
    const Leg legs[] = {
        { "right_hind_leg", q, -q }, { "left_hind_leg", -q, q },
        { "right_middle_hind_leg", e, -0.58119464f }, { "left_middle_hind_leg", -e, 0.58119464f },
        { "right_middle_front_leg", -e, -0.58119464f }, { "left_middle_front_leg", e, 0.58119464f },
        { "right_front_leg", -q, -q }, { "left_front_leg", q, q },
    };
    for (const auto& l : legs) model.SetRot(l.n, 0.0f, l.y, l.z);
}

void RenderSpider(Ctx& ctx, const NbtTagPtr& e, bool cave) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const std::string kind = cave ? "cave_spider" : "spider";
    EntityModel model(fam, "minecraft:" + kind + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseSpider(model);
    ApplyHeadLook(model, pl);
    // CaveSpiderRenderer.scale 0.7 (legacy). 26.x bakes scale 0.7 and y=7.205 into the cave_spider root.
    const double rs = (cave && !ctx.modern) ? 0.7 : 1.0;
    EntityAffine base = LivingBase(pl, (cave ? 0.45 : 0.65), rs);
    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    const std::string texPath = cave ? "entity/spider/cave_spider" : "entity/spider/spider";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    // 1.21 eyes live at entity/spider_eyes; 26.x moved them under entity/spider/.
    const std::string eyes = ctx.modern
        ? PickTex({ "entity/spider/spider_eyes", "entity/spider_eyes" })
        : PickTex({ "entity/spider_eyes", "entity/spider/spider_eyes" });
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
}

// ---------- enderman ----------
// 1.21.1: carriedBlockState compound {Name, Properties}. 26.x: a string, optionally "id[prop=value,...]".
// Properties are not applied; the block is drawn as its item model.
std::string EndermanCarriedBlock(const NbtTagPtr& e) {
    auto c = getChildByName(e, "carriedBlockState");
    if (!c) c = getChildByName(e, "CarriedBlockState");
    if (!c) return {};
    std::string raw;
    if (c->type == TagType::STRING) raw = getStringTag(c);
    else if (c->type == TagType::COMPOUND) {
        raw = Str(c, "Name");
        if (raw.empty()) raw = Str(c, "name");
    } else {
        return {};
    }
    const size_t br = raw.find('[');
    if (br != std::string::npos) raw = raw.substr(0, br);
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '"')) raw.pop_back();
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '"')) raw.erase(raw.begin());
    if (raw.empty() || raw == "air" || raw == "minecraft:air") return {};
    if (raw.find(':') == std::string::npos) raw = "minecraft:" + raw;
    return raw;
}

void RenderEnderman(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:enderman#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // HumanoidModel idle at ageInTicks 0: arm zRot ±0.1, leg yRot/zRot ±0.005. EndermanModel then
    // halves xRot (still 0) and, while carrying, overwrites the arms. Creepy head dip is runtime; skipped.
    // Baked positions (head/hat y=-13, body y=-14, arms, legs) already match setupAnim's restores.
    const std::string carried = EndermanCarriedBlock(e);
    const bool carrying = !carried.empty();
    model.SetRot("head", HeadPitch(pl), 0.0f, 0.0f);
    if (carrying) {
        model.SetRot("right_arm", -0.5f, 0.0f, 0.05f);
        model.SetRot("left_arm", -0.5f, 0.0f, -0.05f);
    } else {
        model.SetRot("right_arm", 0.0f, 0.0f, 0.1f);
        model.SetRot("left_arm", 0.0f, 0.0f, -0.1f);
    }
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
    // Legacy hat is a sibling of head (copy the look). 26.x hat is a child at identity; copying would double.
    int head = model.Find("head"), hat = model.Find("hat");
    if (head >= 0 && hat >= 0 && model.Parent(hat) != head)
        for (int k = 3; k < 6; ++k) model.Pose(hat)[k] = model.Pose(head)[k];
    EntityAffine base = LivingBase(pl, 2.55, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:enderman");
    const std::string texPath = "entity/enderman/enderman";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    const std::string eyes = "entity/enderman/enderman_eyes";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
    // CarriedBlockLayer, on the entity pose stack (not the arm):
    // T(0, 0.6875, -0.75) * RX(20) * RY(45) * T(0.25, 0.1875, 0.25) * S(-0.5, -0.5, 0.5) * RY(90).
    // renderSingleBlock uses the block corner as origin. ItemDisplay::None still translates -0.5, so T(+0.5) cancels it.
    if (carrying) {
        using A = EntityAffine;
        ItemStackInfo block;
        block.id = carried;
        block.count = 1;
        const double d = kPi / 180.0;
        A m = base * A::T(0.0, 0.6875, -0.75) * A::RX(20.0 * d) * A::RY(45.0 * d) * A::T(0.25, 0.1875, 0.25) *
              A::S(-0.5, -0.5, 0.5) * A::RY(90.0 * d) * A::T(0.5, 0.5, 0.5);
        AppendItem(ctx, obj, block, ItemDisplay::None, false, m);
    }
}

// ---------- iron golem / snow golem ----------
void RenderIronGolem(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:iron_golem#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // Walk and attack/offer-flower swings are 0 at rest, so arms and legs stay at the baked pose.
    // Crackiness overlays and the flower item layer are skipped.
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 2.7, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:iron_golem");
    const std::string texPath = "entity/iron_golem/iron_golem";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// SnowGolemModel.setupAnim at yaw 0: arm z moves from the baked ±1 to 0. zRot stays ±1.
void PoseSnowGolem(EntityModel& model, float pitch) {
    model.SetRot("head", pitch, 0.0f, 0.0f);
    int ub = model.Find("upper_body");
    if (ub >= 0) model.Pose(ub)[4] = 0.0f;
    int la = model.Find("left_arm");
    if (la >= 0) { model.Pose(la)[0] = 5.0f; model.Pose(la)[2] = 0.0f; model.Pose(la)[4] = 0.0f; }
    int ra = model.Find("right_arm");
    if (ra >= 0) {
        model.Pose(ra)[0] = -5.0f; model.Pose(ra)[2] = 0.0f;
        model.Pose(ra)[4] = static_cast<float>(kPi);
    }
}

void RenderSnowGolem(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:snow_golem#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseSnowGolem(model, HeadPitch(pl));
    EntityAffine base = LivingBase(pl, 1.7, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:snow_golem");
    const std::string texPath = ctx.modern
        ? PickTex({ "entity/snow_golem/snow_golem", "entity/snow_golem" })
        : PickTex({ "entity/snow_golem", "entity/snow_golem/snow_golem" });
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    // Pumpkin defaults on (Pumpkin:1b). SnowGolemHeadLayer: head * T(0,-0.34375,0) * RY(180) * S(0.625,-0.625,-0.625).
    // ItemDisplay::Head already applies translate(-0.5). 1.21 renders the item; 26.x renders the block model the same way.
    if (NbtFlag(e, "Pumpkin", true)) {
        int head = model.Find("head");
        if (head >= 0) {
            ItemStackInfo pumpkin;
            pumpkin.id = "minecraft:carved_pumpkin";
            pumpkin.count = 1;
            A m = model.PartTransform(head, base) * A::T(0.0, -0.34375, 0.0) * A::RY(kPi) * A::S(0.625, -0.625, -0.625);
            AppendItem(ctx, obj, pumpkin, ItemDisplay::Head, false, m);
        }
    }
}

// ---------- llama / trader llama ----------
std::string LlamaVariantName(const NbtTagPtr& e) {
    static const char* k[] = { "creamy", "white", "brown", "gray" };
    std::string vs = Str(e, "variant");
    if (vs.empty()) vs = Str(e, "Variant");
    if (!vs.empty()) {
        auto v = SplitId(vs).second;
        if (v.rfind("llama_", 0) == 0) v = v.substr(6);
        if (v.size() > 5 && v.compare(v.size() - 5, 5, "_baby") == 0) v = v.substr(0, v.size() - 5);
        if (!v.empty()) return v;
    }
    int variant = static_cast<int>(NumOr(e, "Variant", 0));
    if (variant < 0 || variant > 3) variant = 0;
    return k[variant];
}

// LlamaModel.renderToBuffer young branch. scale-then-translate on the pose stack is S*T here.
TopPre LlamaBabyPre() {
    return [](const std::string& top) {
        using A = EntityAffine;
        if (top == "head") return A::S(0.71428573, 0.64935064, 0.7936508) * A::T(0.0, 1.3125, 0.22);
        if (top == "body") return A::S(0.625, 0.45454544, 0.45454544) * A::T(0.0, 2.0625, 0.0);
        return A::S(0.45454544, 0.41322312, 0.45454544) * A::T(0.0, 2.0625, 0.0);
    };
}

std::string CarpetColor(const ItemStackInfo& item) {
    if (item.empty()) return {};
    std::string p = SplitId(item.id).second;
    const std::string suf = "_carpet";
    if (p.size() <= suf.size() || p.compare(p.size() - suf.size(), suf.size(), suf) != 0) return {};
    return p.substr(0, p.size() - suf.size());
}

void RenderLlama(Ctx& ctx, const NbtTagPtr& e, bool trader) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const std::string kind = trader ? "trader_llama" : "llama";
    EntityModel model(fam, MobLayer(ctx, kind, baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    // Both families hide chests on babies. 26.x extractRenderState: hasChest = !baby && hasChest.
    const bool chest = !baby && (NbtFlag(e, "ChestedHorse", false) || NbtFlag(e, "Chested", false));
    model.SetVisible("left_chest", chest);
    model.SetVisible("right_chest", chest);
    const double eye = 1.78 * (baby ? 0.5 : 1.0);
    EntityAffine base = LivingBase(pl, eye, 1.0);
    TopPre pre = (!ctx.modern && baby) ? LlamaBabyPre() : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    const std::string var = LlamaVariantName(e);
    // 26.x renamed body textures to llama_<variant>[_baby]. Trader uses the same variant body.
    std::string texPath = ctx.modern
        ? "entity/llama/llama_" + var + (baby ? "_baby" : "")
        : "entity/llama/" + var;
    if (!TextureExists("minecraft", texPath))
        texPath = ctx.modern ? "entity/llama/" + var : "entity/llama/llama_" + var;
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));

    ItemStackInfo decorItem;
    if (auto eq = getChildByName(e, "equipment"))
        decorItem = ItemRender::ParseItemStack(getChildByName(eq, "body"));
    // 1.20.5-1.21.4 store the carpet in body_armor_item, older versions in DecorItem.
    if (decorItem.empty()) decorItem = ItemRender::ParseItemStack(getChildByName(e, "body_armor_item"));
    if (decorItem.empty()) decorItem = ItemRender::ParseItemStack(getChildByName(e, "DecorItem"));
    const std::string color = CarpetColor(decorItem);
    const bool carpet = !color.empty();
    // 1.21: swag wins over trader, including babies. 26.x: carpet only when adult; else trader decor
    // (baby trader uses trader_llama_baby, and a carpet on a baby trader is ignored).
    std::string decorTex;
    if (!ctx.modern) {
        if (carpet) decorTex = "entity/llama/decor/" + color;
        else if (trader) decorTex = "entity/llama/decor/trader_llama";
    } else if (carpet && !baby) {
        decorTex = "entity/equipment/llama_body/" + color;
    } else if (trader) {
        decorTex = baby ? "entity/equipment/llama_body/trader_llama_baby" : "entity/equipment/llama_body/trader_llama";
    }
    if (!decorTex.empty() && !TextureExists("minecraft", decorTex)) {
        if (carpet) decorTex = PickTex({ ("entity/llama/decor/" + color).c_str(), ("entity/equipment/llama_body/" + color).c_str() });
        else decorTex = PickTex({ "entity/llama/decor/trader_llama", "entity/equipment/llama_body/trader_llama",
                                  "entity/equipment/llama_body/trader_llama_baby" });
    }
    if (decorTex.empty()) return;
    // No trader_llama#decor layer; decor geometry is llama#decor / llama_baby#decor.
    EntityModel decor(fam, MobLayer(ctx, "llama", baby, "#decor"));
    if (!decor.Valid()) return;
    decor.CopyPoseFrom(model, { "head", "body", "right_hind_leg", "left_hind_leg", "right_front_leg", "left_front_leg",
                                "right_chest", "left_chest" }, true);
    decor.SetVisible("left_chest", chest);
    decor.SetVisible("right_chest", chest);
    EmitTextured(ctx, obj, decor, base, pre, "minecraft:" + decorTex, ctx.Texture("minecraft", decorTex));
}

// ---------- fox ----------
// Adult (and legacy baby, same mesh) sitting/sleeping. Modern baby is a different mesh; see PoseFoxBaby.
void PoseFoxAdult(EntityModel& model, bool sleeping, bool sitting, bool legacyBaby) {
    const float pi = static_cast<float>(kPi);
    if (sleeping) {
        SetXRot(model, "body", pi / 2.0f);                 // standing xRot kept
        int body = model.Find("body");
        if (body >= 0) model.Pose(body)[5] = -pi / 2.0f;   // zRot
        if (legacyBaby) {
            model.SetPose("body", 0.0f, 21.0f, -2.0f);
            if (body >= 0) model.Pose(body)[5] = -pi / 2.0f;
            SetXRot(model, "tail", -2.1816616f);
        } else {
            AddPos(model, "body", 0.0f, 5.0f, 0.0f);
            SetXRot(model, "tail", -pi * 5.0f / 6.0f);
        }
        AddPos(model, "head", 2.0f, 2.99f, 0.0f);
        model.SetRot("head", 0.0f, -pi * 2.0f / 3.0f, 1.0f / 22.0f);
        for (const char* leg : { "right_hind_leg", "left_hind_leg", "right_front_leg", "left_front_leg" })
            model.SetVisible(leg, false);
        return;
    }
    if (!sitting) return;
    SetXRot(model, "body", pi / 6.0f);
    AddPos(model, "body", 0.0f, -7.0f, 3.0f);
    SetXRot(model, "tail", pi / 4.0f);
    AddPos(model, "tail", 0.0f, 0.0f, -1.0f);
    if (legacyBaby) model.SetPose("head", -1.0f, 13.0f, -3.75f);
    else AddPos(model, "head", 0.0f, -6.5f, 2.75f);
    SetXRot(model, "right_front_leg", -pi / 12.0f);
    SetXRot(model, "left_front_leg", -pi / 12.0f);
    SetXRot(model, "right_hind_leg", -pi * 5.0f / 12.0f);
    SetXRot(model, "left_hind_leg", -pi * 5.0f / 12.0f);
    AddPos(model, "right_hind_leg", 0.0f, 4.0f, -0.25f);
    AddPos(model, "left_hind_leg", 0.0f, 4.0f, -0.25f);
}

// BabyFoxModel, ageScale 0.5. Crouch / pounce / faceplant are not applied.
void PoseFoxBaby(EntityModel& model, bool sleeping, bool sitting) {
    const float pi = static_cast<float>(kPi);
    const float s = 0.5f;
    if (sleeping) {
        int body = model.Find("body");
        if (body >= 0) { model.Pose(body)[3] = -pi / 18.0f; model.Pose(body)[5] = -pi / 2.0f; }
        AddPos(model, "body", -1.0f, 1.0f, -1.0f);
        SetXRot(model, "tail", -2.1816616f);
        AddPos(model, "tail", -0.7f, 0.9f, 0.6f);
        AddPos(model, "head", -2.0f, 2.8f, -4.0f);
        model.SetRot("head", 0.0f, -pi * 2.0f / 3.0f, 1.0f / 22.0f);
        for (const char* leg : { "right_hind_leg", "left_hind_leg", "right_front_leg", "left_front_leg" })
            model.SetVisible(leg, false);
        return;
    }
    if (!sitting) return;
    SetXRot(model, "body", -0.959931f);
    AddPos(model, "body", 0.0f, 3.0f * s, -4.5f * s);
    AddPos(model, "tail", 0.0f, -0.6f, -2.0f * s);
    SetXRot(model, "tail", 0.95993114f);
    AddPos(model, "head", 0.0f, -0.75f, 0.0f);
    SetXRot(model, "right_front_leg", -pi / 12.0f);
    SetXRot(model, "left_front_leg", -pi / 12.0f);
    AddPos(model, "right_front_leg", 0.01f, 0.0f, -1.0f);
    AddPos(model, "left_front_leg", -0.01f, 0.0f, -1.0f);
    AddPos(model, "right_hind_leg", 0.01f, 0.0f, -3.75f);
    AddPos(model, "left_hind_leg", -0.01f, 0.0f, -3.75f);
}

void RenderFox(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:fox_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:fox_baby#main" : "minecraft:fox#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // Fox "Sleeping" is the curl-up flag, not a bed (ReadPlacement only treats SleepingX / sleeping_pos as a bed).
    const bool sleeping = NbtFlag(e, "Sleeping", false);
    const bool sitting = !sleeping && IsSitting(e);
    if (modernBaby) PoseFoxBaby(model, sleeping, sitting);
    else PoseFoxAdult(model, sleeping, sitting, !ctx.modern && baby);
    if (!sleeping) ApplyHeadLook(model, pl);   // sitting still receives the head look
    // Held-item layer, crouch, pounce and faceplant are skipped.
    EntityAffine base = LivingBase(pl, 0.4 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeFox) : nullptr;
    std::string type = Str(e, "Type");
    if (type.empty()) type = Str(e, "variant");
    if (type.empty()) type = Str(e, "Variant");
    const bool snow = type.find("snow") != std::string::npos;
    std::string stem;
    if (!ctx.modern) stem = snow ? (sleeping ? "snow_fox_sleep" : "snow_fox") : (sleeping ? "fox_sleep" : "fox");
    else if (snow) stem = sleeping ? "fox_snow_sleep" : "fox_snow";
    else stem = sleeping ? "fox_sleep" : "fox";
    if (modernBaby) stem += "_baby";
    std::string texPath = "entity/fox/" + stem;
    OutObject& obj = ctx.NewObject("minecraft:fox");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// ---------- bee ----------
// On the ground setupAnim leaves the baked wings and legs. In the air (ageInTicks 0) it poses them once.
void PoseBeeAir(EntityModel& model, bool angry) {
    const float pi = static_cast<float>(kPi);
    const float wz = pi * 0.15f;   // cos(0) * pi * 0.15
    int rw = model.Find("right_wing"), lw = model.Find("left_wing");
    if (rw >= 0) { model.Pose(rw)[4] = 0.0f; model.Pose(rw)[5] = wz; }
    if (lw >= 0 && rw >= 0) {
        model.Pose(lw)[3] = model.Pose(rw)[3];
        model.Pose(lw)[4] = 0.0f;
        model.Pose(lw)[5] = -model.Pose(rw)[5];
    }
    const float leg = pi / 4.0f;
    SetXRot(model, "front_legs", leg);
    SetXRot(model, "middle_legs", leg);
    SetXRot(model, "back_legs", leg);
    if (angry) return;
    int bone = model.Find("bone");
    if (bone >= 0) {
        model.Pose(bone)[3] = 0.1f + pi * 0.025f;
        model.Pose(bone)[1] -= 0.9f;
    }
    SetXRot(model, "front_legs", -pi * 0.1f + pi / 8.0f);
    SetXRot(model, "back_legs", -pi * 0.05f + pi / 4.0f);
}

void RenderBee(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:bee_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:bee_baby#main" : "minecraft:bee#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    model.SetVisible("stinger", !NbtFlag(e, "HasStung", false));
    const bool angry = IsAngry(e);
    const bool onGround = NbtFlag(e, "OnGround", true);
    if (!onGround) PoseBeeAir(model, angry);
    EntityAffine base = LivingBase(pl, 0.3 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeBee) : nullptr;
    const bool nectar = NbtFlag(e, "HasNectar", false);
    std::string stem = "bee";
    if (angry && nectar) stem = "bee_angry_nectar";
    else if (angry) stem = "bee_angry";
    else if (nectar) stem = "bee_nectar";
    if (modernBaby) stem += "_baby";
    const std::string texPath = "entity/bee/" + stem;
    OutObject& obj = ctx.NewObject("minecraft:bee");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// ---------- goat / polar bear / panda / rabbit / turtle / parrot / slime ----------
void RenderGoat(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:goat_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:goat_baby#main" : "minecraft:goat#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    // BabyGoatModel forces head.xRot = pi/8 when not ramming. Ramming is skipped.
    if (modernBaby) SetXRot(model, "head", static_cast<float>(kPi / 8.0));
    model.SetVisible("left_horn", NbtFlag(e, "HasLeftHorn", true));
    model.SetVisible("right_horn", NbtFlag(e, "HasRightHorn", true));
    EntityAffine base = LivingBase(pl, 1.1 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeGoat) : nullptr;
    std::string texPath = (modernBaby ? "entity/goat/goat_baby" : "entity/goat/goat");
    OutObject& obj = ctx.NewObject("minecraft:goat");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

void RenderPolarBear(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:polar_bear_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:polar_bear_baby#main" : "minecraft:polar_bear#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // Standing (standScale 0) is the baked pose: adult body xRot = pi/2, baby body xRot = 0. Head pitch only.
    // Rear-up animation is skipped. Renderer scale 1.2 is legacy-only; 26.x bakes it into the adult root.
    ApplyHeadLook(model, pl);
    const double rs = ctx.modern ? 1.0 : 1.2;
    EntityAffine base = LivingBase(pl, 1.19 * (baby ? 0.5 : 1.0), rs);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgePolarBear) : nullptr;
    std::string texPath = modernBaby ? "entity/bear/polarbear_baby" : "entity/bear/polarbear";
    OutObject& obj = ctx.NewObject("minecraft:polar_bear");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

std::string PandaGene(const NbtTagPtr& e) {
    std::string g = Str(e, "MainGene");
    if (g.empty()) g = Str(e, "main_gene");
    if (g.empty()) return "normal";
    g = SplitId(g).second;
    return g.empty() ? "normal" : g;
}

void RenderPanda(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:panda_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:panda_baby#main" : "minecraft:panda#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // sit / lie / roll / sneeze / unhappy are animated (amounts 0 at rest). Do not clear the baked body xRot.
    // Held item layer is skipped.
    ApplyHeadLook(model, pl);
    EntityAffine base = LivingBase(pl, 1.06 * (baby ? 0.5 : 1.0), 1.0);
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgePanda) : nullptr;
    const std::string gene = PandaGene(e);
    std::string texPath;
    if (!ctx.modern) texPath = gene == "normal" ? "entity/panda/panda" : "entity/panda/" + gene + "_panda";
    else if (!modernBaby) texPath = gene == "normal" ? "entity/panda/panda" : "entity/panda/panda_" + gene;
    else texPath = gene == "normal" ? "entity/panda/panda_baby" : "entity/panda/" + gene + "_panda_baby";
    OutObject& obj = ctx.NewObject("minecraft:panda");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// RabbitModel.renderToBuffer. Adult S(0.6)*T(0,1,0). Baby head group vs body group differ.
TopPre RabbitPre(bool baby) {
    return [baby](const std::string& top) {
        using A = EntityAffine;
        if (!baby) return A::S(0.6) * A::T(0.0, 1.0, 0.0);
        if (top == "head" || top == "left_ear" || top == "right_ear" || top == "nose")
            return A::S(0.56666666) * A::T(0.0, 1.375, 0.125);
        return A::S(0.4) * A::T(0.0, 2.25, 0.0);
    };
}

bool NameIsToast(const std::string& raw) { return PlainCustomName(raw, "Toast"); }

std::string RabbitTextureName(const NbtTagPtr& e) {
    if (NameIsToast(Str(e, "CustomName"))) return "toast";
    static const char* k[] = { "brown", "white", "black", "white_splotched", "gold", "salt" };
    std::string vs = Str(e, "variant");
    if (vs.empty()) vs = Str(e, "Variant");
    if (!vs.empty()) {
        std::string v = SplitId(vs).second;
        if (v.rfind("rabbit_", 0) == 0) v = v.substr(7);
        if (v.size() > 5 && v.compare(v.size() - 5, 5, "_baby") == 0) v = v.substr(0, v.size() - 5);
        if (v == "evil") v = "caerbannog";
        if (!v.empty()) return v;
    }
    int t = static_cast<int>(NumOr(e, "RabbitType", 0));
    if (t == 99) return "caerbannog";
    if (t < 0 || t > 5) t = 0;
    return k[t];
}

void RenderRabbit(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:rabbit_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:rabbit_baby#main" : "minecraft:rabbit#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float pitch = HeadPitch(pl);
    TopPre pre;
    if (!ctx.modern) {
        // jumpRotation 0: haunches -21 deg (bake is -20), front legs -11 deg (bake is -10), ears yRot ∓ pi/12.
        const float d = static_cast<float>(kPi / 180.0);
        model.SetRot("nose", pitch, 0.0f, 0.0f);
        model.SetRot("head", pitch, 0.0f, 0.0f);
        model.SetRot("right_ear", pitch, static_cast<float>(-kPi / 12.0), 0.0f);
        model.SetRot("left_ear", pitch, static_cast<float>(kPi / 12.0), 0.0f);
        model.SetRot("left_haunch", -21.0f * d, 0.0f, 0.0f);
        model.SetRot("right_haunch", -21.0f * d, 0.0f, 0.0f);
        model.SetRot("left_hind_foot", 0.0f, 0.0f, 0.0f);
        model.SetRot("right_hind_foot", 0.0f, 0.0f, 0.0f);
        model.SetRot("left_front_leg", -11.0f * d, 0.0f, 0.0f);
        model.SetRot("right_front_leg", -11.0f * d, 0.0f, 0.0f);
        pre = RabbitPre(baby);
    } else if (pitch != 0.0f) {
        // 26.x setupAnim writes head.xRot = pitch absolutely, which would wipe the baked counter-rotation
        // that keeps the head level against body.xRot -0.3927. Add the pitch on top of that bind pose instead.
        model.AddRot("head", pitch, 0.0f, 0.0f);
    }
    EntityAffine base = LivingBase(pl, 0.3 * (baby ? 0.5 : 1.0), 1.0);
    const std::string name = RabbitTextureName(e);
    std::string texPath = ctx.modern
        ? "entity/rabbit/rabbit_" + name + (modernBaby ? "_baby" : "")
        : "entity/rabbit/" + name;
    if (!TextureExists("minecraft", texPath))
        texPath = ctx.modern ? "entity/rabbit/" + name : "entity/rabbit/rabbit_" + name;
    OutObject& obj = ctx.NewObject("minecraft:rabbit");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

void RenderTurtle(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    const bool modernBaby = ctx.modern && baby && EntityModel::Exists(fam, "minecraft:turtle_baby#main");
    EntityModel model(fam, modernBaby ? "minecraft:turtle_baby#main" : "minecraft:turtle#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const bool egg = !baby && NbtFlag(e, "HasEgg", false);
    model.SetVisible("egg_belly", egg);
    EntityAffine base = LivingBase(pl, 0.28 * (baby ? 0.5 : 1.0), 1.0);
    if (egg && !ctx.modern) base = base * A::T(0.0, -0.08, 0.0);          // 1.21 renderToBuffer
    if (egg && ctx.modern) {                                                // AdultTurtleModel: root.y--
        int root = model.Find("root");
        if (root >= 0) model.Pose(root)[1] -= 1.0f;
    }
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeTurtle) : nullptr;
    std::string texPath = !ctx.modern ? "entity/turtle/big_sea_turtle"
                                      : (modernBaby ? "entity/turtle/turtle_baby" : "entity/turtle/turtle");
    OutObject& obj = ctx.NewObject("minecraft:turtle");
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// 1.21 prepare+setupAnim writes absolute rotations (bake is identity rot). 26.x bakes the standing
// prepare pose and setupAnim only adds wing zRot ±0.0873. Sitting offsets differ by family.
void PoseParrot(EntityModel& model, bool sitting, bool modern, float pitch) {
    const float pi = static_cast<float>(kPi);
    model.SetRot("feather", -0.2214f, 0.0f, 0.0f);
    model.SetRot("body", 0.4937f, 0.0f, 0.0f);
    model.SetRot("tail", 1.015f, 0.0f, 0.0f);
    model.SetRot("left_wing", -0.6981f, -pi, -0.0873f);
    model.SetRot("right_wing", -0.6981f, -pi, 0.0873f);
    model.SetRot("left_leg", -0.0299f, 0.0f, 0.0f);
    model.SetRot("right_leg", -0.0299f, 0.0f, 0.0f);
    model.SetRot("head", pitch, 0.0f, 0.0f);
    if (!sitting) return;
    if (!modern) {
        int h = model.Find("head"); if (h >= 0) model.Pose(h)[1] = 17.59f;
        model.SetRot("tail", 1.5388988f, 0.0f, 0.0f);
        int t = model.Find("tail"); if (t >= 0) model.Pose(t)[1] = 22.97f;
        int b = model.Find("body"); if (b >= 0) model.Pose(b)[1] = 18.4f;
        int lw = model.Find("left_wing"); if (lw >= 0) model.Pose(lw)[1] = 18.84f;
        int rw = model.Find("right_wing"); if (rw >= 0) model.Pose(rw)[1] = 18.84f;
        int ll = model.Find("left_leg");
        int rl = model.Find("right_leg");
        if (ll >= 0) { model.Pose(ll)[1] = 23.0f; model.Pose(ll)[3] = -0.0299f + 1.0f; }
        if (rl >= 0) { model.Pose(rl)[1] = 23.0f; model.Pose(rl)[3] = -0.0299f + 1.0f; }
        return;
    }
    // 26.x prepare(SITTING) is +1 on the baked standing pose (the local 1.9 is unused). Wing zRot is set, not added.
    for (const char* p : { "head", "tail", "body", "left_wing", "right_wing", "left_leg", "right_leg" })
        AddPos(model, p, 0.0f, 1.0f, 0.0f);
    model.AddRot("tail", pi / 6.0f, 0.0f, 0.0f);
    model.AddRot("left_leg", 1.0f, 0.0f, 0.0f);
    model.AddRot("right_leg", 1.0f, 0.0f, 0.0f);
}

void RenderParrot(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:parrot#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseParrot(model, IsSitting(e), ctx.modern, HeadPitch(pl));
    // Party and flying poses are skipped.
    EntityAffine base = LivingBase(pl, 0.54, 1.0);
    static const char* k[] = { "red_blue", "blue", "green", "yellow_blue", "grey" };
    std::string name;
    std::string vs = Str(e, "variant");
    if (vs.empty()) vs = Str(e, "Variant");
    if (!vs.empty()) {
        name = SplitId(vs).second;
        if (name.rfind("parrot_", 0) == 0) name = name.substr(7);
    } else {
        int v = static_cast<int>(NumOr(e, "Variant", 0));
        if (v < 0 || v > 4) v = 0;
        name = k[v];
    }
    if (name == "gray") name = "grey";
    if (name.empty()) name = "red_blue";
    const std::string texPath = "entity/parrot/parrot_" + name;
    OutObject& obj = ctx.NewObject("minecraft:parrot");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

void RenderSlime(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:slime#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // Saved Size is getSize()-1. A missing tag is a size-1 slime (setSize(0+1) on read).
    int size = 1;
    if (getChildByName(e, "Size")) {
        int n = static_cast<int>(NumOr(e, "Size", 0));
        if (n < 0) n = 0;
        size = n + 1;
    }
    // scale(): S(0.999) * T(0, 0.001, 0) * S(size) at squish 0, before translate(0, -1.501, 0).
    EntityAffine base = LivingBase(pl, 0.625 * size, 0.999);
    base = base * A::T(0.0, 1.501, 0.0) * A::T(0.0, 0.001, 0.0) * A::S(static_cast<double>(size)) * A::T(0.0, -1.501, 0.0);
    const std::string texPath = "entity/slime/slime";
    std::string tex = ctx.Texture("minecraft", texPath);
    OutObject& obj = ctx.NewObject("minecraft:slime");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, tex);
    EntityModel outer(fam, "minecraft:slime#outer");
    if (outer.Valid()) {
        // Same texture, translucent in vanilla. The OBJ material is opaque; the shell is still emitted.
        EmitTextured(ctx, obj, outer, base, nullptr, "minecraft:" + texPath + "#outer", tex);
    }
}

// ---------- aquatic / small monsters / blaze / ghast / magma cube ----------
// Poses are setupAnim at ageInTicks 0 with no target, like the other mobs.

// LivingEntityRenderer.submit with the renderer's own setupRotations (rot) and scale() (rendererScale).
EntityAffine MobBase(const Placement& p, const EntityAffine& rot, double rendererScale) {
    using A = EntityAffine;
    return A::T(p.x, p.y, p.z) * A::S(p.scale) * rot * A::S(-1.0, -1.0, 1.0) * A::S(rendererScale) * A::T(0.0, -1.501, 0.0);
}
EntityAffine BodyRot(const Placement& p) { return EntityAffine::RYDeg(180.0 - p.yaw); }

void SetZRot(EntityModel& model, const char* part, float z) {
    int i = model.Find(part); if (i < 0) return;
    model.Pose(i)[5] = z;
}

// Entity.isInWater is not saved. Approximate it with the block at the entity's feet.
bool InWater(Ctx& ctx, const Placement& p) {
    try {
        const std::string n = ctx.BlockName(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y + 0.1)),
                                            static_cast<int>(std::floor(p.z)));
        if (n.rfind("minecraft:water", 0) == 0 || n.rfind("minecraft:bubble_column", 0) == 0) return true;
        if (n.find("waterlogged:true") != std::string::npos || n.find("waterlogged=true") != std::string::npos) return true;
        return n.find("seagrass") != std::string::npos || n.find("kelp") != std::string::npos;
    } catch (...) { return true; }
}

// CodRenderer / SalmonRenderer.setupRotations: on land the fish lies on its side.
void RenderFish(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& layer,
                const std::string& texPath, double landX, double landY, double landZ) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, layer);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    A rot = BodyRot(pl);
    if (!InWater(ctx, pl)) rot =rot * A::T(landX, landY, landZ) * A::RZDeg(90.0);
    OutObject& obj = ctx.NewObject(id);
    EmitTextured(ctx, obj, model, MobBase(pl, rot, 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}
void RenderCod(Ctx& ctx, const NbtTagPtr& e) {
    RenderFish(ctx, e, "minecraft:cod", "minecraft:cod#main", "entity/fish/cod", 0.1, 0.1, -0.1);
}
// 26.x Salmon "type" small/medium/large picks the model. 1.21.1 has one size.
void RenderSalmon(Ctx& ctx, const NbtTagPtr& e) {
    std::string layer = "minecraft:salmon#main";
    if (ctx.modern) {
        const std::string type = Str(e, "type");
        if (type == "small") layer = "minecraft:salmon_small#main";
        else if (type == "large") layer = "minecraft:salmon_large#main";
    }
    RenderFish(ctx, e, "minecraft:salmon", layer, "entity/fish/salmon", 0.2, 0.1, 0.0);
}

// PufferfishRenderer: PuffState 0/1/2 -> small/medium/big. setupRotations bobs by cos(0)*0.08 first.
void RenderPufferfish(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const int puff = std::clamp(static_cast<int>(NumOr(e, "PuffState", 0)), 0, 2);
    static const char* kLayers[] = { "minecraft:pufferfish_small#main", "minecraft:pufferfish_medium#main",
                                     "minecraft:pufferfish_big#main" };
    EntityModel model(fam, kLayers[puff]);
    if (!model.Valid()) return;
    // setupAnim: right fin zRot = -0.2 + 0.4 sin(0), left = 0.2 - 0.4 sin(0).
    SetZRot(model, puff == 0 ? "right_fin" : "right_blue_fin", -0.2f);
    SetZRot(model, puff == 0 ? "left_fin" : "left_blue_fin", 0.2f);
    Placement pl = ReadPlacement(e);
    const A rot = A::T(0.0, 0.08, 0.0) * BodyRot(pl);
    const std::string texPath = "entity/fish/pufferfish";
    OutObject& obj = ctx.NewObject("minecraft:pufferfish");
    EmitTextured(ctx, obj, model, MobBase(pl, rot, 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// SquidRenderer.setupRotations replaces the living one. xBodyRot/zBodyRot and the tentacle angle are
// not saved, so they stay 0. 26.x babies have their own layer and offsets; 1.21.1 squids have no babies.
void RenderSquid(Ctx& ctx, const NbtTagPtr& e, bool glow) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const std::string kind = glow ? "glow_squid" : "squid";
    const bool baby = ctx.modern && IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, kind, baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const A rot = A::T(0.0, baby ? 0.25 : 0.5, 0.0) * BodyRot(pl) * A::T(0.0, baby ? -0.6 : -1.2, 0.0);
    const std::string texPath = "entity/squid/" + kind + (baby ? "_baby" : "");
    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    EmitTextured(ctx, obj, model, MobBase(pl, rot, 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// DolphinModel.setupAnim: body follows the head pitch. Not moving, so the tail stays straight.
void RenderDolphin(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = ctx.modern && IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "dolphin", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    SetXRot(model, "body", HeadPitch(pl));
    const std::string texPath = baby ? "entity/dolphin/dolphin_baby" : PickTex({ "entity/dolphin/dolphin", "entity/dolphin" });
    OutObject& obj = ctx.NewObject("minecraft:dolphin");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// GuardianModel.setupAnim: head pitch; spikes and eye keep the baked rest pose (no attack target).
// The elder is the same model at 2.35x: renderer scale in 1.21.1, baked into the 26.x root.
void RenderGuardian(Ctx& ctx, const NbtTagPtr& e, bool elder) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, elder ? "minecraft:elder_guardian#main" : "minecraft:guardian#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const double scale = (elder && !ctx.modern) ? 2.35 : 1.0;
    const std::string texPath = elder ? PickTex({ "entity/guardian/guardian_elder", "entity/guardian_elder" })
                                      : PickTex({ "entity/guardian/guardian", "entity/guardian" });
    OutObject& obj = ctx.NewObject(elder ? "minecraft:elder_guardian" : "minecraft:guardian");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), scale), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// SilverfishModel / EndermiteModel.setupAnim at age 0: segments sway sideways.
void RenderSegmentedBug(Ctx& ctx, const NbtTagPtr& e, bool endermite) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, endermite ? "minecraft:endermite#main" : "minecraft:silverfish#main");
    if (!model.Valid()) return;
    const int segments = endermite ? 4 : 7;
    const float rotK = endermite ? 0.01f : 0.05f, posK = endermite ? 0.1f : 0.2f;
    std::vector<float> yRot(segments), x(segments);
    for (int i = 0; i < segments; ++i) {
        const float a = i * 0.15f * static_cast<float>(kPi);
        const float d = static_cast<float>(std::abs(i - 2));
        yRot[i] = std::cos(a) * static_cast<float>(kPi) * rotK * (1.0f + d);
        x[i] = std::sin(a) * static_cast<float>(kPi) * posK * d;
        const int p = model.Find("segment" + std::to_string(i));
        if (p >= 0) { model.Pose(p)[0] = x[i]; model.Pose(p)[4] = yRot[i]; }
    }
    if (!endermite) {
        // bodyLayers[0] follows segment 2 (rotation only), [1] segment 4, [2] segment 1.
        const int follow[3] = { 2, 4, 1 };
        for (int l = 0; l < 3; ++l) {
            const int p = model.Find("layer" + std::to_string(l));
            if (p < 0) continue;
            model.Pose(p)[4] = yRot[follow[l]];
            if (l > 0) model.Pose(p)[0] = x[follow[l]];
        }
    }
    Placement pl = ReadPlacement(e);
    const std::string kind = endermite ? "endermite" : "silverfish";
    const std::string texPath = PickTex({ ("entity/" + kind + "/" + kind).c_str(), ("entity/" + kind).c_str() });
    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// BlazeModel: the rods' age-0 positions are already the baked rest pose; only the head looks.
void RenderBlaze(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:blaze#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const std::string texPath = PickTex({ "entity/blaze/blaze", "entity/blaze" });
    OutObject& obj = ctx.NewObject("minecraft:blaze");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// GhastModel.animateTentacles at age 0. 1.21.1 renderer scale 4.5; 26.x bakes it into the root.
// isCharging is synced data, not saved, so the normal face is used.
void RenderGhast(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:ghast#main");
    if (!model.Valid()) return;
    for (int i = 0; i < 9; ++i)
        SetXRot(model, ("tentacle" + std::to_string(i)).c_str(), 0.2f * std::sin(static_cast<float>(i)) + 0.4f);
    Placement pl = ReadPlacement(e);
    const std::string texPath = "entity/ghast/ghast";
    OutObject& obj = ctx.NewObject("minecraft:ghast");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), ctx.modern ? 1.0 : 4.5), nullptr,
                 "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// MagmaCubeRenderer: scale(size) at squish 0, without the slime's 0.999 downscale. Size as for slimes.
void RenderMagmaCube(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:magma_cube#main");
    if (!model.Valid()) return;
    int size = 1;
    if (getChildByName(e, "Size")) size = std::max(0, static_cast<int>(NumOr(e, "Size", 0))) + 1;
    Placement pl = ReadPlacement(e);
    const std::string texPath = "entity/slime/magmacube";
    OutObject& obj = ctx.NewObject("minecraft:magma_cube");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), static_cast<double>(size)), nullptr,
                 "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}


// ==== batch2 A renderers (insert below) ====

// RavagerModel.setupAnim at rest: not stunned and not attacking or roaring (none of which is saved), so the
// neck and mouth keep their rest values. RavagerRenderer's 1.1 is the shadow radius, not a scale.
// Adults only in both families. The head follows the pitch, and the mouth's rest xRot is pi * 0.01.
void RenderRavager(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:ravager#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    SetXRot(model, "mouth", static_cast<float>(kPi * 0.01));
    const std::string texPath = "entity/illager/ravager";
    OutObject& obj = ctx.NewObject("minecraft:ravager");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr,
                 "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// HoglinModel / ZoglinModel (AbstractHoglinRenderer, no renderer scale; 0.7 is the shadow radius).
// setupAnim at rest: the head sits at its headbutt rest pose (0.8727) and ignores head pitch, ears keep
// their baked rest zRot. 1.21.1 babies use the adult layer with AgeableList scaling (8, 6, 1.9, 2.0, 24).
// That setupAnim also sets mane z to -7 for adults and -3 for babies, so the baked -5 is overridden.
// 26.x babies have their own layer (BabyHoglinModel), whose rest headbutt y offset is 0.
const AgeableParams kAgeHoglin { true, 8.0f, 6.0f, 1.9f, 2.0f, 24.0f, { "head" } };
void RenderHoglinLike(Ctx& ctx, const NbtTagPtr& e, const std::string& kind, const std::string& id) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, kind, baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    if (int mane = model.Find("mane"); mane >= 0) model.Pose(mane)[2] = (!ctx.modern && baby) ? -3.0f : -7.0f;
    const std::string adultTex = "entity/hoglin/" + kind;
    const std::string babyTex = adultTex + "_baby";
    const std::string texPath = (ctx.modern && baby) ? PickTex({ babyTex.c_str(), adultTex.c_str() }) : adultTex;
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeHoglin) : nullptr;
    OutObject& obj = ctx.NewObject(id);
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}
void RenderHoglin(Ctx& ctx, const NbtTagPtr& e) { RenderHoglinLike(ctx, e, "hoglin", "minecraft:hoglin"); }
void RenderZoglin(Ctx& ctx, const NbtTagPtr& e) { RenderHoglinLike(ctx, e, "zoglin", "minecraft:zoglin"); }

// OcelotModel (1.21.1) / AdultOcelotModel, BabyOcelotModel (26.x). Crouching and sprinting are not saved,
// so the standing pose applies: setupAnim p == 1 gives tail2 xRot 1.7278761, all else stays baked.
// OcelotRenderer's 0.4 is the shadow radius. Legacy babies use the AgeableList scaling of kAgeOcelot.
// 1.21.1 has no ocelot_baby texture, so legacy babies use ocelot.png.
void RenderOcelot(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "ocelot", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    SetXRot(model, "tail2", 1.7278761f);
    ApplyHeadLook(model, pl);
    const std::string adultTex = "entity/cat/ocelot";
    const std::string babyTex = "entity/cat/ocelot_baby";
    const std::string texPath = (ctx.modern && baby) ? PickTex({ babyTex.c_str(), adultTex.c_str() }) : adultTex;
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kAgeOcelot) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:ocelot");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// ---- end batch2 A ----

// ==== batch2 B renderers (insert below) ====

// TadpoleModel.setupAnim: tail yRot = -amp*0.25*sin(0.3*age). At age 0 that is 0, so the baked pose stands.
// TadpoleRenderer's 0.14 is the shadow radius, not a scale, so only the living base applies.
void RenderTadpole(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:tadpole#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const std::string texPath = "entity/tadpole/tadpole";
    OutObject& obj = ctx.NewObject("minecraft:tadpole");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// VexModel.setupAnim at age 0, not charging, nothing in hands: the arms sit at pi/5 with the cos(0)*0.1 bob,
// the body tilts pi/20, and the wings take their fixed angles with the flap yRot at age 0.
// Head pitch follows the entity. Yaw is relative to the body and not saved, so it stays 0.
// Held items (ItemInHandLayer) are not drawn. The vex texture is translucent in vanilla; the OBJ material is opaque.
void RenderVex(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:vex#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const float pi = static_cast<float>(kPi);
    const float armZ = pi / 5.0f + 0.1f;
    SetZRot(model, "right_arm", armZ);
    SetZRot(model, "left_arm", -armZ);
    model.SetRot("body", pi / 20.0f, 0.0f, 0.0f);
    const float wingY = 1.0995574f + static_cast<float>(16.2 * kPi / 180.0);  // + cos(0) * 16.2 deg
    model.SetRot("left_wing", 0.47123888f, wingY, -0.47123888f);
    model.SetRot("right_wing", 0.47123888f, -wingY, 0.47123888f);
    const std::string texPath = "entity/illager/vex";
    OutObject& obj = ctx.NewObject("minecraft:vex");
    const EntityAffine vexBase = MobBase(pl, BodyRot(pl), 1.0);
    EmitTextured(ctx, obj, model, vexBase, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    // ItemInHandLayer with VexModel.translateToHand: the arm, scale 0.55, then the hand offset (x flipped for the left hand)
    const Equipment eq = ReadEquipment(e);
    for (int side = 0; side < 2; ++side) {
        const bool isLeft = side == 1;
        const ItemStackInfo& item = side == 0 ? (eq.leftHanded ? eq.offhand : eq.mainhand)
                                              : (eq.leftHanded ? eq.mainhand : eq.offhand);
        if (item.empty()) continue;
        const int arm = model.Find(isLeft ? "left_arm" : "right_arm");
        if (arm < 0) continue;
        using A = EntityAffine;
        A m = model.PartTransform(arm, vexBase) * A::S(0.55) * A::T(isLeft ? -0.046875 : 0.046875, -0.15625, 0.078125);
        m = m * A::RX(-kPi / 2) * A::RY(kPi) * A::T((isLeft ? -1.0 : 1.0) / 16.0, 2.0 / 16.0, -10.0 / 16.0);
        AppendItem(ctx, obj, item, isLeft ? ItemDisplay::ThirdPersonLeftHand : ItemDisplay::ThirdPersonRightHand, isLeft, m);
    }
}

// AllayModel.setupAnim at age 0, not moving, dancing or holding: the wings flap at amplitude 0.15*pi,
// the arms hang at the idle sway 0.43633 rad, and the root gets the cos(0)*0.25 idle bob.
// Dancing, the held-item raise and the spin are client-side and not saved, so they are left out.
// Head pitch follows the entity; yaw is relative to the body and stays 0.
void RenderAllay(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:allay#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const float pi = static_cast<float>(kPi);
    const float flap = 0.15f * pi;
    const float sway = 0.43633232f;
    model.SetRot("right_wing", sway, -pi / 4.0f + flap, 0.0f);
    model.SetRot("left_wing", sway, pi / 4.0f - flap, 0.0f);
    model.SetRot("right_arm", 0.0f, 0.0f, sway);
    model.SetRot("left_arm", 0.0f, 0.0f, -sway);
    AddPos(model, "root", 0.0f, 0.25f, 0.0f);
    const std::string texPath = "entity/allay/allay";
    OutObject& obj = ctx.NewObject("minecraft:allay");
    const EntityAffine allayBase = MobBase(pl, BodyRot(pl), 1.0);
    EmitTextured(ctx, obj, model, allayBase, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    // ItemInHandLayer with AllayModel.translateToHand. Arm holding raise is runtime (progress 0 -> xRot 0 here).
    ItemStackInfo held;
    if (auto inv = getChildByName(e, "Inventory"); inv && inv->type == TagType::LIST) {
        for (const auto& c : inv->children) {
            if (NumOr(c, "Slot", -1) == 0) held = ItemRender::ParseItemStack(c);
        }
    }
    if (!held.empty()) {
        using A = EntityAffine;
        const int body = model.Find("body"), arm = model.Find("right_arm");
        if (body >= 0 && arm >= 0) {
            A m = model.PartTransform(body, allayBase) * A::T(0.0, 1.0 / 16.0, 3.0 / 16.0) * A::RX(model.Pose(arm)[3]) *
                  A::S(0.7) * A::T(1.0 / 16.0, 0.0, 0.0);
            m = m * A::RX(-kPi / 2) * A::RY(kPi) * A::T(1.0 / 16.0, 2.0 / 16.0, -10.0 / 16.0);
            AppendItem(ctx, obj, held, ItemDisplay::ThirdPersonRightHand, false, m);
        }
    }
}

// PhantomRenderer: setupRotations is the body yaw plus RX(pitch) (before the -1,-1,1 flip), then scale(1 + 0.15*size) and
// translate(0, 1.3125, 0.1875) before the usual -1.501 lift. Size is "size" in 26.x and "Size" in 1.21.1.
// The wings and tail follow flapTime, which includes a per-entity offset that is not saved, so they keep
// the dump's rest pose. The eyes are the same posed model with the eyes texture.
void RenderPhantom(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:phantom#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const int size = std::clamp(static_cast<int>(NumOr(e, "size", NumOr(e, "Size", 0))), 0, 64);
    const double s = 1.0 + 0.15 * size;
    const EntityAffine base = A::T(pl.x, pl.y, pl.z) * A::S(pl.scale) * BodyRot(pl) * A::RXDeg(pl.pitch)
                            * A::S(-1.0, -1.0, 1.0) * A::S(s) * A::T(0.0, 1.3125, 0.1875) * A::T(0.0, -1.501, 0.0);
    const std::string texPath = PickTex({ "entity/phantom/phantom", "entity/phantom" });
    OutObject& obj = ctx.NewObject("minecraft:phantom");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    const std::string eyes = PickTex({ "entity/phantom/phantom_eyes", "entity/phantom_eyes" });
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
}

// ---- end batch2 B ----

// ==== batch2 C renderers (insert below) ====
// ---------- shulker ----------
// ShulkerModel.setupAnim (age 0): bs = (0.5+peek)π, lid y = 16 + sin(bs)*8, peek>0.3 时 lid yRot = q^4·π/8, q = -1+sin(bs).
// ShulkerRenderer: 无阴影; yBodyRot 恒为 0 (身体不随 Rotation 转), head yaw = Rotation - 180。
// 外壳伸展方向 D = AttachFace.getOpposite() (3D id ^ 1), 按 rotateAround(R(D), (0,0.5,0)) 旋转;
// R(D) 为 26.3 Direction.getRotation 的反汇编结果 (JDK 26 javap):
// DOWN=RX(π), UP=I, NORTH=RX(π/2)·RZ(π), SOUTH=RX(π/2), WEST=RX(π/2)·RZ(π/2), EAST=RX(π/2)·RZ(-π/2)。
// getRenderPosition (clientSideTeleportInterpolation 的瞬移插值偏移) 不存盘, 忽略。
void RenderShulker(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:shulker#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);

    // ---- pose ----
    const float peek = static_cast<float>(std::clamp(static_cast<double>(NumOr(e, "Peek", 0.0)), 0.0, 100.0) * 0.01);
    const float bs = (0.5f + peek) * static_cast<float>(kPi);
    const float s = std::sin(bs);
    const float q = -1.0f + s;
    if (int lid = model.Find("lid"); lid >= 0) {
        float* p = model.Pose(lid);
        p[1] = 16.0f + s * 8.0f;
        p[4] = peek > 0.3f ? q * q * q * q * static_cast<float>(kPi) * 0.125f : 0.0f;
    }
    model.SetRot("head", HeadPitch(pl), static_cast<float>((pl.yaw - 180.0) * kPi / 180.0), 0.0f);

    // ---- attach face ----
    const int face = std::clamp(static_cast<int>(NumOr(e, "AttachFace", 0)), 0, 5);
    const int dir = face ^ 1;  // getOpposite(): DOWN<->UP, NORTH<->SOUTH, WEST<->EAST
    const double h = kPi / 2.0;
    A R;
    switch (dir) {
        case 0: R = A::RX(kPi); break;                 // DOWN
        case 1: break;                                 // UP
        case 2: R = A::RX(h) * A::RZ(kPi); break;      // NORTH
        case 3: R = A::RX(h); break;                   // SOUTH
        case 4: R = A::RX(h) * A::RZ(h); break;        // WEST
        default: R = A::RX(h) * A::RZ(-h); break;      // EAST
    }
    const A rot = A::T(0.0, 0.5, 0.0) * R * A::T(0.0, -0.5, 0.0);

    // ---- texture: DyeColor id 0-15, 16 / 缺省 = 无色 ----
    static const char* kColors[16] = { "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                                       "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
    const int color = static_cast<int>(NumOr(e, "Color", 16));
    std::string texPath = "entity/shulker/shulker";
    if (color >= 0 && color <= 15) {
        const std::string colored = std::string("entity/shulker/shulker_") + kColors[color];
        if (TextureExists("minecraft", colored)) texPath = colored;
    }
    OutObject& obj = ctx.NewObject("minecraft:shulker");
    EmitTextured(ctx, obj, model, MobBase(pl, rot, 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// ---------- witch ----------
// WitchModel (VillagerModel 身体 + 帽子 + 鼻子) / WitchRenderer: 无 renderer scale (26.x 根节点已烘焙 0.9375)。
// setupAnim at age 0: 鼻子 zRot = cos(0)*2.5°; 手持物品时鼻子下垂 (setPos(0,1,-1.5), xRot -0.9)。
// 手持物品 (WitchItemLayer) 暂未渲染。
void RenderWitch(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:witch#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const bool holding = !ReadEquipment(e).mainhand.empty();
    if (int nose = model.Find("nose"); nose >= 0) {
        float* p = model.Pose(nose);
        p[5] = static_cast<float>(2.5 * kPi / 180.0);
        if (holding) { p[0] = 0.0f; p[1] = 1.0f; p[2] = -1.5f; p[3] = -0.9f; }
    }
    const std::string texPath = PickTex({ "entity/witch/witch", "entity/witch" });  // 1.21.1: entity/witch.png
    OutObject& obj = ctx.NewObject("minecraft:witch");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}


// ---- end batch2 C ----

// ==== batch3 A renderers (insert below) ====

// ---------- illagers (pillager / vindicator / evoker / illusioner) ----------
// IllagerModel.setupAnim 静止相位 (walkAnimationPos/Speed = 0): arms 与 legs/手臂 的摆动项为 0, arms 位姿已在 dump 的初始姿态里。
// 手臂姿态 (AbstractIllager.getArmPose 在 NBT 里没有对应状态时的值):
//   pillager: 手里有弩 (主手/副手) -> CROSSBOW_HOLD (animateCrossbowHold(right, left, head, true)); 否则 NEUTRAL (双臂下垂)
//   vindicator / evoker / illusioner: 默认 CROSSED (双手交叉, 只显示 arms, 隐藏 left_arm / right_arm)
// IllagerModel 构造函数会把 hat 设为不可见; IllusionerRenderer 再把 hat 打开。
// 手持物品: 只有 pillager / vindicator / evoker 有 ItemInHandLayer (heldItems); illusioner 与 IllagerRenderer 本身只有 CustomHeadLayer。
// 盔甲层 / 隐身镜像分身 不画。
// 旧版 IllagerRenderer.scale(0.9375); 26.x 的 root 已烘焙 0.9375, renderer 不再缩放。
enum class IllagerArms { Neutral, Crossed, CrossbowHold };
void RenderIllager(Ctx& ctx, const NbtTagPtr& e, const char* kind, const char* objId, IllagerArms arms,
                   bool hatVisible, const char* texPath, bool heldItems = false) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, std::string("minecraft:") + kind + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float pitch = HeadPitch(pl);
    model.SetRot("head", pitch, 0.0f, 0.0f);
    model.SetVisible("hat", hatVisible);
    if (arms == IllagerArms::CrossbowHold) {
        // AnimationUtils.animateCrossbowHold(rightArm, leftArm, head, true); head.yRot = 0 (与其它生物一致)
        model.SetRot("right_arm", static_cast<float>(-kPi / 2) + pitch + 0.1f, -0.3f, 0.0f);
        model.SetRot("left_arm", -1.5f + pitch, 0.6f, 0.0f);
    }
    const bool crossed = arms == IllagerArms::Crossed;
    model.SetVisible("arms", crossed);
    model.SetVisible("left_arm", !crossed);
    model.SetVisible("right_arm", !crossed);

    const double rendererScale = ctx.modern ? 1.0 : 0.9375;
    EntityAffine base = LivingBase(pl, 1.62, rendererScale);
    OutObject& obj = ctx.NewObject(objId);
    EmitTextured(ctx, obj, model, base, nullptr, std::string("minecraft:") + texPath, ctx.Texture("minecraft", texPath));
    // IllagerModel.translateToHand = root + arm, the same as RenderHeldItems' humanoid transform
    if (heldItems) RenderHeldItems(ctx, obj, model, ReadEquipment(e), base, false, false);
}

// PillagerRenderer / PillagerModel(IllagerModel): 手持弩时为 CROSSBOW_HOLD, 否则 NEUTRAL
void RenderPillager(Ctx& ctx, const NbtTagPtr& e) {
    const Equipment eq = ReadEquipment(e);
    const bool crossbow = eq.mainhand.id == "minecraft:crossbow" || eq.offhand.id == "minecraft:crossbow";
    RenderIllager(ctx, e, "pillager", "minecraft:pillager",
                  crossbow ? IllagerArms::CrossbowHold : IllagerArms::Neutral, false, "entity/illager/pillager", true);
}

// VindicatorRenderer: 默认 CROSSED (非攻击状态, 攻击状态不在 NBT 里)
void RenderVindicator(Ctx& ctx, const NbtTagPtr& e) {
    RenderIllager(ctx, e, "vindicator", "minecraft:vindicator", IllagerArms::Crossed, false, "entity/illager/vindicator", true);
}

// EvokerRenderer: SpellcasterIllager.getArmPose 默认 CROSSED (施法状态不在 NBT 里)
void RenderEvoker(Ctx& ctx, const NbtTagPtr& e) {
    RenderIllager(ctx, e, "evoker", "minecraft:evoker", IllagerArms::Crossed, false, "entity/illager/evoker", true);
}

// IllusionerRenderer: hat 可见; 默认 CROSSED. 只画本体 (isInvisible 的镜像分身不画)
void RenderIllusioner(Ctx& ctx, const NbtTagPtr& e) {
    RenderIllager(ctx, e, "illusioner", "minecraft:illusioner", IllagerArms::Crossed, true, "entity/illager/illusioner");
}

// ---------- wandering trader ----------
// WanderingTraderRenderer + VillagerModel (与 RenderVillagerLike 的 villager 同一套体型).
// 旧版 WanderingTraderRenderer.scale(0.9375); 26.x 根节点已烘焙, 不再缩放.
// 贴图: 1.21.1 entity/wandering_trader.png, 26.x entity/wandering_trader/wandering_trader.png
void RenderWanderingTrader(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:wandering_trader#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    model.SetRot("head", HeadPitch(pl), 0.0f, 0.0f);
    const double rendererScale = ctx.modern ? 1.0 : 0.9375;
    EntityAffine base = LivingBase(pl, 1.62, rendererScale);
    const std::string texPath = PickTex({ "entity/wandering_trader/wandering_trader", "entity/wandering_trader" });
    OutObject& obj = ctx.NewObject("minecraft:wandering_trader");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}

// ---- end batch3 A ----

// ==== batch3 B renderers (insert below) ====
// ---------- B: piglin family ----------
// PiglinModel / ZombifiedPiglinModel at age 0, standing still, arm pose DEFAULT (PiglinArmPose is not saved):
// ears keep their static angle plus the idle wobble cos(0)*0.08, so left = -ang - 0.08, right = ang + 0.08,
// ang = 30 deg adult, 5 deg for the 26.x baby layer (1.21.1 has no baby layer, so babies keep 30 deg there).
// Piglin / brute arms: HumanoidModel idle. Zombified piglin arms: animateZombieArms, raised at -pi/2.25,
// or -pi/1.5 while angry (AngerTime / anger_end_time, read by IsAngry).
void BPiglinPose(EntityModel& model, bool modernBaby, bool zombified, bool angry) {
    const float pi = static_cast<float>(kPi);
    const float ang = static_cast<float>((modernBaby ? 5.0 : 30.0) * kPi / 180.0);
    model.SetRot("left_ear", 0.0f, 0.0f, -ang - 0.08f);
    model.SetRot("right_ear", 0.0f, 0.0f, ang + 0.08f);
    if (!zombified) {
        PoseSkeletonIdle(model);
        return;
    }
    const float armX = angry ? -pi / 1.5f : -pi / 2.25f;
    model.SetRot("right_arm", armX, -0.1f, 0.05f);
    model.SetRot("left_arm", armX, 0.1f, -0.05f);
    model.SetRot("right_leg", 0.0f, 0.005f, 0.005f);
    model.SetRot("left_leg", 0.0f, -0.005f, -0.005f);
}

// Shared piglin path. Legacy babies have no layer and use the zombie-style legacy baby scaling.
// 26.x babies use <kind>_baby#main and the piglin_baby / zombified_piglin_baby texture.
// Held items are not drawn; armor and the worn head item follow the zombie path.
void BRenderPiglinCommon(Ctx& ctx, const NbtTagPtr& e, const std::string& kind, const std::string& adultTex,
                         const std::string& babyTex, bool zombified) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    bool modernBaby = false;
    std::string layer = "minecraft:" + kind + "#main";
    if (ctx.modern && baby) {
        const std::string babyLayer = "minecraft:" + kind + "_baby#main";
        if (EntityModel::Exists(fam, babyLayer)) {
            layer = babyLayer;
            modernBaby = true;
        }
    }
    EntityModel model(fam, layer);
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    BPiglinPose(model, modernBaby, zombified, zombified && IsAngry(e));
    const double eye = 1.74 * (baby ? 0.5 : 1.0);
    EntityAffine base = LivingBase(pl, eye, 1.0);
    TopPre pre;
    if (!ctx.modern && baby) pre = LegacyBabyPre(model);
    const bool young = ctx.modern ? modernBaby : baby;
    const std::string texPath = (modernBaby && !babyTex.empty()) ? babyTex : adultTex;
    OutObject& obj = ctx.NewObject("minecraft:" + kind);
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
    Equipment eq = ReadEquipment(e);
    RenderArmor(ctx, obj, "minecraft:" + kind, model, eq, base, pre, young, false);
    // ItemInHandLayer. A 1.21.1 baby is scaled by pre, which RenderHeldItems does not apply, so it is skipped.
    if (ctx.modern || !baby) RenderHeldItems(ctx, obj, model, eq, base, false, ctx.modern && young);
    RenderHeadItem(ctx, obj, model, eq, base, young, false, 0.0);
}

void RenderPiglin(Ctx& ctx, const NbtTagPtr& e) {
    BRenderPiglinCommon(ctx, e, "piglin", "entity/piglin/piglin", "entity/piglin/piglin_baby", false);
}

// Brute: same adult model (no baby layer), brute texture.
void RenderPiglinBrute(Ctx& ctx, const NbtTagPtr& e) {
    BRenderPiglinCommon(ctx, e, "piglin_brute", "entity/piglin/piglin_brute", "", false);
}

// Zombified piglin: zombie-style arms (see BPiglinPose), the zombified_piglin textures.
void RenderZombifiedPiglin(Ctx& ctx, const NbtTagPtr& e) {
    BRenderPiglinCommon(ctx, e, "zombified_piglin", "entity/piglin/zombified_piglin",
                        "entity/piglin/zombified_piglin_baby", true);
}

// BatModel at animation time 0: BatAnimation.BAT_RESTING (BatFlags bit0) or BAT_FLYING, same keyframes in
// 1.21.1 and 26.3. The keyframe values are offsets on the dump's rest pose (posVec negates y).
// Resting head yaw is not saved, so it stays 0. Bats have no head pitch, and the 0.25 in the renderer is the
// shadow radius, so the scale is 1.0.
// GiantMobRenderer: the zombie model at scale 6 with the zombie texture. The legacy family applies the 6 as the
// renderer scale; 26.x already has it in the giant layer root, so the renderer scale stays 1.
void RenderGiant(Ctx& ctx, const NbtTagPtr& e) {
    RenderHumanoidMob(ctx, e, "giant", "entity/zombie/zombie", 10.44, true, 6.0);
}

void RenderBat(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:bat#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float pi = static_cast<float>(kPi);
    const float deg = static_cast<float>(kPi / 180.0);
    const bool resting = (static_cast<int>(NumOr(e, "BatFlags", 0)) & 1) != 0;
    if (resting) {
        model.AddRot("head", pi, 0.0f, 0.0f);
        AddPos(model, "head", 0.0f, -0.5f, 0.0f);
        model.AddRot("body", pi, 0.0f, 0.0f);
        AddPos(model, "body", 0.0f, -0.5f, 0.0f);
        model.AddRot("right_wing", 0.0f, -10.0f * deg, 0.0f);
        AddPos(model, "right_wing", 0.0f, 0.0f, 1.0f);
        model.AddRot("right_wing_tip", 0.0f, -120.0f * deg, 0.0f);
        model.AddRot("left_wing", 0.0f, 10.0f * deg, 0.0f);
        AddPos(model, "left_wing", 0.0f, 0.0f, 1.0f);
        model.AddRot("left_wing_tip", 0.0f, 120.0f * deg, 0.0f);
    } else {
        model.AddRot("body", 40.0f * deg, 0.0f, 0.0f);
        model.AddRot("feet", 10.0f * deg, 0.0f, 0.0f);
        model.AddRot("right_wing", 0.0f, 85.0f * deg, 0.0f);
        model.AddRot("right_wing_tip", 0.0f, 10.5f * deg, 0.0f);
        model.AddRot("left_wing", 0.0f, -85.0f * deg, 0.0f);
        model.AddRot("left_wing_tip", 0.0f, -10.5f * deg, 0.0f);
    }
    const std::string texPath = PickTex({ "entity/bat/bat", "entity/bat" });
    OutObject& obj = ctx.NewObject("minecraft:bat");
    EmitTextured(ctx, obj, model, MobBase(pl, BodyRot(pl), 1.0), nullptr, "minecraft:" + texPath,
                 ctx.Texture("minecraft", texPath));
}

// ---- end batch3 B ----

// ==== batch3 C renderers (insert below) ====

// ---- batch3 C: frog ----
// FrogModel.setupAnim: 关键帧 (跳跃/叫/舌头/游泳/行走) 在没有启动时不改姿态, 静止就是烘焙姿态。
// croaking_body 只在叫动画中可见, 静止时隐藏。头部不做朝向 (原版 setupAnim 不读 head)。
// 贴图按 variant (temperate/warm/cold): 26.x entity/frog/frog_<v>, 1.21.1 entity/frog/<v>_frog。
void RenderFrog(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:frog#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    model.SetVisible("croaking_body", false);
    const std::string v = ClimateVariant(e);
    std::string tex = ctx.modern ? "entity/frog/frog_" + v : "entity/frog/" + v + "_frog";
    const std::string alt = ctx.modern ? "entity/frog/" + v + "_frog" : "entity/frog/frog_" + v;
    if (!TextureExists("minecraft", tex) && TextureExists("minecraft", alt)) tex = alt;
    OutObject& obj = ctx.NewObject("minecraft:frog");
    EmitTextured(ctx, obj, model, LivingBase(pl, 0.3, 1.0), nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}
// ---- end batch3 C frog ----
// ---- batch3 C: armadillo ----
// ArmadilloModel.setupAnim (未缩球): cube 隐藏; 头俯仰 clamp 到 [-22.5°, 25°], 头 yaw 取 0 (NBT 没有头部朝向)。
// 行走 / 滚动 / 偷看关键帧未启动, 静止即烘焙姿态。缩球状态 (state NBT) 未做, 一律按未缩球渲染。
// 26.x 幼体有独立 layer 和贴图 (armadillo_baby); 1.21.1 没有幼体, 按成年体渲染。
void RenderArmadillo(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "armadillo", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    // isHidingInShell = state.shouldHideInShell(ticksInState): scared always, unrolling while ticks < 26; ticks are
    // runtime and a loaded entity starts at 0. Rolled model: body's own cubes skipped (children still draw), hind legs
    // and tail hidden, cube shown; front legs and head keep drawing. Head look is not applied while rolled.
    const std::string state = Str(e, "state");
    const bool rolled = state == "scared" || state == "unrolling";
    if (!rolled) {
        model.SetVisible("cube", false);
        model.SetRot("head", static_cast<float>(std::clamp(pl.pitch, -22.5, 25.0) * kPi / 180.0), 0.0f, 0.0f);
    }
    const bool modernBaby = ctx.modern && baby;
    const std::string texPath = modernBaby ? "entity/armadillo/armadillo_baby"
                                           : PickTex({ "entity/armadillo/armadillo", "entity/armadillo" });
    OutObject& obj = ctx.NewObject("minecraft:armadillo");
    const TopPre pre = (!ctx.modern && baby) ? LegacyWholeBabyPre(0.6f, 16.02f) : nullptr;  // 1.21.1 ArmadilloModel
    const std::string tex = ctx.Texture("minecraft", texPath);
    if (rolled && !tex.empty()) {
        const int mat = ctx.Material("minecraft:" + texPath, tex);
        model.Emit(obj.mesh, LivingBase(pl, 0.4, 1.0), [&](int part) {
            const std::string& n = model.Name(part);
            return (n == "body" || n == "tail" || n == "left_hind_leg" || n == "right_hind_leg") ? -1 : mat;
        }, pre);
        return;
    }
    EmitTextured(ctx, obj, model, LivingBase(pl, 0.4, 1.0), pre, "minecraft:" + texPath, tex);
}
// ---- end batch3 C armadillo ----
// ---- batch3 C: axolotl ----
// 静止姿态 (年龄 0, 无速度, 无头部 yaw; 水里 = 脚下方块是水, 否则视为趴在陆地上)。
// 26.x 成年体 AdultAxolotlModel.setupAnim: 水里 -> 悬停 (factor 1); 陆地 -> laystill (factor 1); 再把左腿镜像到右腿。
// 26.x 幼体 BabyAxolotlModel: 静止动画全是关键帧 (idle/swim/walk 未启动), 用烘焙姿态。
// 1.21.1 AxolotlModel 与 26.x 常数相同, 但每帧先把姿态重置再按 5% 插值, 导出无法复现; 用烘焙姿态 +
// 空映射分支的 body 俯仰 (body = (headPitch, netHeadYaw, 0)), 幼体同样 (AgeableListModel 参数见 kC3AgeAxolotl)。
const AgeableParams kC3AgeAxolotl { true, 8.0f, 3.35f, 2.0f, 2.0f, 24.0f, { "head" } };

// AdultAxolotlModel.setupWaterHoveringAnimation, 年龄 0, factor 1 (sin 0 = 0, cos 0 = 1)
void C3AxolotlHover(EntityModel& m) {
    m.AddRot("body", -0.075f, 0.0f, 0.0f);
    m.AddRot("head", 0.075f, 0.0f, 0.0f);
    m.AddRot("top_gills", 0.2f, 0.0f, 0.0f);
    m.AddRot("left_gills", 0.0f, -0.49f, 0.0f);
    m.AddRot("right_gills", 0.0f, 0.49f, 0.0f);
    m.AddRot("left_hind_leg", static_cast<float>(3.0 * kPi / 4.0 - 0.11), 0.47123894f, 1.7278761f);
    m.AddRot("left_front_leg", static_cast<float>(kPi / 4.0 - 0.2), 2.042035f, 0.0f);
    m.AddRot("tail", 0.0f, 0.5f, 0.0f);
}
// AdultAxolotlModel.setupLayStillOnGroundAnimation, 年龄 0, factor 1 (movement = 0, movement2 = 1)
void C3AxolotlLayStill(EntityModel& m) {
    m.AddRot("head", 0.0f, 0.0f, -0.2f);
    m.AddRot("tail", 0.0f, -0.1f, 0.0f);
    m.AddRot("top_gills", 0.65f, 0.0f, 0.0f);
    m.AddRot("left_gills", 0.0f, -0.65f, 0.0f);
    m.AddRot("right_gills", 0.0f, 0.65f, 0.0f);
    m.AddRot("left_hind_leg", 1.1f, 1.0f, 0.0f);
    m.AddRot("left_front_leg", 0.8f, 2.3f, -0.5f);
}
// applyMirrorLegRotations (factor 1): right += (x, -y, -z) of left
void C3AxolotlMirror(EntityModel& m, const char* left, const char* right) {
    const int l = m.Find(left), r = m.Find(right);
    if (l < 0 || r < 0) return;
    const float* a = m.Pose(l);
    float* b = m.Pose(r);
    b[3] += a[3];
    b[4] += -a[4];
    b[5] += -a[5];
}

void RenderAxolotl(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "axolotl", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const bool modernBaby = ctx.modern && baby;
    if (ctx.modern && !baby) {
        if (InWater(ctx, pl)) C3AxolotlHover(model);
        else C3AxolotlLayStill(model);
        C3AxolotlMirror(model, "left_hind_leg", "right_hind_leg");
        C3AxolotlMirror(model, "left_front_leg", "right_front_leg");
    } else if (!ctx.modern) {
        model.SetRot("body", static_cast<float>(pl.pitch * kPi / 180.0), 0.0f, 0.0f);
    }
    // Variant: 0 lucy, 1 wild, 2 gold, 3 cyan, 4 blue
    static const char* kNames[] = { "lucy", "wild", "gold", "cyan", "blue" };
    const int variant = std::clamp(static_cast<int>(NumOr(e, "Variant", 0)), 0, 4);
    const std::string texPath = std::string("entity/axolotl/axolotl_") + kNames[variant] + (modernBaby ? "_baby" : "");
    TopPre pre = (!ctx.modern && baby) ? LegacyAgeablePre(kC3AgeAxolotl) : nullptr;
    OutObject& obj = ctx.NewObject("minecraft:axolotl");
    EmitTextured(ctx, obj, model, LivingBase(pl, 0.3, 1.0), pre, "minecraft:" + texPath, ctx.Texture("minecraft", texPath));
}
// ---- end batch3 C axolotl ----
// ---- batch3 C: tropical fish ----
// Variant 位域: shape = v & 0xFF (0 small, 1 large), pattern = (v >> 8) & 0xFF (花纹下标 0-5 -> 1-6),
// baseColor = (v >> 16) & 0xFF, patternColor = (v >> 24) & 0xFF (DyeColor id)。
// TropicalFishRenderer: 底层 tropical_a (small) / tropical_b (large) 乘 baseColor;
// TropicalFishPatternLayer: tropical_{a,b}_pattern_<n> 乘 patternColor, 几何是 #pattern layer。
// 静止: setupAnim 的 tail = -amp * 0.45 * sin(0) = 0, bodyZRot = 0, 即烘焙姿态。
// 离水侧躺 (setupRotations: translate(0.2, 0.1, 0) 再绕 Z 转 90°), 同 RenderCod。
void RenderTropicalFish(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(NumOr(e, "Variant", 0)));
    const bool large = (v & 0xFFu) == 1;
    const int patternIdx = static_cast<int>((v >> 8) & 0xFFu);
    const int baseColor = static_cast<int>((v >> 16) & 0xFFu);
    const int patternColor = static_cast<int>((v >> 24) & 0xFFu);
    const std::string kind = large ? "tropical_fish_large" : "tropical_fish_small";
    EntityModel model(fam, "minecraft:" + kind + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    A rot = BodyRot(pl);
    if (!InWater(ctx, pl)) rot = rot * A::T(0.2, 0.1, 0.0) * A::RZDeg(90.0);
    const A base = MobBase(pl, rot, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:tropical_fish");
    EmitDyedLayer(ctx, obj, model, base, nullptr, large ? "entity/fish/tropical_b" : "entity/fish/tropical_a",
                  static_cast<uint32_t>(DyeRgb(baseColor)));
    EntityModel pattern(fam, "minecraft:" + kind + "#pattern");
    if (!pattern.Valid()) return;
    pattern.CopyPoseFrom(model, { "body", "tail", "left_fin", "right_fin", "top_fin" }, true);
    const int n = std::clamp(patternIdx + 1, 1, 6);
    const std::string patTex = std::string(large ? "entity/fish/tropical_b_pattern_" : "entity/fish/tropical_a_pattern_") +
                               std::to_string(n);
    EmitDyedLayer(ctx, obj, pattern, base, nullptr, patTex, static_cast<uint32_t>(DyeRgb(patternColor)));
}
// ---- end batch3 C tropical fish ----
// ---- end batch3 C ----

// ==== batch3 D renderers (insert below) ====

// ---------- creaking (batch3 D, 26.x only) ----------
// CreakingModel.setupAnim at rest: head.xRot = pitch, head.yRot = 0. The walk step has no effect at speed 0.
// The eyes glow comes from runtime state (isActive / tearing down), which is not saved, so the eyes layer is not drawn.
// CreakingRenderer's 0.6 is the shadow radius.
void RenderCreaking(Ctx& ctx, const NbtTagPtr& e) {
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:creaking#main");  // 1.21.1 has no creaking
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:creaking");
    const std::string tex = "entity/creaking/creaking";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}


// Emit only the listed parts' own polys (a child part is skipped unless listed). Visibility is not used,
// because Emit hides children of hidden parents.
void BatchD_EmitParts(Ctx& ctx, OutObject& obj, EntityModel& model, const EntityAffine& base,
                      const std::string& texPath, const std::vector<std::string>& parts) {
    const std::string tex = ctx.Texture("minecraft", texPath);
    if (tex.empty()) return;
    const int mat = ctx.Material("minecraft:" + texPath, tex);
    model.Emit(obj.mesh, base, [&](int part) {
        const std::string& n = model.Name(part);
        return std::find(parts.begin(), parts.end(), n) != parts.end() ? mat : -1;
    }, nullptr);
}

// ---------- warden (batch3 D) ----------
// WardenModel rest pose (age 0, walk speed 0): head look (pitch, yaw 0) + idle head.zRot 0.06. The walk step zeroes
// body, then the idle step sets body.xRot 0.025. Arms, legs and tendrils keep the dump rest pose. Keyframes are not applied.
// Glow layers: bioluminescent (alpha 1.0) is drawn. Pulsating spots (alpha max(0, 0.25 cos(age)), runtime), heart and
// tendril glows (runtime values) are not drawn.
void PoseWardenRest(EntityModel& model, const Placement& pl) {
    model.SetRot("head", HeadPitch(pl), 0.0f, 0.06f);
    model.SetRot("body", 0.025f, 0.0f, 0.0f);
}
void RenderWarden(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:warden#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    PoseWardenRest(model, pl);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);  // WardenRenderer's 0.9 is the shadow radius
    OutObject& obj = ctx.NewObject("minecraft:warden");
    const std::string tex = "entity/warden/warden";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
    const std::string glow = "entity/warden/warden_bioluminescent_layer";
    if (ctx.modern) {
        EntityModel bio(fam, "minecraft:warden#bioluminescent");
        if (bio.Valid()) {
            PoseWardenRest(bio, pl);
            EmitTextured(ctx, obj, bio, base, nullptr, "minecraft:" + glow, ctx.Texture("minecraft", glow));
        }
    } else {  // 1.21.1: the same body model, bioluminescent = head, arms, legs
        BatchD_EmitParts(ctx, obj, model, base, glow, { "head", "left_arm", "right_arm", "left_leg", "right_leg" });
    }
}


// ---------- breeze (batch3 D) ----------

// BreezeModel has no head look. 26.x: three layer models (main = head + rods, wind = wind_body, eyes) with the dump
// rest pose; keyframes are not applied. 1.21.1: one model switched per pass (base = head + rods, wind = wind_body
// subtree on breeze_wind#main, eyes = eyes). The 1.21.1 setupAnim at age 0 writes absolute values: head.y 4.25,
// wind_top.x 0.6, wind_mid.z 0.8, wind_bottom.x -0.25. BreezeRenderer's 0.5 is the shadow radius.
void BreezeLegacyRest(EntityModel& model) {
    auto set = [&](const char* p, int axis, float v) { int i = model.Find(p); if (i >= 0) model.Pose(i)[axis] = v; };
    set("head", 1, 4.25f);
    set("wind_top", 0, 0.6f);
    set("wind_mid", 2, 0.8f);
    set("wind_bottom", 0, -0.25f);
}
void RenderBreeze(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel main(fam, "minecraft:breeze#main");
    if (!main.Valid()) return;
    Placement pl = ReadPlacement(e);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:breeze");
    const std::string body = "entity/breeze/breeze", wind = "entity/breeze/breeze_wind", eyes = "entity/breeze/breeze_eyes";
    if (ctx.modern) {
        EmitTextured(ctx, obj, main, base, nullptr, "minecraft:" + body, ctx.Texture("minecraft", body));
        EntityModel windM(fam, "minecraft:breeze#wind");
        if (windM.Valid()) EmitTextured(ctx, obj, windM, base, nullptr, "minecraft:" + wind, ctx.Texture("minecraft", wind));
        EntityModel eyesM(fam, "minecraft:breeze#eyes");
        if (eyesM.Valid()) EmitTextured(ctx, obj, eyesM, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
    } else {
        EntityModel windM(fam, "minecraft:breeze_wind#main");
        BreezeLegacyRest(main);
        BatchD_EmitParts(ctx, obj, main, base, body, { "head", "rods", "rod_1", "rod_2", "rod_3" });
        if (windM.Valid()) {
            BreezeLegacyRest(windM);
            BatchD_EmitParts(ctx, obj, windM, base, wind, { "wind_body", "wind_bottom", "wind_mid", "wind_top" });
        }
        BatchD_EmitParts(ctx, obj, main, base, eyes, { "eyes" });
    }
}


// ---------- sniffer (batch3 D) ----------
// SnifferModel.setupAnim at rest: head.xRot = pitch, head.yRot = netHeadYaw (0). Dig/sniff/walk keyframes use the
// dump rest pose. 26.x snifflet = sniffer_baby + snifflet.png. 1.21.1 has no baby model and no baby scaling.
// SnifferRenderer's 1.1 is the shadow radius.
void RenderSniffer(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "sniffer", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:sniffer");
    const char* babyTex = (ctx.modern && baby) ? "entity/sniffer/snifflet" : nullptr;
    const std::string tex = PickTex({ babyTex, "entity/sniffer/sniffer" });
    const TopPre pre = (!ctx.modern && baby) ? LegacyWholeBabyPre(0.5f, 24.0f) : nullptr;  // 1.21.1 SnifferModel
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}


// ---------- camel (batch3 D) ----------
// CamelModel.setupAnim at rest: applyHeadRotation only (netHeadYaw 0 for NoAI, head pitch clamped to [-25, 45];
// jumpCooldown is runtime, so no lift). Walk/sit/standup/dash keyframes use the dump rest pose.
// 1.21.1: saddle + bridle visible when saddled, reins when saddled and ridden (CamelModel.setupAnim).
// 26.x: camel#saddle layer (CamelSaddleModel) = saddle + bridle, reins only when ridden.
// CamelRenderer's 0.7 is the shadow radius, so there is no renderer scale.
void RenderCamel(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);  // 26.x: camel_baby layer; 1.21.1: adult layer scaled in renderToBuffer
    EntityModel model(fam, MobLayer(ctx, "camel", baby));
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float headX = static_cast<float>(std::clamp(pl.pitch, -25.0, 45.0) * kPi / 180.0);
    model.SetRot("head", headX, 0.0f, 0.0f);
    // CamelModel.setupAnim: CAMEL_SIT_POSE / CAMEL_BABY_SIT_POSE, constant keyframes added to the rest pose (after the
    // head look). isCamelSitting = LastPoseTick < 0 (1.21.1 and 26.x). The transition is assumed finished, as at rest.
    // The 26.x baby uses its own sit pose; 1.21.1 has no baby model, so the adult pose is scaled with the body.
    const bool sitting = NumOr(e, "LastPoseTick", 0) < 0;
    const float deg = static_cast<float>(kPi / 180.0);
    struct SitOff { const char* part; float rx, ry, rz, px, py, pz; };
    static const SitOff kAdultSit[] = {
        { "body", 0.0f, 0.0f, 0.0f, 0.0f, -19.9f, 0.0f },
        { "right_front_leg", -90.0f, 10.0f, 0.0f, 0.0f, -20.6f, 12.0f },
        { "left_front_leg", -90.0f, -10.0f, 0.0f, 0.0f, -20.6f, 12.0f },
        { "left_hind_leg", -90.0f, -15.0f, 0.0f, 1.0f, -20.5f, 5.0f },
        { "right_hind_leg", -90.0f, 15.0f, 0.0f, -1.0f, -20.5f, 5.0f },
        { "tail", 50.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
    };
    static const SitOff kBabySit[] = {
        { "body", 2.5f, 0.0f, 0.0f, 0.0f, -13.25f, 0.0f },
        { "right_front_leg", -90.0f, 10.0f, 0.0f, 0.1f, -11.0f, 6.0f },
        { "left_front_leg", -90.0f, -10.0f, 0.0f, 0.0f, -11.0f, 6.0f },
        { "left_hind_leg", -90.0f, -11.88f, -0.54f, 1.4f, -10.95f, 1.95f },
        { "right_hind_leg", -90.0f, 11.89f, 0.54f, -1.33f, -11.0f, 1.95f },
        { "head", -4.75f, 0.0f, 0.0f, 0.0f, 0.0f, 1.22f },
        { "tail", 52.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },
        { "left_ear", 0.0f, 0.0f, -22.5f, 0.0f, 0.0f, 0.0f },
        { "right_ear", 0.0f, 0.0f, 15.0f, 0.0f, 0.0f, 0.0f },
    };
    auto applySitPose = [&](EntityModel& m, bool babyPose) {
        const SitOff* first = babyPose ? kBabySit : kAdultSit;
        const SitOff* last = first + (babyPose ? sizeof(kBabySit) : sizeof(kAdultSit)) / sizeof(SitOff);
        for (const SitOff* o = first; o != last; ++o) {
            m.AddRot(o->part, o->rx * deg, o->ry * deg, o->rz * deg);
            AddPos(m, o->part, o->px, o->py, o->pz);
        }
    };
    if (sitting) applySitPose(model, ctx.modern && baby);
    const ItemStackInfo saddleItem = SaddleStack(e);
    const bool saddled = !saddleItem.empty();
    const bool ridden = HasPassengers(e);
    if (!ctx.modern) {  // 1.21.1 body-model saddle parts (bridle/reins are children of head)
        model.SetVisible("saddle", saddled);
        model.SetVisible("bridle", saddled);
        model.SetVisible("reins", saddled && ridden);
    }
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:camel");
    const char* babyTex = (ctx.modern && baby) ? "entity/camel/camel_baby" : nullptr;
    const std::string tex = PickTex({ babyTex, "entity/camel/camel" });
    const TopPre pre = (!ctx.modern && baby) ? LegacyWholeBabyPre(0.45f, 29.35f) : nullptr;  // 1.21.1 CamelModel
    EmitTextured(ctx, obj, model, base, pre, "minecraft:" + tex, ctx.Texture("minecraft", tex));

    // 26.x SimpleEquipmentLayer(camel_saddle) with CamelSaddleModel (adult geometry, not drawn for babies here).
    // Only the saddle/bridle/reins parts are emitted, as for the horse saddle, to avoid the transparent body sheet.
    if (ctx.modern && saddled && !baby) {
        const std::string asset = saddleItem.equipAsset.empty() ? "minecraft:saddle" : saddleItem.equipAsset;
        std::vector<ArmorTexLayer> layers = EquipmentTexLayers(saddleItem, asset, "camel_saddle");
        if (layers.empty()) layers.push_back({ "minecraft", "entity/equipment/camel_saddle/saddle", -1 });
        EntityModel saddle(fam, "minecraft:camel#saddle");
        if (saddle.Valid()) {
            saddle.SetRot("head", headX, 0.0f, 0.0f);
            if (sitting) applySitPose(saddle, false);  // CamelSaddleModel extends AdultCamelModel
            const int mat = ArmorMaterial(ctx, layers);
            if (mat >= 0) {
                saddle.Emit(obj.mesh, base, [&](int part) {
                    const std::string& n = saddle.Name(part);
                    return (n == "saddle" || n == "bridle" || (n == "reins" && ridden)) ? mat : -1;
                }, nullptr);
            }
        }
    }
}


// ---- end batch3 D ----

// ==== batch4 A renderers (insert below) ====

// ---------- ender dragon (batch4 A) ----------
// EnderDragonModel at rest. Its pose comes from flight history and flapTime, neither saved: the history is taken as
// samples all equal to the entity's yaw and y (no bend in neck, head or tail), flapTime 0, and the default hovering
// phase (head offset 0). EnderDragonRenderer rotates by -yaw, flips, and drops by 1.501; the eyes layer uses the same
// model. 26.x only: 1.21.1 draws its neck and tail segments inline, not as dump parts, so it is not reproduced.
void RenderEnderDragon(Ctx& ctx, const NbtTagPtr& e) {
    EntityModel model("modern", "minecraft:ender_dragon#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const float flapTime = 0.0f;  // runtime flap phase, not saved
    float bounce = std::sin(flapTime * 2.0f * kPi - 1.0f) + 1.0f;
    bounce = (bounce * bounce + bounce * 2.0f) * 0.05f;
    model.SetPose("root", 0.0f, (bounce - 2.0f) * 16.0f, -48.0f);
    model.SetRot("root", bounce * 2.0f * kPi / 180.0f, 0.0f, 0.0f);
    model.SetRot("jaw", (std::sin(flapTime * 2.0f * kPi) + 1.0f) * 0.2f, 0.0f, 0.0f);
    // neck: each segment starts where the previous one ends; no yaw or roll without history
    const float* n0 = model.Pose(model.Find("neck0"));
    float xx = n0[0], yy = n0[1], zz = n0[2];
    for (int i = 0; i < 5; ++i) {
        const std::string name = "neck" + std::to_string(i);
        const float xRot = std::cos(i * 0.45f + flapTime) * 0.15f;
        model.SetPose(name, xx, yy, zz);
        model.SetRot(name, xRot, 0.0f, 0.0f);
        yy += std::sin(xRot) * 10.0f;
        zz -= std::cos(xRot) * 10.0f;
    }
    model.SetPose("head", xx, yy, zz);
    model.SetRot("head", 0.0f, 0.0f, 0.0f);
    // tail: each segment yaws 180 deg; the pitch sway accumulates along the chain
    const float* t0 = model.Pose(model.Find("tail0"));
    xx = t0[0]; yy = t0[1]; zz = t0[2];
    float tailX = 0.0f;
    for (int i = 0; i < 12; ++i) {
        const std::string name = "tail" + std::to_string(i);
        tailX += std::sin(i * 0.45f + flapTime) * 0.05f;
        model.SetPose(name, xx, yy, zz);
        model.SetRot(name, tailX, kPi, 0.0f);
        yy += std::sin(tailX) * 10.0f;
        zz -= std::cos(kPi) * std::cos(tailX) * 10.0f;
        xx -= std::sin(kPi) * std::cos(tailX) * 10.0f;
    }
    // wings (flap 0)
    const float wingX = 0.125f - std::cos(flapTime) * 0.2f;
    const float wingZ = -(std::sin(flapTime) + 0.125f) * 0.8f;
    const float tipZ = (std::sin(flapTime + 2.0f) + 0.5f) * 0.75f;
    model.SetRot("left_wing", wingX, -0.25f, wingZ);
    model.SetRot("left_wing_tip", 0.0f, 0.0f, tipZ);
    model.SetRot("right_wing", wingX, 0.25f, -wingZ);
    model.SetRot("right_wing_tip", 0.0f, 0.0f, -tipZ);
    // legs (EnderDragonModel.poseLimbs)
    for (const std::string side : { std::string("left_"), std::string("right_") }) {
        model.SetRot(side + "hind_leg", 1.0f + bounce * 0.1f, 0.0f, 0.0f);
        model.SetRot(side + "hind_leg_tip", 0.5f + bounce * 0.1f, 0.0f, 0.0f);
        model.SetRot(side + "hind_foot", 0.75f + bounce * 0.1f, 0.0f, 0.0f);
        model.SetRot(side + "front_leg", 1.3f + bounce * 0.1f, 0.0f, 0.0f);
        model.SetRot(side + "front_leg_tip", -0.5f - bounce * 0.1f, 0.0f, 0.0f);
        model.SetRot(side + "front_foot", 0.75f + bounce * 0.1f, 0.0f, 0.0f);
    }
    const EntityAffine base = EntityAffine::T(pl.x, pl.y, pl.z) * EntityAffine::RYDeg(-pl.yaw) *
                              EntityAffine::T(0.0, 0.0, 1.0) * EntityAffine::S(-1.0, -1.0, 1.0) *
                              EntityAffine::T(0.0, -1.501, 0.0);
    OutObject& obj = ctx.NewObject("minecraft:ender_dragon");
    const std::string tex = "entity/enderdragon/dragon";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
    const std::string eyes = "entity/enderdragon/dragon_eyes";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
}

// ---------- wither (batch4 A) ----------
// WitherBossModel at age 0 (the ribcage/tail sway is cos(0) = 1): ribcage.xRot = 0.115 pi, tail.xRot = 0.365 pi, and the
// tail pivot follows the ribcage. Head rotations are not saved; the center head takes the entity pitch and yaw 0 (facing
// the body, as for other mobs). The side heads' xHeadRots/yHeadRots are runtime state, so they stay at the bake pose.
// WitherBossRenderer: scale 2.0, shrinking by Invul/220 * 0.5 while invulnerable; texture swaps on Invul. The armor
// overlay (EnergySwirlLayer, UV-scrolled, drawn when powered) is not drawn, as for the creeper charge layer.
void RenderWither(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:wither#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const int invul = static_cast<int>(NumOr(e, "Invul", 0));
    const float rendererScale = invul > 0 ? 2.0f - invul / 220.0f * 0.5f : 2.0f;
    const float ribcageX = (0.065f + 0.05f) * kPi;
    const float tailX = (0.265f + 0.1f) * kPi;
    model.SetRot("ribcage", ribcageX, 0.0f, 0.0f);
    model.SetPose("tail", -2.0f, 6.9f + std::cos(ribcageX) * 10.0f, -0.5f + std::sin(ribcageX) * 10.0f);
    model.SetRot("tail", tailX, 0.0f, 0.0f);
    ApplyHeadLook(model, pl, "center_head");
    const EntityAffine base = LivingBase(pl, 1.0, rendererScale);  // eye height only matters for beds
    const bool invulnerable = invul > 0 && (invul > 80 || invul / 5 % 2 != 1);
    OutObject& obj = ctx.NewObject("minecraft:wither");
    const std::string tex = invulnerable ? "entity/wither/wither_invulnerable" : "entity/wither/wither";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- end crystal (batch4 A) ----------
// Rotation matrix for `rad` about the unit axis (ax, ay, az) (Rodrigues).
void AxisAngleMatrix(double rad, double ax, double ay, double az, double r[3][3]) {
    const double c = std::cos(rad), s = std::sin(rad), t = 1.0 - c;
    const double m[3][3] = {
        { c + t * ax * ax, t * ax * ay - s * az, t * ax * az + s * ay },
        { t * ax * ay + s * az, c + t * ay * ay, t * ay * az - s * ax },
        { t * ax * az - s * ay, t * ay * az + s * ax, c + t * az * az } };
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = m[i][j];
}
// Euler angles in ModelPart's ZYX order (R = Rz * Ry * Rx). Vanilla composes these rotations as quaternions.
void AxisAngleToEulerZYX(double rad, double ax, double ay, double az, float& x, float& y, float& z) {
    double r[3][3];
    AxisAngleMatrix(rad, ax, ay, az, r);
    y = static_cast<float>(std::asin(std::clamp(-r[2][0], -1.0, 1.0)));
    x = static_cast<float>(std::atan2(r[2][1], r[2][2]));
    z = static_cast<float>(std::atan2(r[1][0], r[0][0]));
}
// EndCrystalRenderer: scale 2, translate (0, -0.5, 0); base only when ShowBottom. EndCrystalModel.setupAnim at age 0:
// spin is 0, the glass pivot moves by getY(0) * 8 px (getY(0) = -1.1), and the outer glass, inner glass and cube are
// tilted 60 deg about (1, 0, 1)/sqrt2. The beam (BeamTarget) is not drawn.
// 1.21.1 has one glass part drawn at three nested transforms (outer, scale 0.875, scale 0.875 again for the cube).
void RenderEndCrystal(Ctx& ctx, const NbtTagPtr& e) {
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return;
    const float getY0 = (0.5f * 0.5f + 0.5f) * 0.4f - 1.4f;
    const double s45 = std::sqrt(0.5);
    const bool showBottom = NumOr(e, "ShowBottom", 0) != 0;
    const EntityAffine base = EntityAffine::T(pos[0], pos[1], pos[2]) * EntityAffine::S(2.0) *
                              EntityAffine::T(0.0, -0.5, 0.0);
    const std::string tex = "entity/end_crystal/end_crystal";
    OutObject& obj = ctx.NewObject("minecraft:end_crystal");
    if (ctx.modern) {  // 26.x: the pivots and tilts live in the part poses
        EntityModel model("modern", "minecraft:end_crystal#main");
        if (!model.Valid()) return;
        AddPos(model, "outer_glass", 0.0f, getY0 * 8.0f, 0.0f);
        float rx = 0.0f, ry = 0.0f, rz = 0.0f;
        AxisAngleToEulerZYX(kPi / 3.0, s45, 0.0, s45, rx, ry, rz);
        for (const char* part : { "outer_glass", "inner_glass", "cube" }) model.SetRot(part, rx, ry, rz);
        model.SetVisible("base", showBottom);
        EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
        return;
    }
    EntityModel model("legacy", "minecraft:end_crystal#main");  // glass, cube and base all have zero poses
    if (!model.Valid()) return;
    double t[3][3];
    AxisAngleMatrix(kPi / 3.0, s45, 0.0, s45, t);
    EntityAffine tilt;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) tilt.m[i][j] = t[i][j];
    const EntityAffine outer = base * EntityAffine::T(0.0, 1.5 + getY0 / 2.0, 0.0) * tilt;
    const EntityAffine inner = outer * EntityAffine::S(0.875) * tilt;
    const EntityAffine cube = inner * EntityAffine::S(0.875) * tilt;
    BatchD_EmitParts(ctx, obj, model, outer, tex, { "glass" });
    BatchD_EmitParts(ctx, obj, model, inner, tex, { "glass" });
    BatchD_EmitParts(ctx, obj, model, cube, tex, { "cube" });
    if (showBottom) BatchD_EmitParts(ctx, obj, model, base, tex, { "base" });
}

// ---- end batch4 A ----

// ==== batch4 B renderers (insert below) ====
// ---------- happy_ghast (batch4 B, 26.x only) ----------
// HappyGhastModel.setupAnim at rest: GhastModel.animateTentacles at age 0 (as RenderGhast). A body item (harness) scales
// body to 0.9375, which the tentacles inherit. The baby layer is baked with BABY_TRANSFORMER.
// Harness (SimpleEquipmentLayer HAPPY_GHAST_BODY): goggles follow isRidden (passengers) via y / xRot.
// Ropes are drawn only while the ghast is a leash holder (runtime state, not saved on this entity), so they are skipped.
// HappyGhastRenderer's 2.0 is the shadow radius, so there is no renderer scale.
void RenderHappyGhast(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "happy_ghast", baby));
    if (!model.Valid()) return;  // 1.21.1 has no happy ghast
    for (int i = 0; i < 9; ++i)
        SetXRot(model, ("tentacle" + std::to_string(i)).c_str(), 0.2f * std::sin(static_cast<float>(i)) + 0.4f);
    const ItemStackInfo bodyItem = BodyArmorStack(e);
    if (!bodyItem.empty()) {
        const int body = model.Find("body");
        if (body >= 0) { float* q = model.Pose(body); q[6] = q[7] = q[8] = 0.9375f; }
    }
    Placement pl = ReadPlacement(e);
    const EntityAffine base = MobBase(pl, BodyRot(pl), 1.0);
    OutObject& obj = ctx.NewObject("minecraft:happy_ghast");
    const std::string tex = baby ? "entity/ghast/happy_ghast_baby" : "entity/ghast/happy_ghast";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));

    // Harness: only when the saved body item resolves to a happy_ghast_body equipment layer.
    const std::string asset = bodyItem.equipAsset;
    const std::vector<ArmorTexLayer> layers = asset.empty() ? std::vector<ArmorTexLayer>()
                                                            : EquipmentTexLayers(bodyItem, asset, "happy_ghast_body");
    const int mat = ArmorMaterial(ctx, layers);
    if (mat < 0) return;
    EntityModel harness(fam, baby ? "minecraft:happy_ghast_baby_harness#main" : "minecraft:happy_ghast_harness#main");
    if (!harness.Valid()) return;
    const bool ridden = HasPassengers(e);
    const int goggles = harness.Find("goggles");
    if (goggles >= 0) {
        float* q = harness.Pose(goggles);
        q[1] = ridden ? 14.0f : 9.0f;
        q[3] = ridden ? 0.0f : -0.7854f;
    }
    harness.Emit(obj.mesh, base, [mat](int) { return mat; }, nullptr);
}

// ---------- copper_golem (batch4 B, 26.x only) ----------
// CopperGolemModel.setupAnim at rest: head pitch from the look, yaw 0. Walk, idle and interaction keyframes are not
// applied, so the dump rest pose is used. The state (IDLE/STAR/SITTING/RUNNING) is runtime and only changes the
// held-item pose, so the standing model is used. weather_state picks the body and eyes texture.
// The eyes layer is drawn with the full model and the eyes texture (vanilla COPPER_GOLEM_EYES is an empty layer).
// The antenna block, held items and the head layer are not drawn. CopperGolemRenderer's 0.5 is the shadow radius.
void RenderCopperGolem(Ctx& ctx, const NbtTagPtr& e) {
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:copper_golem#main");
    if (!model.Valid()) return;  // 1.21.1 has no copper golem
    Placement pl = ReadPlacement(e);
    ApplyHeadLook(model, pl);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    const std::string weather = Str(e, "weather_state");  // WeatheringCopper.WeatherState serialized name
    const std::string suffix = (weather == "exposed" || weather == "weathered" || weather == "oxidized") ? "_" + weather : "";
    OutObject& obj = ctx.NewObject("minecraft:copper_golem");
    const std::string tex = "entity/copper_golem/copper_golem" + suffix;
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
    const std::string eyes = "entity/copper_golem/copper_golem_eyes" + suffix;
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + eyes, ctx.Texture("minecraft", eyes));
}

// ---------- nautilus / zombie_nautilus (batch4 B, 26.x only) ----------
// NautilusModel.setupAnim at rest: body yRot 0 and body xRot = pitch clamped to [-10, 10] (netHeadYaw 0 for NoAI).
// The swim keyframes run at walk speed + 0.2 at runtime, so the dump rest pose is used.
// Body armor and saddle are not drawn: both layers are full-body geometry, so emitting them would redraw the whole body.
// ZombieNautilusCoralModel hides the corals while body armor is worn (the armor itself is not drawn).
// Nautilus and ZombieNautilusRenderer's 0.7 is the shadow radius, so there is no renderer scale.
void PoseNautilusRest(EntityModel& model, const Placement& pl, bool armored) {
    const float x = static_cast<float>(std::clamp(pl.pitch, -10.0, 10.0) * kPi / 180.0);
    model.SetRot("body", x, 0.0f, 0.0f);
    if (model.Find("corals") >= 0) model.SetVisible("corals", !armored);
}
void RenderNautilus(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, MobLayer(ctx, "nautilus", baby));
    if (!model.Valid()) return;  // 1.21.1 has no nautilus
    Placement pl = ReadPlacement(e);
    PoseNautilusRest(model, pl, false);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:nautilus");
    const std::string tex = baby ? "entity/nautilus/nautilus_baby" : "entity/nautilus/nautilus";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}
// ZombieNautilusVariant: "warm" uses ZombieNautilusCoralModel and zombie_nautilus_coral.png, temperate (default) the normal model.
void RenderZombieNautilus(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    std::string variant = SplitId(Str(e, "variant")).second;
    if (variant.empty()) variant = "temperate";
    const bool warm = variant == "warm";
    EntityModel model(fam, warm ? "minecraft:zombie_nautilus_coral#main" : "minecraft:zombie_nautilus#main");
    if (!model.Valid()) return;  // 1.21.1 has no zombie nautilus
    Placement pl = ReadPlacement(e);
    PoseNautilusRest(model, pl, !BodyArmorStack(e).empty());
    const EntityAffine base = LivingBase(pl, 1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:zombie_nautilus");
    const std::string tex = warm ? "entity/nautilus/zombie_nautilus_coral" : "entity/nautilus/zombie_nautilus";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- parched (batch4 B, 26.x only) ----------
// Parched uses the skeleton model and AbstractSkeletonRenderer with parched.png and no clothing overlay, so the skeleton
// path is used. The parched#outer layer is not drawn because the renderer adds no overlay. 1.74 is the skeleton eye height
// (used only for the sleeping offset).
void RenderParched(Ctx& ctx, const NbtTagPtr& e) { RenderHumanoidMob(ctx, e, "parched", "entity/skeleton/parched", 1.74, false); }

// ---------- sulfur_cube (batch4 B, 26.x only) ----------
// SulfurCubeModel has no setupAnim, so the dump rest pose is used. SulfurCubeRenderer.scale at rest, applied between the
// flip and the model draw (like the renderer scale slot of LivingBase): downscaleSlightly (0.999, y +0.001), size from
// applySizeAndSquish with squish 0, then adult 0.5 / baby 1.0 and y offset 0.98 / 1.24 minus one pixel. Squish and fuse
// are runtime and taken as 0. Size is NBT "Size" + 1. The chain is conjugated around the -1.501 model offset.
// Inner layer is drawn only when no body item is carried (a carried block or skull replaces it and is not drawn).
void RenderSulfurCube(Ctx& ctx, const NbtTagPtr& e) {
    const std::string fam = ctx.modern ? "modern" : "legacy";
    const bool baby = IsBaby(e);
    EntityModel model(fam, baby ? "minecraft:sulfur_cube_small#main" : "minecraft:sulfur_cube#main");
    if (!model.Valid()) return;  // 1.21.1 has no sulfur cube
    Placement pl = ReadPlacement(e);
    using A = EntityAffine;
    const double size = NumOr(e, "Size", 0) + 1;
    const A chain = A::S(0.999) * A::T(0.0, 0.001, 0.0) * A::S(size) * A::S(baby ? 1.0 : 0.5) *
                    A::T(0.0, (baby ? 1.24 : 0.98) - 1.0 / 16.0, 0.0);
    const A conj = A::T(0.0, 1.501, 0.0) * chain * A::T(0.0, -1.501, 0.0);
    const EntityAffine base = LivingBase(pl, 1.0, 1.0) * conj;
    OutObject& obj = ctx.NewObject("minecraft:sulfur_cube");
    const std::string tex = baby ? "entity/sulfur_cube/sulfur_cube_outer_small" : "entity/sulfur_cube/sulfur_cube_outer";
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
    if (BodyArmorStack(e).empty()) {
        EntityModel inner(fam, baby ? "minecraft:sulfur_cube_small#inner" : "minecraft:sulfur_cube#inner");
        const std::string itex = baby ? "entity/sulfur_cube/sulfur_cube_inner_small" : "entity/sulfur_cube/sulfur_cube_inner";
        if (inner.Valid()) EmitTextured(ctx, obj, inner, base, nullptr, "minecraft:" + itex, ctx.Texture("minecraft", itex));
    }
}
// ---- end batch4 B ----

// ==== batch4 C renderers (insert below) ====
// ---------- boats and rafts (batch4 C) ----------
// AbstractBoatRenderer.submit at rest (no hurt, no bubble): T(0, 0.375, 0) * RY(180 - yaw) * S(-1, -1, 1) * RY(90).
// Paddles at rest: AbstractBoatModel.setupAnim with rowing time 0 (AbstractBoat.getRowingTime is 0 unless paddling):
// xRot = lerp(0.5, -60deg, -15deg), yRot = lerp((sin 1 + 1) / 2, -45deg, 45deg); the right paddle uses 180deg - yRot.
// Paddle z rotation stays at the baked value. Rafts share the paddle code (RaftModel extends AbstractBoatModel).
// The water patch (BoatRenderer.submitTypeAdditions, outside water only) is not exported.
void BoatPaddlesRest(EntityModel& model) {
    const float xRot = static_cast<float>(-kPi / 3 + 0.5 * (kPi / 4));
    const float yRot = static_cast<float>(-kPi / 4 + (std::sin(1.0) + 1.0) / 2.0 * (kPi / 2));
    const char* names[2] = { "left_paddle", "right_paddle" };
    for (int side = 0; side < 2; ++side) {
        const int i = model.Find(names[side]);
        if (i < 0) continue;
        model.Pose(i)[3] = xRot;
        model.Pose(i)[4] = side == 0 ? yRot : static_cast<float>(kPi) - yRot;
    }
}

// layer = "boat/<wood>" or "chest_boat/<wood>" (raft types use the raft geometry under the same layer name)
void RenderBoat(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& layer) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:" + layer + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    BoatPaddlesRest(model);
    const A base = A::T(pl.x, pl.y, pl.z) * A::T(0.0, 0.375, 0.0) * A::RYDeg(180.0 - pl.yaw) *
                   A::S(-1.0, -1.0, 1.0) * A::RYDeg(90.0);
    OutObject& obj = ctx.NewObject(id);
    const std::string tex = "entity/" + layer;
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}
// 1.21.1 and older: one entity id per boat kind, the wood comes from the "Type" string (unknown -> oak).
void RenderLegacyBoat(Ctx& ctx, const NbtTagPtr& e, const std::string& id) {
    static const char* kTypes[] = { "oak", "spruce", "birch", "jungle", "acacia", "cherry", "dark_oak", "mangrove", "bamboo" };
    std::string type = Str(e, "Type");
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    bool known = false;
    for (const char* t : kTypes) known = known || type == t;
    if (!known) type = "oak";
    RenderBoat(ctx, e, id, (id == "minecraft:chest_boat" ? "chest_boat/" : "boat/") + type);
}
// ---------- minecarts (batch4 C) ----------
// AbstractMinecartRenderer.submit at rest (no hurt, rails not followed): T(0, 0.375, 0) * RY(180 - yaw) * RZ(-pitch),
// then the display block (scale 0.75, T(-0.5, (offset - 8) / 16, 0.5), RY 90) and the cart model with S(-1, -1, 1).
// Rail-following orientation and the random per-entity jitter (+-0.002 block) are not reproduced.
// Display block: DisplayState when present, otherwise the cart type's default block and offset. The block is drawn
// through the block-in-entity item path (as CarriedBlockLayer does); block properties (furnace LIT, chest facing)
// are not applied. Chest blocks have no item model (builtin/entity), so the chest contents are skipped.
std::string MinecartDisplayBlock(const NbtTagPtr& e, const std::string& fallback) {
    auto c = getChildByName(e, "DisplayState");
    if (!c) return fallback;
    std::string raw;
    if (c->type == TagType::STRING) raw = getStringTag(c);
    else if (c->type == TagType::COMPOUND) raw = Str(c, "Name");
    else return fallback;
    const size_t br = raw.find('[');
    if (br != std::string::npos) raw = raw.substr(0, br);
    if (raw.empty() || raw == "air" || raw == "minecraft:air") return {};
    if (raw.find(':') == std::string::npos) raw = "minecraft:" + raw;
    return raw;
}

void RenderMinecart(Ctx& ctx, const NbtTagPtr& e, const std::string& id, const std::string& layer,
                    const std::string& defaultBlock, int defaultOffset) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:" + layer + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const std::string tex = ctx.modern ? "entity/minecart/minecart" : "entity/minecart";  // one texture for all carts
    const A pre = A::T(pl.x, pl.y, pl.z) * A::T(0.0, 0.375, 0.0) * A::RYDeg(180.0 - pl.yaw) * A::RZDeg(-pl.pitch);
    OutObject& obj = ctx.NewObject(id);
    const std::string block = MinecartDisplayBlock(e, defaultBlock);
    const int offset = static_cast<int>(NumOr(e, "DisplayOffset", defaultOffset));
    if (!block.empty()) {
        ItemStackInfo item;
        item.id = block;
        item.count = 1;
        // ItemDisplay::None centres the block on the origin; T(0.5) moves it back to the block-corner origin used by the cart
        const A m = pre * A::S(0.75) * A::T(-0.5, (offset - 8) / 16.0, 0.5) * A::RYDeg(90.0) * A::T(0.5, 0.5, 0.5);
        AppendItem(ctx, obj, item, ItemDisplay::None, false, m);
    }
    EmitTextured(ctx, obj, model, pre * A::S(-1.0, -1.0, 1.0), nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}
// ---- end batch4 C ----

// ==== batch4 D renderers (insert below) ====
// ---------- arrow / spectral arrow (batch4 D) ----------
// ArrowRenderer: yaw-90 and pitch from Rotation. Tipped arrows use arrow_tipped; the tip state is the potion
// contents of the saved pickup item. 1.21.1 draws arrows with hard-coded quads and has no arrow layer, so
// legacy arrows are counted as skipped.
bool ArrowTipped(const NbtTagPtr& e) {
    auto pc = getChildByName(getChildByName(getChildByName(e, "item"), "components"), "minecraft:potion_contents");
    if (!pc) return false;
    auto effects = getChildByName(pc, "custom_effects");
    return getChildByName(pc, "potion") || getChildByName(pc, "custom_color") || (effects && !effects->children.empty());
}

void RenderArrow(Ctx& ctx, const NbtTagPtr& e, bool spectral) {
    using A = EntityAffine;
    const char* id = spectral ? "minecraft:spectral_arrow" : "minecraft:arrow";
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:arrow#main");
    if (!model.Valid()) { ctx.skippedEntities[id]++; return; }
    Placement pl = ReadPlacement(e);
    const A base = A::T(pl.x, pl.y, pl.z) * A::RYDeg(pl.yaw - 90.0) * A::RZDeg(pl.pitch);
    const std::string tex = spectral ? "entity/projectiles/arrow_spectral"
                          : ArrowTipped(e) ? "entity/projectiles/arrow_tipped" : "entity/projectiles/arrow";
    OutObject& obj = ctx.NewObject(id);
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- trident (batch4 D) ----------
// ThrownTridentRenderer: yaw-90 and pitch+90 from Rotation. The enchantment glint pass is not exported.
void RenderTrident(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:trident#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const A base = A::T(pl.x, pl.y, pl.z) * A::RYDeg(pl.yaw - 90.0) * A::RZDeg(pl.pitch + 90.0);
    const std::string tex = PickTex({ "entity/trident/trident", "entity/trident" });
    OutObject& obj = ctx.NewObject("minecraft:trident");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- leash knot (batch4 D) ----------
// LeashKnotRenderer has no pose: the knot model at the entity position with the usual scale(-1,-1,1).
void RenderLeashKnot(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:leash_knot#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const A base = A::T(pl.x, pl.y, pl.z) * A::S(-1.0, -1.0, 1.0);
    const std::string tex = PickTex({ "entity/lead_knot/lead_knot", "entity/lead_knot" });
    OutObject& obj = ctx.NewObject("minecraft:leash_knot");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- llama spit (batch4 D) ----------
// LlamaSpitRenderer: translate(0, 0.15, 0), then yaw-90 and pitch from Rotation. The model has no animation.
void RenderLlamaSpit(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:llama_spit#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    const A base = A::T(pl.x, pl.y, pl.z) * A::T(0.0, 0.15, 0.0) * A::RYDeg(pl.yaw - 90.0) * A::RZDeg(pl.pitch);
    const std::string tex = PickTex({ "entity/llama/llama_spit", "entity/llama/spit" });
    OutObject& obj = ctx.NewObject("minecraft:llama_spit");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---------- wither skull (batch4 D) ----------
// WitherSkullRenderer: SkullModel head takes the entity yaw/pitch (Rotation), animationPos is 0.
// The blue (invulnerable) skull is the "dangerous" NBT flag. Drawn at the entity position after scale(-1,-1,1).
void RenderWitherSkull(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    EntityModel model(ctx.modern ? "modern" : "legacy", "minecraft:wither_skull#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    model.SetRot("head", HeadPitch(pl), static_cast<float>(pl.yaw * kPi / 180.0), 0.0f);
    const bool dangerous = NumOr(e, "dangerous", 0) != 0;
    const std::string tex = dangerous ? "entity/wither/wither_invulnerable" : "entity/wither/wither";
    const A base = A::T(pl.x, pl.y, pl.z) * A::S(-1.0, -1.0, 1.0);
    OutObject& obj = ctx.NewObject("minecraft:wither_skull");
    EmitTextured(ctx, obj, model, base, nullptr, "minecraft:" + tex, ctx.Texture("minecraft", tex));
}

// ---- end batch4 D ----

// ==== batch4 E renderers (insert below) ====
// ---- end batch4 E ----

// ==== batch4 F renderers (insert below) ====
// ---- end batch4 F ----


// ---------- armor stand ----------
// ArmorStandRenderer + ArmorStandModel(setupAnim) + 盔甲/手持/头部物品层
void RenderArmorStand(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    const std::string fam = ctx.modern ? "modern" : "legacy";
    bool small = NumOr(e, "Small", 0) != 0;
    const std::string kind = std::string("minecraft:armor_stand") + (ctx.modern && small ? "_small" : "");
    EntityModel model(fam, kind + "#main");
    if (!model.Valid()) return;
    Placement pl = ReadPlacement(e);
    pl.sleeping = false;

    // Pose{Head, Body, LeftArm, RightArm, LeftLeg, RightLeg} (度), 缺省值同原版 DEFAULT_*_POSE
    auto pose = getChildByName(e, "Pose");
    auto rot = [&](const char* key, std::array<double, 3> def) {
        auto v = NumList(getChildByName(pose, key));
        if (v.size() >= 3) def = { v[0], v[1], v[2] };
        const double d = kPi / 180.0;
        return std::array<float, 3>{ static_cast<float>(def[0] * d), static_cast<float>(def[1] * d), static_cast<float>(def[2] * d) };
    };
    auto head = rot("Head", { 0, 0, 0 }), body = rot("Body", { 0, 0, 0 });
    auto la = rot("LeftArm", { -10, 0, -10 }), ra = rot("RightArm", { -15, 0, 10 });
    auto ll = rot("LeftLeg", { -1, 0, -1 }), rl = rot("RightLeg", { 1, 0, 1 });
    model.SetRot("head", head[0], head[1], head[2]);
    model.SetRot("body", body[0], body[1], body[2]);
    model.SetRot("left_arm", la[0], la[1], la[2]);
    model.SetRot("right_arm", ra[0], ra[1], ra[2]);
    model.SetRot("left_leg", ll[0], ll[1], ll[2]);
    model.SetRot("right_leg", rl[0], rl[1], rl[2]);
    for (const char* stick : { "right_body_stick", "left_body_stick", "shoulder_stick" })
        model.SetRot(stick, body[0], body[1], body[2]);
    if (!ctx.modern) {
        // ArmorStandArmorModel.setupAnim: hat.copyFrom(head) (盔甲层通过 copyPropertiesTo 继承)
        int h = model.Find("head"), hat = model.Find("hat");
        if (h >= 0 && hat >= 0) for (int k = 0; k < 6; ++k) model.Pose(hat)[k] = model.Pose(h)[k];
    } else {
        model.SetRot("base_plate", 0.0f, static_cast<float>(-pl.yaw * kPi / 180.0), 0.0f);
    }
    bool showArms = NumOr(e, "ShowArms", 0) != 0;
    model.SetVisible("left_arm", showArms);
    model.SetVisible("right_arm", showArms);
    model.SetVisible("base_plate", NumOr(e, "NoBasePlate", 0) == 0);
    model.SetVisible("hat", false);

    A base = LivingBase(pl, 1.0, 1.0);
    TopPre pre;
    if (!ctx.modern && small) {  // 旧版 AgeableListModel 幼体 (头部件 head/hat, 其余为身体部件)
        pre = [](const std::string& top) {
            if (top == "head" || top == "hat") return A::S(0.75) * A::T(0.0, 1.0, 0.0);
            return A::S(0.5) * A::T(0.0, 1.5, 0.0);
        };
    }
    OutObject& obj = ctx.NewObject("minecraft:armor_stand");
    if (NumOr(e, "Invisible", 0) == 0) {
        // ArmorStandRenderer.DEFAULT_SKIN_LOCATION: 26.x armorstand.png, 1.21 wood.png
        const std::string skin = ctx.modern ? "entity/armorstand/armorstand" : "entity/armorstand/wood";
        std::string tex = ctx.Texture("minecraft", skin);
        if (!tex.empty()) {
            int mat = ctx.Material("minecraft:" + skin, tex);
            model.Emit(obj.mesh, base, [mat](int) { return mat; }, pre);
        }
    }
    Equipment eq = ReadEquipment(e);
    RenderArmor(ctx, obj, kind, model, eq, base, pre, small, true);
    RenderHeldItems(ctx, obj, model, eq, base, !ctx.modern && small, false);
    RenderHeadItem(ctx, obj, model, eq, base, !ctx.modern && small, false, 0.0);
}

// ---------- item frame ----------
// ItemFrameRenderer: 框 = block/(glow_)item_frame(_map) 方块模型, 物品 = FIXED 上下文 * 0.5
void RenderItemFrame(Ctx& ctx, const NbtTagPtr& e, bool glow) {
    using A = EntityAffine;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return;
    static const int kDir[6][3] = { {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0} };
    int facing = std::clamp(static_cast<int>(NumOr(e, "Facing", 0)), 0, 5);
    const int* d = kDir[facing];
    double xRot = 0.0, yRot = 180.0;
    if (d[1] == 0) {
        double toYRot = d[2] == 1 ? 0.0 : d[0] == -1 ? 90.0 : d[2] == -1 ? 180.0 : 270.0;  // Direction.toYRot
        yRot = 180.0 - toYRot;
    } else {
        xRot = -90.0 * d[1];
    }
    A m = A::T(pos[0], pos[1], pos[2]) * A::T(d[0] * 0.46875, d[1] * 0.46875, d[2] * 0.46875) *
          A::RX(xRot * kPi / 180.0) * A::RY(yRot * kPi / 180.0);
    ItemStackInfo item = ItemRender::ParseItemStack(getChildByName(e, "Item"));
    bool isMap = item.id == "minecraft:filled_map";
    bool invisible = NumOr(e, "Invisible", 0) != 0;
    OutObject& obj = ctx.NewObject(glow ? "minecraft:glow_item_frame" : "minecraft:item_frame");
    if (!invisible) {
        std::string frameModel = std::string("block/") + (glow ? "glow_item_frame" : "item_frame") + (isMap ? "_map" : "");
        ModelData frame = ProcessModelJson("minecraft", frameModel, 0, 0, false);
        ItemRender::Transform(frame, m * A::T(-0.5, -0.5, -0.5));
        AppendMesh(ctx, obj, frame);
    }
    if (item.empty() || isMap) { if (isMap) ctx.skippedItems[item.id]++; return; }  // 地图内容需要地图数据, 未支持
    int rotation = static_cast<int>(NumOr(e, "ItemRotation", 0));
    m = m * A::T(0.0, 0.0, invisible ? 0.5 : 0.4375) * A::RZ(rotation * kPi / 4.0) * A::S(0.5);
    AppendItem(ctx, obj, item, ItemDisplay::Fixed, false, m);
}

// ---------- dropped item ----------
// ItemEntityRenderer: 上下浮动取 bobOffs = 0, 旋转 = Age/20 弧度; 堆叠只画一份
void RenderItemEntity(Ctx& ctx, const NbtTagPtr& e) {
    using A = EntityAffine;
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return;
    ItemStackInfo item = ItemRender::ParseItemStack(getChildByName(e, "Item"));
    if (item.empty()) return;
    ModelData mesh;
    double groundScaleY = 1.0;
    if (!ItemRender::Build(item, ItemDisplay::Ground, false, mesh, &groundScaleY)) { ctx.skippedItems[item.id]++; return; }
    double age = NumOr(e, "Age", 0);
    double bob = std::sin(age / 10.0) * 0.1 + 0.1;
    double lift;
    if (ctx.modern) {  // 26.x: -bbox.minY + 1/16
        double minY = 1e9;
        for (size_t i = 1; i < mesh.vertices.size(); i += 3) minY = std::min<double>(minY, mesh.vertices[i]);
        lift = (minY < 1e8 ? -minY : 0.0) + 0.0625;
    } else {
        lift = 0.25 * groundScaleY;
    }
    ItemRender::Transform(mesh, A::T(pos[0], pos[1], pos[2]) * A::T(0.0, bob + lift, 0.0) * A::RY(age / 20.0));
    OutObject& obj = ctx.NewObject("minecraft:item");
    AppendMesh(ctx, obj, mesh);
}

bool InSelection(const NbtTagPtr& e) {
    auto pos = NumList(getChildByName(e, "Pos"));
    if (pos.size() < 3) return false;
    return pos[0] >= config.minX && pos[0] <= config.maxX + 1.0 &&
           pos[1] >= config.minY && pos[1] <= config.maxY + 1.0 &&
           pos[2] >= config.minZ && pos[2] <= config.maxZ + 1.0;
}

fs::path Utf8Path(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

void WriteFiles(const Ctx& ctx, const std::string& name) {
    const std::string dir = getExecutableDir();
    // 全局 locale 是 en_US.UTF-8 (fileutils/GlobalCache), 不 imbue 的话数字会带千位分隔符 ("1,000")
    std::ofstream mtl(Utf8Path(dir + name + ".mtl"), std::ios::binary);
    mtl.imbue(std::locale::classic());
    for (const auto& m : ctx.materials) {
        RegisterMaterialFullName(m.name);
        mtl << "newmtl " << BlenderSafeName(m.name) << "\n"
            << "Ns 90.000000\nKd 1.000000 1.000000 1.000000\nKs 0.000000 0.000000 0.000000\n"
            << "Ke 0.000000 0.000000 0.000000\nNi 1.500000\nillum 1\n"
            << "map_Kd " << m.texture << "\n\n";
    }
    std::ofstream obj(Utf8Path(dir + name + ".obj"), std::ios::binary);
    obj.imbue(std::locale::classic());
    obj << "# WorldImporter entities\nmtllib " << name << ".mtl\n";
    obj << std::fixed;
    size_t vBase = 1, tBase = 1;
    for (const auto& o : ctx.objects) {
        const ModelData& md = o.mesh;
        obj << "o " << o.name << "\n";
        obj << std::setprecision(5);
        for (size_t i = 0; i + 2 < md.vertices.size(); i += 3)
            obj << "v " << md.vertices[i] << ' ' << md.vertices[i + 1] << ' ' << md.vertices[i + 2] << "\n";
        obj << std::setprecision(6);
        for (size_t i = 0; i + 1 < md.uvCoordinates.size(); i += 2)
            obj << "vt " << md.uvCoordinates[i] << ' ' << md.uvCoordinates[i + 1] << "\n";
        int cur = -1;
        for (const auto& f : md.faces) {
            if (f.materialIndex != cur) { cur = f.materialIndex; obj << "usemtl " << BlenderSafeName(ctx.materials[cur].name) << "\n"; }
            obj << "f";
            for (int k = 0; k < 4; ++k)
                obj << ' ' << (vBase + f.vertexIndices[k]) << '/' << (tBase + f.uvIndices[k]);
            obj << "\n";
        }
        vBase += md.vertices.size() / 3;
        tBase += md.uvCoordinates.size() / 2;
    }
}
// 区域导出已写过 tint.json; 并入实体物品/盔甲的固定色 (同 CreateTintJsonFile 的格式, color 为线性值)
void MergeTintJson(const std::map<std::string, TintResult>& tints) {
    if (tints.empty()) return;
    const fs::path path = Utf8Path(getExecutableDir() + "tint.json");
    nlohmann::json root = nlohmann::json::object();
    {
        std::ifstream in(path, std::ios::binary);
        if (in) { try { in >> root; } catch (...) { root = nlohmann::json::object(); } }
    }
    if (!root.is_object()) root = nlohmann::json::object();
    auto lin = [](uint32_t c) {
        float s = static_cast<float>(c & 0xFF) / 255.0f;
        return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
    };
    for (const auto& [name, t] : tints) {
        if (t.kind != TintKind::Fixed) continue;
        root[name] = { {"kind", TintKindName(t.kind)}, {"color", {lin(t.color >> 16), lin(t.color >> 8), lin(t.color)}} };
    }
    std::ofstream out(path, std::ios::binary);
    out << root.dump(2);
}
}  // namespace

namespace EntityModelExporter {
size_t Export(const std::string& name) {
    Ctx ctx;
    // 26.x 的幼体是独立模型 + 贴图; 以原版资源中是否存在 villager_baby.png 判定模型族
    ctx.modern = TextureExists("minecraft", "entity/villager/villager_baby");
    size_t chunks = EntityExporter::VisitEntities([&](const NbtTagPtr& e) {
        std::string id = Str(e, "id");
        if (id.empty() || !InSelection(e)) return;
        if (id == "minecraft:armor_stand") { RenderArmorStand(ctx, e); return; }       // 隐形时仍画装备
        if (id == "minecraft:item_frame" || id == "minecraft:glow_item_frame") {
            RenderItemFrame(ctx, e, id == "minecraft:glow_item_frame"); return;
        }
        if (id == "minecraft:item") { RenderItemEntity(ctx, e); return; }
        if (NumOr(e, "Invisible", 0) != 0) return;
        bool more1 = false, more2 = false;
        if (id == "minecraft:villager") RenderVillagerLike(ctx, e, false);
        else if (id == "minecraft:zombie_villager") RenderVillagerLike(ctx, e, true);
        else if (id == "minecraft:zombie")
            RenderHumanoidMob(ctx, e, "zombie", "entity/zombie/zombie", 1.74, true);
        else if (id == "minecraft:husk")
            RenderHumanoidMob(ctx, e, "husk", "entity/zombie/husk", 1.74, true, 1.0625);
        else if (id == "minecraft:drowned")
            // Vanilla holds a drowned's arms like a zombie only while it is aggressive; keep the zombie pose for now.
            RenderHumanoidMob(ctx, e, "drowned", "entity/zombie/drowned", 1.74, true, 1.0,
                              "#outer", "entity/zombie/drowned_outer_layer");
        else if (id == "minecraft:skeleton")
            RenderHumanoidMob(ctx, e, "skeleton", "entity/skeleton/skeleton", 1.74, false);
        else if (id == "minecraft:stray")
            RenderHumanoidMob(ctx, e, "stray", "entity/skeleton/stray", 1.74, false, 1.0,
                              "#outer", "entity/skeleton/stray_overlay");
        else if (id == "minecraft:bogged")
            RenderHumanoidMob(ctx, e, "bogged", "entity/skeleton/bogged", 1.74, false, 1.0,
                              "#outer", "entity/skeleton/bogged_overlay");
        else if (id == "minecraft:wither_skeleton")
            RenderHumanoidMob(ctx, e, "wither_skeleton", "entity/skeleton/wither_skeleton", 2.1, false, 1.2);
        else if (id == "minecraft:creeper") RenderCreeper(ctx, e);
        else if (id == "minecraft:pig") RenderPig(ctx, e);
        else if (id == "minecraft:cow") RenderClimateCow(ctx, e);
        else if (id == "minecraft:mooshroom") RenderMooshroom(ctx, e);
        else if (id == "minecraft:sheep") RenderSheep(ctx, e);
        else if (id == "minecraft:chicken") RenderClimateChicken(ctx, e);
        else if (id == "minecraft:wolf") RenderWolf(ctx, e);
        else if (id == "minecraft:cat") RenderCat(ctx, e);
        else if (id == "minecraft:horse") RenderEquine(ctx, e, EquineKind::Horse);
        else if (id == "minecraft:donkey") RenderEquine(ctx, e, EquineKind::Donkey);
        else if (id == "minecraft:mule") RenderEquine(ctx, e, EquineKind::Mule);
        else if (id == "minecraft:skeleton_horse") RenderEquine(ctx, e, EquineKind::Skeleton);
        else if (id == "minecraft:zombie_horse") RenderEquine(ctx, e, EquineKind::Zombie);
        else if (id == "minecraft:strider") RenderStrider(ctx, e);
        else if (id == "minecraft:spider") RenderSpider(ctx, e, false);
        else if (id == "minecraft:cave_spider") RenderSpider(ctx, e, true);
        else if (id == "minecraft:enderman") RenderEnderman(ctx, e);
        else if (id == "minecraft:iron_golem") RenderIronGolem(ctx, e);
        else if (id == "minecraft:snow_golem") RenderSnowGolem(ctx, e);
        else if (id == "minecraft:llama") RenderLlama(ctx, e, false);
        else if (id == "minecraft:trader_llama") RenderLlama(ctx, e, true);
        else if (id == "minecraft:fox") RenderFox(ctx, e);
        else if (id == "minecraft:bee") RenderBee(ctx, e);
        else if (id == "minecraft:goat") RenderGoat(ctx, e);
        else if (id == "minecraft:polar_bear") RenderPolarBear(ctx, e);
        else if (id == "minecraft:panda") RenderPanda(ctx, e);
        else if (id == "minecraft:rabbit") RenderRabbit(ctx, e);
        else if (id == "minecraft:turtle") RenderTurtle(ctx, e);
        else if (id == "minecraft:parrot") RenderParrot(ctx, e);
        else if (id == "minecraft:slime") RenderSlime(ctx, e);
        else if (id == "minecraft:cod") RenderCod(ctx, e);
        else if (id == "minecraft:salmon") RenderSalmon(ctx, e);
        else if (id == "minecraft:pufferfish") RenderPufferfish(ctx, e);
        else if (id == "minecraft:squid") RenderSquid(ctx, e, false);
        else more1 = true;  // MSVC C1061: 拆开 else-if 链避免嵌套超限
        if (more1) {
        if (id == "minecraft:glow_squid") RenderSquid(ctx, e, true);
        else if (id == "minecraft:dolphin") RenderDolphin(ctx, e);
        else if (id == "minecraft:guardian") RenderGuardian(ctx, e, false);
        else if (id == "minecraft:elder_guardian") RenderGuardian(ctx, e, true);
        else if (id == "minecraft:silverfish") RenderSegmentedBug(ctx, e, false);
        else if (id == "minecraft:endermite") RenderSegmentedBug(ctx, e, true);
        else if (id == "minecraft:blaze") RenderBlaze(ctx, e);
        else if (id == "minecraft:ghast") RenderGhast(ctx, e);
        else if (id == "minecraft:magma_cube") RenderMagmaCube(ctx, e);
        // ==== batch2 A dispatch (insert below) ====
        else if (id == "minecraft:ravager") RenderRavager(ctx, e);
        else if (id == "minecraft:hoglin") RenderHoglin(ctx, e);
        else if (id == "minecraft:zoglin") RenderZoglin(ctx, e);
        else if (id == "minecraft:ocelot") RenderOcelot(ctx, e);
        // ---- end batch2 A ----
        // ==== batch2 B dispatch (insert below) ====
        else if (id == "minecraft:tadpole") RenderTadpole(ctx, e);
        else if (id == "minecraft:vex") RenderVex(ctx, e);
        else if (id == "minecraft:allay") RenderAllay(ctx, e);
        else if (id == "minecraft:phantom") RenderPhantom(ctx, e);
        // ---- end batch2 B ----
        // ==== batch2 C dispatch (insert below) ====
        else if (id == "minecraft:shulker") RenderShulker(ctx, e);
        else if (id == "minecraft:witch") RenderWitch(ctx, e);
        // ---- end batch2 C ----
        // ==== batch3 A dispatch (insert below) ====
        else if (id == "minecraft:pillager") RenderPillager(ctx, e);
        else if (id == "minecraft:vindicator") RenderVindicator(ctx, e);
        else if (id == "minecraft:evoker") RenderEvoker(ctx, e);
        else if (id == "minecraft:illusioner") RenderIllusioner(ctx, e);
        else if (id == "minecraft:wandering_trader") RenderWanderingTrader(ctx, e);
        // ---- end batch3 A ----
        // ==== batch3 B dispatch (insert below) ====
        else if (id == "minecraft:piglin") RenderPiglin(ctx, e);
        else if (id == "minecraft:piglin_brute") RenderPiglinBrute(ctx, e);
        else if (id == "minecraft:zombified_piglin") RenderZombifiedPiglin(ctx, e);
        else if (id == "minecraft:bat") RenderBat(ctx, e);
        else if (id == "minecraft:giant") RenderGiant(ctx, e);
        // ---- end batch3 B ----
        // ==== batch3 C dispatch (insert below) ====
        else if (id == "minecraft:frog") RenderFrog(ctx, e);
        else if (id == "minecraft:armadillo") RenderArmadillo(ctx, e);
        else if (id == "minecraft:axolotl") RenderAxolotl(ctx, e);
        else if (id == "minecraft:tropical_fish") RenderTropicalFish(ctx, e);
        // ---- end batch3 C ----
        // ==== batch3 D dispatch (insert below) ====
        else if (id == "minecraft:creaking") RenderCreaking(ctx, e);
        else if (id == "minecraft:warden") RenderWarden(ctx, e);
        else if (id == "minecraft:breeze") RenderBreeze(ctx, e);
        else if (id == "minecraft:sniffer") RenderSniffer(ctx, e);
        else if (id == "minecraft:camel") RenderCamel(ctx, e);
        // ---- end batch3 D ----
        // ==== batch4 A dispatch (insert below) ====
        else if (id == "minecraft:ender_dragon" && ctx.modern) RenderEnderDragon(ctx, e);
        else more2 = true;  // MSVC C1061: 拆开 else-if 链避免嵌套超限
        if (more2) {
        if (id == "minecraft:wither") RenderWither(ctx, e);
        else if (id == "minecraft:end_crystal") RenderEndCrystal(ctx, e);
        // ---- end batch4 A ----
        // ==== batch4 B dispatch (insert below) ====
        else if (id == "minecraft:happy_ghast") RenderHappyGhast(ctx, e);
        else if (id == "minecraft:copper_golem") RenderCopperGolem(ctx, e);
        else if (id == "minecraft:nautilus") RenderNautilus(ctx, e);
        else if (id == "minecraft:zombie_nautilus") RenderZombieNautilus(ctx, e);
        else if (id == "minecraft:parched") RenderParched(ctx, e);
        else if (id == "minecraft:sulfur_cube") RenderSulfurCube(ctx, e);
        // ---- end batch4 B ----
        // ==== batch4 C dispatch (insert below) ====
        else if (!ctx.modern && (id == "minecraft:boat" || id == "minecraft:chest_boat")) RenderLegacyBoat(ctx, e, id);
        else if (id == "minecraft:oak_boat") RenderBoat(ctx, e, id, "boat/oak");
        else if (id == "minecraft:spruce_boat") RenderBoat(ctx, e, id, "boat/spruce");
        else if (id == "minecraft:birch_boat") RenderBoat(ctx, e, id, "boat/birch");
        else if (id == "minecraft:jungle_boat") RenderBoat(ctx, e, id, "boat/jungle");
        else if (id == "minecraft:acacia_boat") RenderBoat(ctx, e, id, "boat/acacia");
        else if (id == "minecraft:dark_oak_boat") RenderBoat(ctx, e, id, "boat/dark_oak");
        else if (id == "minecraft:mangrove_boat") RenderBoat(ctx, e, id, "boat/mangrove");
        else if (id == "minecraft:cherry_boat") RenderBoat(ctx, e, id, "boat/cherry");
        else if (ctx.modern && id == "minecraft:pale_oak_boat") RenderBoat(ctx, e, id, "boat/pale_oak");
        else if (ctx.modern && id == "minecraft:poplar_boat") RenderBoat(ctx, e, id, "boat/poplar");
        else if (id == "minecraft:bamboo_raft") RenderBoat(ctx, e, id, "boat/bamboo");
        else if (id == "minecraft:oak_chest_boat") RenderBoat(ctx, e, id, "chest_boat/oak");
        else if (id == "minecraft:spruce_chest_boat") RenderBoat(ctx, e, id, "chest_boat/spruce");
        else if (id == "minecraft:birch_chest_boat") RenderBoat(ctx, e, id, "chest_boat/birch");
        else if (id == "minecraft:jungle_chest_boat") RenderBoat(ctx, e, id, "chest_boat/jungle");
        else if (id == "minecraft:acacia_chest_boat") RenderBoat(ctx, e, id, "chest_boat/acacia");
        else if (id == "minecraft:dark_oak_chest_boat") RenderBoat(ctx, e, id, "chest_boat/dark_oak");
        else if (id == "minecraft:mangrove_chest_boat") RenderBoat(ctx, e, id, "chest_boat/mangrove");
        else if (id == "minecraft:cherry_chest_boat") RenderBoat(ctx, e, id, "chest_boat/cherry");
        else if (ctx.modern && id == "minecraft:pale_oak_chest_boat") RenderBoat(ctx, e, id, "chest_boat/pale_oak");
        else if (ctx.modern && id == "minecraft:poplar_chest_boat") RenderBoat(ctx, e, id, "chest_boat/poplar");
        else if (id == "minecraft:bamboo_chest_raft") RenderBoat(ctx, e, id, "chest_boat/bamboo");
        else if (id == "minecraft:minecart") RenderMinecart(ctx, e, id, "minecart", "", 6);
        else if (id == "minecraft:chest_minecart") RenderMinecart(ctx, e, id, "chest_minecart", "minecraft:chest", 8);
        else if (id == "minecraft:furnace_minecart") RenderMinecart(ctx, e, id, "furnace_minecart", "minecraft:furnace", 6);
        else if (id == "minecraft:hopper_minecart") RenderMinecart(ctx, e, id, "hopper_minecart", "minecraft:hopper", 1);
        else if (id == "minecraft:tnt_minecart") RenderMinecart(ctx, e, id, "tnt_minecart", "minecraft:tnt", 6);
        else if (id == "minecraft:spawner_minecart") RenderMinecart(ctx, e, id, "spawner_minecart", "minecraft:spawner", 6);
        else if (id == "minecraft:command_block_minecart") RenderMinecart(ctx, e, id, "command_block_minecart", "minecraft:command_block", 6);
        // ---- end batch4 C ----
        // ==== batch4 D dispatch (insert below) ====
        else if (id == "minecraft:arrow") RenderArrow(ctx, e, false);
        else if (id == "minecraft:spectral_arrow") RenderArrow(ctx, e, true);
        else if (id == "minecraft:trident") RenderTrident(ctx, e);
        else if (id == "minecraft:leash_knot") RenderLeashKnot(ctx, e);
        else if (id == "minecraft:llama_spit") RenderLlamaSpit(ctx, e);
        else if (id == "minecraft:wither_skull") RenderWitherSkull(ctx, e);
        // ---- end batch4 D ----
        // ==== batch4 E dispatch (insert below) ====
        // ---- end batch4 E ----
        // ==== batch4 F dispatch (insert below) ====
        // ---- end batch4 F ----
        else if (id != "minecraft:painting") ctx.skippedEntities[id]++;  // 画由 PaintingExporter 导出
        }
        }
    });
    if (!ctx.skippedEntities.empty()) {
        std::cout << "[EntityModels] entities without model:";
        for (const auto& [id, n] : ctx.skippedEntities) std::cout << ' ' << id << '=' << n;
        std::cout << std::endl;
    }
    if (ctx.objects.empty()) {
        std::cout << "[EntityModels] no supported entities (" << chunks << " entity chunks)" << std::endl;
        std::error_code ec;  // 不留上次导出的实体文件
        fs::remove(Utf8Path(getExecutableDir() + name + ".obj"), ec);
        fs::remove(Utf8Path(getExecutableDir() + name + ".mtl"), ec);
        return 0;
    }
    WriteFiles(ctx, name);
    std::map<std::string, TintResult> safeTints;
    for (const auto& [k, v] : ctx.tints) safeTints[BlenderSafeName(k)] = v;
    MergeTintJson(safeTints);
    WriteMaterialsJson();     // 区域导出已写过一次；这里读旧文件合并实体材质名
    CreateOverlayJsonFile();  // 区域导出已写过一次; 补上实体叠加层后重写
    std::cout << "[EntityModels] " << ctx.objects.size() << " entities (" << (ctx.modern ? "modern" : "legacy")
              << " models), " << ctx.materials.size() << " materials ->" << name << ".obj:";
    for (const auto& [id, n] : ctx.exported) std::cout << ' ' << id << '=' << n;
    std::cout << std::endl;
    if (!ctx.skippedItems.empty()) {
        std::cout << "[EntityModels] items without model (special renderer/map):";
        for (const auto& [id, n] : ctx.skippedItems) std::cout << ' ' << id << '=' << n;
        std::cout << std::endl;
    }
    return ctx.objects.size();
}
}
