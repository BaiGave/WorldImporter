#include "ItemRender.h"
#include "EntityNbt.h"
#include "GlobalCache.h"
#include "blocktint.h"
#include "texture.h"
#include "include/stb_image_write.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <unordered_map>

ModelData ProcessModelData(const nlohmann::json& modelJson, const std::string& blockName);  // model.cpp

using namespace EntityNbt;

namespace {
constexpr double kPi = 3.14159265358979323846;

// ---------------- 资源 ----------------
std::mutex g_jsonMutex;
std::unordered_map<std::string, nlohmann::json> g_jsonCache;

const char* DisplayName(ItemDisplay d) {
    switch (d) {
    case ItemDisplay::ThirdPersonLeftHand: return "thirdperson_lefthand";
    case ItemDisplay::ThirdPersonRightHand: return "thirdperson_righthand";
    case ItemDisplay::FirstPersonLeftHand: return "firstperson_lefthand";
    case ItemDisplay::FirstPersonRightHand: return "firstperson_righthand";
    case ItemDisplay::Head: return "head";
    case ItemDisplay::Gui: return "gui";
    case ItemDisplay::Ground: return "ground";
    case ItemDisplay::Fixed: return "fixed";
    default: return "none";
    }
}

int ColorOf(const nlohmann::json& v) {  // ARGB int 或 [r, g, b] (0..1)
    if (v.is_number_integer()) return static_cast<int>(v.get<int64_t>() & 0xFFFFFF);
    if (v.is_array() && v.size() >= 3) {
        int c = 0;
        for (int i = 0; i < 3; ++i)
            c = (c << 8) | std::clamp(static_cast<int>(std::lround(v[i].get<double>() * 255.0)), 0, 255);
        return c;
    }
    return -1;
}

// ---------------- 物品定义 (1.21.4+ items/*.json) ----------------
struct ModelRef {
    std::string ns, path;     // models/<path>.json
    std::vector<int> tints;   // 按 tintindex 的 RGB, -1 = 不染色
};

std::vector<int> ParseTints(const nlohmann::json& node, const ItemStackInfo& item) {
    std::vector<int> out;
    if (!node.contains("tints") || !node["tints"].is_array()) return out;
    for (const auto& t : node["tints"]) {
        std::string type = t.value("type", "");
        if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
        int c = -1;
        if (type == "constant" && t.contains("value")) c = ColorOf(t["value"]);
        else if (type == "dye") c = item.dyeColor >= 0 ? item.dyeColor : (t.contains("default") ? ColorOf(t["default"]) : -1);
        else if (type == "grass") c = 0x7CBD6B;  // GrassColor.get(0.5, 1.0) (原版物品全部用该参数)
        else if (t.contains("default")) c = ColorOf(t["default"]);
        out.push_back(c);
    }
    return out;
}

bool WhenMatches(const nlohmann::json& when, const std::string& value) {
    if (when.is_string()) return when.get<std::string>() == value;
    if (when.is_array())
        for (const auto& w : when) if (w.is_string() && w.get<std::string>() == value) return true;
    return false;
}

// 静态状态下的分支选择: condition -> on_false, select -> 匹配项或 fallback, range_dispatch -> fallback/首项
bool ResolveItemDef(const nlohmann::json& node, const ItemStackInfo& item, ItemDisplay display,
                    std::vector<ModelRef>& out, int depth = 0) {
    if (depth > 32 || !node.is_object()) return false;
    std::string type = node.value("type", "");
    if (type.rfind("minecraft:", 0) == 0) type = type.substr(10);
    if (type == "model") {
        std::string m = node.value("model", "");
        if (m.empty()) return false;
        auto [ns, path] = SplitId(m);
        out.push_back({ ns, path, ParseTints(node, item) });
        return true;
    }
    if (type == "composite") {
        bool any = false;
        if (node.contains("models") && node["models"].is_array())
            for (const auto& m : node["models"]) any = ResolveItemDef(m, item, display, out, depth + 1) || any;
        return any;
    }
    if (type == "condition") {
        return node.contains("on_false") && ResolveItemDef(node["on_false"], item, display, out, depth + 1);
    }
    if (type == "select") {
        std::string prop = node.value("property", "");
        std::string value;
        if (prop == "minecraft:display_context") value = DisplayName(display);
        else if (prop == "minecraft:trim_material") value = item.trimMaterial;
        if (!value.empty() && node.contains("cases") && node["cases"].is_array())
            for (const auto& c : node["cases"])
                if (c.contains("when") && WhenMatches(c["when"], value) && c.contains("model"))
                    return ResolveItemDef(c["model"], item, display, out, depth + 1);
        if (node.contains("fallback")) return ResolveItemDef(node["fallback"], item, display, out, depth + 1);
        return false;
    }
    if (type == "range_dispatch") {
        if (node.contains("fallback")) return ResolveItemDef(node["fallback"], item, display, out, depth + 1);
        if (node.contains("entries") && node["entries"].is_array() && !node["entries"].empty() &&
            node["entries"][0].contains("model"))
            return ResolveItemDef(node["entries"][0]["model"], item, display, out, depth + 1);
        return false;
    }
    return false;  // special (箱子/盾牌/旗帜/头颅/三叉戟...), bundle/selected_item, empty
}

// ---------------- 旧版物品染色 (ItemColors, 无世界上下文) ----------------
std::vector<int> LegacyTints(const std::string& id, const ItemStackInfo& item) {
    auto [ns, p] = SplitId(id);
    if (ns != "minecraft") return {};
    if (p.rfind("leather_", 0) == 0) return { item.dyeColor >= 0 ? item.dyeColor : 0xA06540 };
    if (p == "grass_block" || p == "grass" || p == "short_grass" || p == "fern" || p == "tall_grass" || p == "large_fern")
        return { 0x7CBD6B };
    if (p == "spruce_leaves") return { 0x619961 };
    if (p == "birch_leaves") return { 0x80A755 };
    if (p == "mangrove_leaves") return { 0x92C648 };
    if (p == "oak_leaves" || p == "jungle_leaves" || p == "acacia_leaves" || p == "dark_oak_leaves" || p == "vine")
        return { 0x48B518 };
    if (p == "lily_pad") return { 0x71C35C };
    return {};
}

// ---------------- 模型 JSON 继承链 ----------------
struct ResolvedModel {
    std::map<std::string, std::string> textures;  // key -> 原始值 ("#ref" 或 "ns:path")
    nlohmann::json elements;                      // 首个(最近的)带 elements 的模型
    std::map<std::string, nlohmann::json> display;
    bool generated = false, builtinEntity = false, ok = false;
};

std::string TexValue(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_object() && v.contains("sprite") && v["sprite"].is_string()) return v["sprite"].get<std::string>();
    return {};
}

ResolvedModel ResolveModel(std::string ns, std::string path) {
    ResolvedModel r;
    for (int depth = 0; depth < 64; ++depth) {
        if (path == "builtin/generated") { r.generated = true; break; }
        if (path == "builtin/entity") { r.builtinEntity = true; break; }
        nlohmann::json j = GetModelJson(ns, path);
        if (!j.is_object()) { if (depth == 0) return r; break; }
        if (j.contains("textures") && j["textures"].is_object())
            for (auto& t : j["textures"].items())
                if (!r.textures.count(t.key())) r.textures[t.key()] = TexValue(t.value());
        if (r.elements.is_null() && j.contains("elements") && j["elements"].is_array()) r.elements = j["elements"];
        if (j.contains("display") && j["display"].is_object())
            for (auto& d : j["display"].items())
                if (!r.display.count(d.key())) r.display[d.key()] = d.value();
        if (!j.contains("parent") || !j["parent"].is_string()) break;
        auto [pns, ppath] = SplitId(j["parent"].get<std::string>());
        ns = pns; path = ppath;
    }
    r.ok = true;
    return r;
}

std::string ResolveTexture(const ResolvedModel& r, const std::string& key) {
    auto it = r.textures.find(key);
    std::string v = it == r.textures.end() ? std::string() : it->second;
    for (int d = 0; d < 32 && !v.empty() && v[0] == '#'; ++d) {
        auto ref = r.textures.find(v.substr(1));
        v = ref == r.textures.end() ? std::string() : ref->second;
    }
    return (!v.empty() && v[0] == '#') ? std::string() : v;
}

// ItemTransform.apply(leftHand) + translate(-0.5)
EntityAffine DisplayTransform(const ResolvedModel& r, ItemDisplay display, bool leftHand, double* scaleY) {
    using A = EntityAffine;
    const nlohmann::json* t = nullptr;
    auto find = [&](const char* k) -> const nlohmann::json* {
        auto it = r.display.find(k);
        return it == r.display.end() ? nullptr : &it->second;
    };
    if (display != ItemDisplay::None) t = find(DisplayName(display));
    if (!t && display == ItemDisplay::ThirdPersonLeftHand) t = find("thirdperson_righthand");
    if (!t && display == ItemDisplay::FirstPersonLeftHand) t = find("firstperson_righthand");
    double rot[3] = { 0, 0, 0 }, tr[3] = { 0, 0, 0 }, sc[3] = { 1, 1, 1 };
    if (t && t->is_object()) {
        auto vec = [&](const char* k, double* o, double def) {
            if (t->contains(k) && (*t)[k].is_array() && (*t)[k].size() >= 3)
                for (int i = 0; i < 3; ++i) o[i] = (*t)[k][i].is_number() ? (*t)[k][i].get<double>() : def;
        };
        vec("rotation", rot, 0); vec("translation", tr, 0); vec("scale", sc, 1);
        for (int i = 0; i < 3; ++i) { tr[i] = std::clamp(tr[i], -80.0, 80.0) / 16.0; sc[i] = std::clamp(sc[i], -4.0, 4.0); }
    }
    if (scaleY) *scaleY = sc[1];
    if (leftHand) { tr[0] = -tr[0]; rot[1] = -rot[1]; rot[2] = -rot[2]; }
    const double d = kPi / 180.0;
    return A::T(tr[0], tr[1], tr[2]) * A::RX(rot[0] * d) * A::RY(rot[1] * d) * A::RZ(rot[2] * d) *
           A::S(sc[0], sc[1], sc[2]) * A::T(-0.5, -0.5, -0.5);
}

// ItemModelGenerator: 每层 = 前后两面 (z 7.5..8.5) + 按不透明像素边界生成的侧面条带
nlohmann::json GeneratedElements(const ResolvedModel& r, nlohmann::json& textures) {
    nlohmann::json elements = nlohmann::json::array();
    for (int layer = 0; layer < 16; ++layer) {
        std::string key = "layer" + std::to_string(layer);
        std::string tex = ResolveTexture(r, key);
        if (tex.empty()) break;
        textures[key] = tex;
        auto [tns, tpath] = SplitId(tex);
        const std::string ref = "#" + key;
        nlohmann::json front = { {"from", {0, 0, 7.5}}, {"to", {16, 16, 8.5}}, {"faces", {
            {"south", {{"uv", {0, 0, 16, 16}}, {"texture", ref}, {"tintindex", layer}}},
            {"north", {{"uv", {16, 0, 0, 16}}, {"texture", ref}, {"tintindex", layer}}} }} };
        elements.push_back(front);

        std::vector<unsigned char> px; int w = 0, h = 0;
        if (!LoadTexturePixels(tns, tpath, px, w, h) || w <= 0 || h <= 0) continue;
        int fh = (h > w && h % w == 0) ? w : h;  // 动态贴图: 竖排帧
        int frames = h / fh;
        auto transparent = [&](int f, int x, int y) {
            if (x < 0 || y < 0 || x >= w || y >= fh) return true;
            return px[(static_cast<size_t>(f * fh + y) * w + x) * 4 + 3] == 0;
        };
        // SpanFacing: UP(dy=-1) DOWN(dy=+1) LEFT(dx=-1, 面朝 EAST) RIGHT(dx=+1, 面朝 WEST)
        struct Span { int facing, min, max, anchor; };
        std::vector<Span> spans;
        const int dxs[4] = { 0, 0, -1, 1 }, dys[4] = { -1, 1, 0, 0 };
        for (int f = 0; f < frames; ++f)
            for (int y = 0; y < fh; ++y)
                for (int x = 0; x < w; ++x) {
                    if (transparent(f, x, y)) continue;
                    for (int s = 0; s < 4; ++s) {
                        if (!transparent(f, x + dxs[s], y + dys[s])) continue;
                        bool horiz = s < 2;
                        int anchor = horiz ? y : x, pos = horiz ? x : y;
                        auto it = std::find_if(spans.begin(), spans.end(),
                                               [&](const Span& sp) { return sp.facing == s && sp.anchor == anchor; });
                        if (it == spans.end()) spans.push_back({ s, pos, pos, anchor });
                        else { it->min = std::min(it->min, pos); it->max = std::max(it->max, pos); }
                    }
                }
        const float sx = 16.0f / w, sy = 16.0f / fh;
        static const char* kDir[4] = { "up", "down", "east", "west" };
        for (const auto& sp : spans) {
            float x0 = 0, y0 = 0, x1 = 0, y1 = 0, u0 = 0, u1 = 0, v0 = 0, v1 = 0;
            float mn = static_cast<float>(sp.min), mx = static_cast<float>(sp.max), an = static_cast<float>(sp.anchor);
            switch (sp.facing) {
            case 0: u0 = x0 = mn; x1 = u1 = mx + 1; v0 = y0 = y1 = an; v1 = an + 1; break;
            case 1: v0 = an; v1 = an + 1; u0 = x0 = mn; x1 = u1 = mx + 1; y0 = y1 = an + 1; break;
            case 2: u0 = x0 = x1 = an; u1 = an + 1; v1 = y0 = mn; y1 = v0 = mx + 1; break;
            case 3: u0 = an; u1 = an + 1; x0 = x1 = an + 1; v1 = y0 = mn; y1 = v0 = mx + 1; break;
            }
            // LEFT/RIGHT: 原版 from.y > to.y, 顶点 v 随之上下颠倒; 这里按 min/max 存, 交换 v 保持一致
            if (sp.facing >= 2) std::swap(v0, v1);
            x0 *= sx; x1 *= sx; y0 = 16.0f - y0 * sy; y1 = 16.0f - y1 * sy;
            u0 *= sx; u1 *= sx; v0 *= sy; v1 *= sy;
            nlohmann::json face = { {"uv", {u0, v0, u1, v1}}, {"texture", ref}, {"tintindex", layer} };
            nlohmann::json el;
            // 原版 BlockElement(from, to) 只取各轴 min/max, 与 y0/y1 的先后无关
            el["from"] = { std::min(x0, x1), std::min(y0, y1), 7.5 };
            el["to"] = { std::max(x0, x1), std::max(y0, y1), 8.5 };
            el["faces"] = { {kDir[sp.facing], face} };
            elements.push_back(el);
        }
    }
    return elements;
}

// 按 tintindex 把面换到带固定色的材质变体上
void ApplyTints(ModelData& m, const std::vector<int>& tints) {
    if (tints.empty()) return;
    std::map<std::pair<int, int>, int> variant;
    for (auto& f : m.faces) {
        int t = f.tintIndex;
        if (t < 0 || t >= static_cast<int>(tints.size()) || tints[t] < 0) continue;
        int color = tints[t];
        auto key = std::make_pair(f.materialIndex, color);
        auto it = variant.find(key);
        if (it == variant.end()) {
            Material mat = m.materials[f.materialIndex];
            mat.tint.kind = TintKind::Fixed;
            mat.tint.color = static_cast<uint32_t>(color);
            mat.name += TintSuffix(mat.tint);
            m.materials.push_back(mat);
            it = variant.emplace(key, static_cast<int>(m.materials.size()) - 1).first;
        }
        f.materialIndex = it->second;
    }
}

bool BuildRef(const ModelRef& ref, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    ResolvedModel r = ResolveModel(ref.ns, ref.path);
    if (!r.ok || r.builtinEntity) return false;
    nlohmann::json model;
    nlohmann::json textures = nlohmann::json::object();
    if (!r.elements.is_null()) {
        for (const auto& [k, v] : r.textures) {
            std::string res = ResolveTexture(r, k);
            if (!res.empty()) textures[k] = res;
        }
        model["elements"] = r.elements;
    } else if (r.generated) {
        model["elements"] = GeneratedElements(r, textures);
    } else {
        return false;
    }
    model["textures"] = textures;
    ModelData m = ProcessModelData(model, "");
    if (m.faces.empty()) return false;
    ApplyTints(m, ref.tints);
    ItemRender::Transform(m, DisplayTransform(r, display, leftHand, scaleY));
    // 合并到 out
    int vBase = static_cast<int>(out.vertices.size() / 3), uBase = static_cast<int>(out.uvCoordinates.size() / 2);
    int mBase = static_cast<int>(out.materials.size());
    out.vertices.insert(out.vertices.end(), m.vertices.begin(), m.vertices.end());
    out.uvCoordinates.insert(out.uvCoordinates.end(), m.uvCoordinates.begin(), m.uvCoordinates.end());
    out.materials.insert(out.materials.end(), m.materials.begin(), m.materials.end());
    for (auto f : m.faces) {
        for (auto& i : f.vertexIndices) i += vBase;
        for (auto& i : f.uvIndices) i += uBase;
        f.materialIndex += mBase;
        f.faceDirection = DO_NOT_CULL;
        out.faces.push_back(f);
    }
    return true;
}

bool HasItemDefinitions() {
    static int cached = -1;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    if (cached < 0) cached = ItemRender::AssetJson("minecraft", "items/stone.json").is_object() ? 1 : 0;
    return cached == 1;
}

std::mutex g_buildMutex;
std::unordered_map<std::string, std::pair<bool, ModelData>> g_buildCache;
std::unordered_map<std::string, double> g_scaleCache;

// ---------------- 盾牌 / 三叉戟 / 头颅 ----------------
int DyeFromName(std::string s) {
    if (s.rfind("minecraft:", 0) == 0) s = s.substr(10);
    static const char* kNames[] = {
        "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
        "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
    for (int i = 0; i < 16; ++i) if (s == kNames[i]) return i;
    if (s == "grey") return 7;
    if (s == "light_grey") return 8;
    return -1;
}
int DyeFromTag(const NbtTagPtr& t) {
    if (!t) return -1;
    if (t->type == TagType::STRING) return DyeFromName(getStringTag(t));
    double v;
    if (Num(t, v)) {
        int i = static_cast<int>(v);
        if (i >= 0 && i < 16) return i;
    }
    return -1;
}
// 组件里的 id 既可能是字符串, 也可能是 {id:...} 持有者
std::string IdString(const NbtTagPtr& parent, const char* name) {
    auto t = getChildByName(parent, name);
    if (!t) return {};
    if (t->type == TagType::STRING) return getStringTag(t);
    if (t->type == TagType::COMPOUND) {
        std::string id = Str(t, "id");
        if (id.empty()) id = Str(t, "name");
        return id;
    }
    return {};
}
// 1.20.4 及更早的 BannerPattern hash 短码 -> 贴图名。未知短码原样返回, 贴图缺失时跳过该层。
std::string PatternAsset(std::string s) {
    if (s.rfind("minecraft:", 0) == 0) s = s.substr(10);
    if (s.empty()) return {};
    static const std::unordered_map<std::string, std::string> kShort = {
        {"b", "base"}, {"bl", "square_bottom_left"}, {"br", "square_bottom_right"},
        {"tl", "square_top_left"}, {"tr", "square_top_right"},
        {"bs", "stripe_bottom"}, {"ts", "stripe_top"}, {"ls", "stripe_left"}, {"rs", "stripe_right"},
        {"cs", "stripe_center"}, {"ms", "stripe_middle"}, {"drs", "stripe_downright"}, {"dls", "stripe_downleft"},
        {"ss", "small_stripes"}, {"cr", "cross"}, {"sc", "straight_cross"},
        {"bt", "triangle_bottom"}, {"tt", "triangle_top"}, {"bts", "triangles_bottom"}, {"tts", "triangles_top"},
        {"ld", "diagonal_left"}, {"rd", "diagonal_right"}, {"lud", "diagonal_up_left"}, {"rud", "diagonal_up_right"},
        {"mc", "circle"}, {"mr", "rhombus"}, {"vh", "half_vertical"}, {"hh", "half_horizontal"},
        {"vhr", "half_vertical_right"}, {"hhb", "half_horizontal_bottom"},
        {"bo", "border"}, {"cbo", "curly_border"}, {"cre", "creeper"},
        {"gra", "gradient"}, {"gru", "gradient_up"}, {"bri", "bricks"},
        {"sku", "skull"}, {"flo", "flower"}, {"moj", "mojang"}, {"glb", "globe"}, {"pig", "piglin"},
        {"flow", "flow"}, {"gus", "guster"} };
    auto it = kShort.find(s);
    if (it != kShort.end()) return it->second;
    for (char c : s) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '/')) return {};
    return s;
}
void ReadPatternList(const NbtTagPtr& list, std::vector<BannerPatternLayer>& out) {
    if (!list || list->type != TagType::LIST) return;
    for (const auto& layer : list->children) {
        if (out.size() >= 16) break;
        std::string pat = IdString(layer, "pattern");
        if (pat.empty()) pat = IdString(layer, "Pattern");
        pat = PatternAsset(pat);
        int dye = DyeFromTag(getChildByName(layer, "color"));
        if (dye < 0) dye = DyeFromTag(getChildByName(layer, "Color"));
        if (pat.empty() || dye < 0) continue;
        out.push_back({ pat, dye });
    }
}

bool TextureIndexed(const std::string& path) {
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    return GlobalCache::textureIndex.count("textures:minecraft:" + path) > 0;
}
// 写出贴图并返回 jar 导出后的绝对路径 (SaveTextureToFile 把第三个参数改成完整文件路径)。
std::string ExportTextureFull(const std::string& path) {
    std::string dir = "textures";
    if (!SaveTextureToFile("minecraft", path, dir)) return {};
    return dir;
}
std::string RelativeTexture(const std::string& path) {
    return "textures/minecraft/" + path + ".png";
}
std::string FirstTexture(std::initializer_list<const char*> paths) {
    for (const char* p : paths) if (TextureIndexed(p)) return p;
    return paths.size() ? *paths.begin() : std::string();
}
// DyeColor.getTextureDiffuseColor (与 EntityModelExporter::DyeRgb 同一张表)
int DyeDiffuse(int id) {
    static const int k[] = {
        0xF9FFFF, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
        0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21 };
    if (id < 0 || id > 15) id = 0;
    return k[id];
}
void TintMask(std::vector<unsigned char>& px, int rgb) {
    const int dr = (rgb >> 16) & 255, dg = (rgb >> 8) & 255, db = rgb & 255;
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        px[i] = static_cast<unsigned char>(px[i] * dr / 255);
        px[i + 1] = static_cast<unsigned char>(px[i + 1] * dg / 255);
        px[i + 2] = static_cast<unsigned char>(px[i + 2] * db / 255);
    }
}
// entityNoOutline 叠到已画好的底图上: src-over, 源颜色已乘过染料。
void CompositeOver(std::vector<unsigned char>& dst, const std::vector<unsigned char>& src) {
    const size_t n = std::min(dst.size(), src.size()) / 4;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char* s = src.data() + i * 4;
        unsigned char* d = dst.data() + i * 4;
        const int sa = s[3];
        if (sa == 0) continue;
        const int inv = 255 - sa;
        for (int c = 0; c < 3; ++c)
            d[c] = static_cast<unsigned char>((s[c] * sa + d[c] * inv) / 255);
        d[3] = static_cast<unsigned char>(sa + d[3] * inv / 255);
    }
}
std::string ShieldCacheHex(const std::string& key) {
    uint64_t h = 14695981039346656037ull;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    std::ostringstream o;
    o << std::hex << h;
    return o.str();
}

std::mutex g_bakeMutex;
std::unordered_map<std::string, std::string> g_shieldBake;  // key -> relative png

// 底图 shield_base + entity/shield/base(底色) + 每层 entity/shield/<pattern>。失败返回空。
std::string BakeShieldPlate(const std::string& basePath, int baseDye, const std::vector<BannerPatternLayer>& patterns) {
    std::string key = basePath + "|" + std::to_string(baseDye);
    for (const auto& p : patterns) key += "," + p.asset + ":" + std::to_string(p.dye);
    {
        std::lock_guard<std::mutex> lock(g_bakeMutex);
        auto it = g_shieldBake.find(key);
        if (it != g_shieldBake.end()) return it->second;
    }
    std::vector<unsigned char> base;
    int bw = 0, bh = 0;
    if (!LoadTexturePixels("minecraft", basePath, base, bw, bh) || bw <= 0 || bh <= 0) return {};
    auto addLayer = [&](const std::string& maskPath, int dye) {
        if (!TextureIndexed(maskPath)) return;
        std::vector<unsigned char> mask;
        int mw = 0, mh = 0;
        if (!LoadTexturePixels("minecraft", maskPath, mask, mw, mh) || mw != bw || mh != bh) return;
        TintMask(mask, DyeDiffuse(dye));
        CompositeOver(base, mask);
    };
    addLayer("entity/shield/base", baseDye);
    for (const auto& p : patterns) addLayer("entity/shield/" + p.asset, p.dye);

    const std::string fullBase = ExportTextureFull(basePath);
    if (fullBase.empty()) return {};
    const std::string file = "shield_" + ShieldCacheHex(key) + ".png";
    const std::filesystem::path out = std::filesystem::path(fullBase).parent_path() / file;
    std::error_code ec;
    std::filesystem::create_directories(out.parent_path(), ec);
    if (!stbi_write_png(out.string().c_str(), bw, bh, 4, base.data(), bw * 4)) return {};
    std::string rel = RelativeTexture(basePath);
    const auto slash = rel.find_last_of('/');
    if (slash != std::string::npos) rel = rel.substr(0, slash + 1) + file;
    else rel = file;
    std::lock_guard<std::mutex> lock(g_bakeMutex);
    g_shieldBake[key] = rel;
    return rel;
}

struct SkullDesc { const char* layer; const char* texture; bool dragon; bool piglin; };
bool LookupSkull(const std::string& id, SkullDesc& d) {
    if (id == "minecraft:skeleton_skull") d = { "minecraft:skeleton_skull#main", "entity/skeleton/skeleton", false, false };
    else if (id == "minecraft:wither_skeleton_skull") d = { "minecraft:wither_skeleton_skull#main", "entity/skeleton/wither_skeleton", false, false };
    else if (id == "minecraft:zombie_head") d = { "minecraft:zombie_head#main", "entity/zombie/zombie", false, false };
    else if (id == "minecraft:creeper_head") d = { "minecraft:creeper_head#main", "entity/creeper/creeper", false, false };
    else if (id == "minecraft:player_head") d = { "minecraft:player_head#main", "", false, false };
    else if (id == "minecraft:dragon_head") d = { "minecraft:dragon_skull#main", "entity/enderdragon/dragon", true, false };
    else if (id == "minecraft:piglin_head") d = { "minecraft:piglin_head#main", "entity/piglin/piglin", false, true };
    else return false;
    return true;
}
std::string PlayerTexture() {
    if (TextureIndexed("entity/player/wide/steve")) return "entity/player/wide/steve";
    if (TextureIndexed("entity/steve")) return "entity/steve";
    return "entity/player/wide/steve";
}
void AddMaterial(ModelData& mesh, const std::string& name, const std::string& rel) {
    Material mat;
    mat.name = name;
    mat.texturePath = rel;
    mesh.materials.push_back(mat);
}
int MaterialIndex(ModelData& mesh, const std::string& name, const std::string& rel) {
    for (size_t i = 0; i < mesh.materials.size(); ++i)
        if (mesh.materials[i].texturePath == rel && mesh.materials[i].name == name) return static_cast<int>(i);
    AddMaterial(mesh, name, rel);
    return static_cast<int>(mesh.materials.size()) - 1;
}

bool BuildShield(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    ResolvedModel r = ResolveModel("minecraft", "item/shield");
    if (!r.ok) return false;
    const bool modern = HasItemDefinitions();
    const std::string fam = modern ? "modern" : "legacy";
    EntityModel model(fam, "minecraft:shield#main");
    if (!model.Valid()) return false;
    const bool hasPatterns = item.bannerBase >= 0 || !item.bannerPatterns.empty();
    const char* baseModern = hasPatterns ? "entity/shield/shield_base" : "entity/shield/shield_base_nopattern";
    const char* baseLegacy = hasPatterns ? "entity/shield_base" : "entity/shield_base_nopattern";
    const std::string basePath = modern ? FirstTexture({ baseModern, baseLegacy }) : FirstTexture({ baseLegacy, baseModern });
    const std::string baseFull = ExportTextureFull(basePath);
    if (baseFull.empty()) return false;
    const std::string baseRel = RelativeTexture(basePath);
    std::string plateRel = baseRel;
    std::string plateName = "minecraft:" + basePath;
    if (hasPatterns) {
        const int baseDye = item.bannerBase >= 0 ? item.bannerBase : 0;  // 有图案但没底色: 白色
        std::string baked = BakeShieldPlate(basePath, baseDye, item.bannerPatterns);
        if (!baked.empty()) {
            plateRel = baked;
            plateName = "minecraft:" + basePath + "~" + ShieldCacheHex(baked);
        }
    }
    const int plateMat = MaterialIndex(out, plateName, plateRel);
    const int handleMat = MaterialIndex(out, "minecraft:" + basePath, baseRel);
    EntityAffine world = DisplayTransform(r, display, leftHand, scaleY) * EntityAffine::S(1.0, -1.0, -1.0);
    model.Emit(out, world, [&](int part) {
        const std::string& n = model.Name(part);
        if (n == "plate") return plateMat;
        if (n == "handle") return handleMat;
        return -1;
    });
    return !out.faces.empty();
}
bool BuildTrident(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    (void)item;
    if (display == ItemDisplay::Gui || display == ItemDisplay::Ground || display == ItemDisplay::Fixed) return false;
    ResolvedModel r = ResolveModel("minecraft", "item/trident_in_hand");
    if (!r.ok) return false;
    const bool modern = HasItemDefinitions();
    EntityModel model(modern ? "modern" : "legacy", "minecraft:trident#main");
    if (!model.Valid()) return false;
    // TridentModel.TEXTURE: 26.x entity/trident/trident, 1.21 entity/trident
    const std::string texPath = modern ? "entity/trident/trident" : "entity/trident";
    const std::string rel = RelativeTexture(texPath);
    if (ExportTextureFull(texPath).empty()) return false;
    const int mat = MaterialIndex(out, "minecraft:" + texPath, rel);
    EntityAffine world = DisplayTransform(r, display, leftHand, scaleY) * EntityAffine::S(1.0, -1.0, -1.0);
    model.Emit(out, world, [mat](int) { return mat; });
    return !out.faces.empty();
}
bool BuildSkullItem(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    const std::string modelPath = (item.id == "minecraft:dragon_head") ? "item/dragon_head" : "item/template_skull";
    ResolvedModel r = ResolveModel("minecraft", modelPath);
    if (!r.ok) return false;
    EntityAffine world = DisplayTransform(r, display, leftHand, scaleY) * EntityAffine::T(0.5, 0.0, 0.5);
    // 26.x: items/<skull>.json 的 special transformation = T(0.5,0,0.5) * left_rotation [1,0,0,0] (绕 X 180°),
    // 即 S(1,-1,-1)。1.21 的 S(-1,-1,1) + yaw π 在 EmitSkullModel 里, 结果相同。
    const bool modern = HasItemDefinitions();
    if (modern) world = world * EntityAffine::S(1.0, -1.0, -1.0);
    return ItemRender::EmitSkullModel(item.id, world, modern, out);
}
bool BuildSpecial(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* scaleY) {
    auto [ns, path] = SplitId(item.id);
    if (ns != "minecraft") return false;
    if (path == "shield") return BuildShield(item, display, leftHand, out, scaleY);
    if (path == "trident") return BuildTrident(item, display, leftHand, out, scaleY);
    SkullDesc d;
    if (LookupSkull(item.id, d)) return BuildSkullItem(item, display, leftHand, out, scaleY);
    return false;
}
}  // namespace

namespace ItemRender {

nlohmann::json AssetJson(const std::string& ns, const std::string& path) {
    const std::string key = ns + ":#" + path;
    {
        std::lock_guard<std::mutex> lock(g_jsonMutex);
        auto it = g_jsonCache.find(key);
        if (it != g_jsonCache.end()) return it->second;
    }
    nlohmann::json j;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto idx = GlobalCache::modelAssetIndex.find("modelassets:" + key);
        if (idx != GlobalCache::modelAssetIndex.end()) {
            auto it = GlobalCache::modelAssets.find(idx->second);
            if (it != GlobalCache::modelAssets.end()) {
                try { j = nlohmann::json::parse(it->second); } catch (...) { j = nlohmann::json(); }
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_jsonMutex);
    g_jsonCache[key] = j;
    return j;
}

ItemStackInfo ParseItemStack(const NbtTagPtr& tag) {
    ItemStackInfo s;
    if (!tag || tag->type != TagType::COMPOUND) return s;
    s.id = Str(tag, "id");
    if (s.id.empty()) return s;
    if (s.id.find(':') == std::string::npos) s.id = "minecraft:" + s.id;
    if (getChildByName(tag, "Count")) s.count = static_cast<int>(NumOr(tag, "Count", 1));   // <=1.20.4
    else s.count = static_cast<int>(NumOr(tag, "count", 1));                                // 1.20.5+ (缺省 1)
    auto takeTrim = [&](const NbtTagPtr& trim) {
        if (!trim) return;
        std::string m = IdString(trim, "material");
        std::string p = IdString(trim, "pattern");
        if (!m.empty()) s.trimMaterial = m;
        if (!p.empty()) s.trimPattern = p;
    };
    NbtTagPtr blockEntity;
    if (auto legacy = getChildByName(tag, "tag")) {                                         // <=1.20.4
        if (auto display = getChildByName(legacy, "display")) {
            double c; if (Num(getChildByName(display, "color"), c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;
        }
        takeTrim(getChildByName(legacy, "Trim"));
        blockEntity = getChildByName(legacy, "BlockEntityTag");
    }
    if (!blockEntity) blockEntity = getChildByName(tag, "BlockEntityTag");
    bool sawBase = false, sawPatterns = false;
    if (auto comps = getChildByName(tag, "components")) {                                    // 1.20.5+
        if (auto dye = getChildByName(comps, "minecraft:dyed_color")) {
            double c;
            if (Num(dye, c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;                    // 1.21.5+: int
            else if (Num(getChildByName(dye, "rgb"), c)) s.dyeColor = static_cast<int>(c) & 0xFFFFFF;
        }
        takeTrim(getChildByName(comps, "minecraft:trim"));
        if (auto eq = getChildByName(comps, "minecraft:equippable")) s.equipAsset = Str(eq, "asset_id");
        if (auto base = getChildByName(comps, "minecraft:base_color")) {
            sawBase = true;
            s.bannerBase = DyeFromTag(base);
        }
        if (auto pats = getChildByName(comps, "minecraft:banner_patterns")) {
            sawPatterns = true;
            ReadPatternList(pats, s.bannerPatterns);
        }
    }
    if (blockEntity) {
        if (!sawBase) {
            if (auto base = getChildByName(blockEntity, "Base")) s.bannerBase = DyeFromTag(base);
        }
        if (!sawPatterns) ReadPatternList(getChildByName(blockEntity, "Patterns"), s.bannerPatterns);
    }
    return s;
}

bool EmitSkullModel(const std::string& itemId, const EntityAffine& world, bool modern, ModelData& out) {
    SkullDesc d;
    if (!LookupSkull(itemId, d)) return false;
    const std::string texPath = d.texture[0] ? d.texture : PlayerTexture();
    if (ExportTextureFull(texPath).empty()) return false;
    EntityModel model(modern ? "modern" : "legacy", d.layer);
    if (!model.Valid()) return false;
    // setupAnim(animation=0). 1.21 renderSkull 把 yaw 设成 180°; 26.x 物品/佩戴都是 0。
    if (!modern) model.SetRot("head", 0.0f, static_cast<float>(kPi), 0.0f);
    if (d.dragon) model.SetRot("jaw", 0.2f, 0.0f, 0.0f);
    if (d.piglin) {
        model.SetRot("left_ear", 0.0f, 0.0f, -0.7f);
        model.SetRot("right_ear", 0.0f, 0.0f, 0.7f);
    }
    EntityAffine m = world;
    if (!modern) {
        m = m * EntityAffine::S(-1.0, -1.0, 1.0);
        if (d.dragon) m = m * EntityAffine::T(0.0, -0.374375, 0.0) * EntityAffine::S(0.75);
    }
    const int mat = MaterialIndex(out, "minecraft:" + texPath, RelativeTexture(texPath));
    model.Emit(out, m, [mat](int) { return mat; });
    return !out.faces.empty();
}

void Transform(ModelData& mesh, const EntityAffine& a) {
    for (size_t i = 0; i + 2 < mesh.vertices.size(); i += 3) {
        double p[3] = { mesh.vertices[i], mesh.vertices[i + 1], mesh.vertices[i + 2] }, w[3];
        a.Apply(p, w);
        mesh.vertices[i] = static_cast<float>(w[0]);
        mesh.vertices[i + 1] = static_cast<float>(w[1]);
        mesh.vertices[i + 2] = static_cast<float>(w[2]);
    }
    if (a.Det() < 0.0)
        for (auto& f : mesh.faces) {
            std::reverse(f.vertexIndices.begin(), f.vertexIndices.end());
            std::reverse(f.uvIndices.begin(), f.uvIndices.end());
        }
}

bool Build(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out, double* groundScaleY) {
    if (item.empty()) return false;
    std::string bannerKey;
    if (item.bannerBase >= 0 || !item.bannerPatterns.empty()) {
        bannerKey = "|ban:" + std::to_string(item.bannerBase);
        for (const auto& p : item.bannerPatterns) bannerKey += "," + p.asset + ":" + std::to_string(p.dye);
    }
    const std::string key = item.id + "|" + DisplayName(display) + (leftHand ? "|L|" : "|R|") +
                            std::to_string(item.dyeColor) + "|" + item.trimMaterial + bannerKey;
    {
        std::lock_guard<std::mutex> lock(g_buildMutex);
        auto it = g_buildCache.find(key);
        if (it != g_buildCache.end()) {
            if (groundScaleY) *groundScaleY = g_scaleCache[key];
            if (!it->second.first) return false;
            out = it->second.second;
            return true;
        }
    }
    auto [ns, path] = SplitId(item.id);
    std::vector<ModelRef> refs;
    bool handled = false;
    if (HasItemDefinitions()) {
        nlohmann::json def = AssetJson(ns, "items/" + path + ".json");
        if (def.is_object() && def.contains("model")) {
            handled = true;
            ResolveItemDef(def["model"], item, display, refs);
        }
    }
    if (!handled) {
        std::string p = "item/" + path;
        bool inHand = display != ItemDisplay::Gui && display != ItemDisplay::Ground && display != ItemDisplay::Fixed;
        if (ns == "minecraft" && path == "spyglass" && inHand) p = "item/spyglass_in_hand";
        if (ns == "minecraft" && path == "trident" && inHand) p = "item/trident_in_hand";  // builtin/entity, 下面走 3D
        refs.push_back({ ns, p, LegacyTints(item.id, item) });
    }
    ModelData m;
    double scaleY = 1.0;
    bool ok = false;
    for (const auto& r : refs) ok = BuildRef(r, display, leftHand, m, &scaleY) || ok;
    if (!ok) ok = BuildSpecial(item, display, leftHand, m, &scaleY);
    std::lock_guard<std::mutex> lock(g_buildMutex);
    g_buildCache[key] = { ok, m };
    g_scaleCache[key] = scaleY;
    if (groundScaleY) *groundScaleY = scaleY;
    if (!ok) return false;
    out = std::move(m);
    return true;
}

}  // namespace ItemRender
