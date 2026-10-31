#include "BannerModel.h"
#include "EntityCube.h"
#include "fileutils.h"
#include "include/stb_image_write.h"
#include <Windows.h>
#include <sstream>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <cstdio>

using namespace std;
using namespace entitycube;

namespace {
    const char* kDyes[16] = { "white", "orange", "magenta", "light_blue", "yellow", "lime", "pink", "gray",
                              "light_gray", "cyan", "purple", "blue", "brown", "green", "red", "black" };
    // DyeColor.textureDiffuseColor
    const unsigned kDyeRgb[16] = { 0xF9FFFE, 0xF9801D, 0xC74EBD, 0x3AB3DA, 0xFED83D, 0x80C71F, 0xF38BAA, 0x474F52,
                                   0x9D9D97, 0x169C9C, 0x8932B8, 0x3C44AA, 0x835432, 0x5E7C16, 0xB02E26, 0x1D1D21 };
    int DyeIndex(const string& name) {
        for (int i = 0; i < 16; ++i) if (name == kDyes[i]) return i;
        return -1;
    }
    // 1.20.4 及更早: Patterns[].Pattern 为短代码
    const char* kShortCodes[][2] = {
        {"b","base"},{"bl","square_bottom_left"},{"br","square_bottom_right"},{"tl","square_top_left"},
        {"tr","square_top_right"},{"bs","stripe_bottom"},{"ts","stripe_top"},{"ls","stripe_left"},
        {"rs","stripe_right"},{"cs","stripe_center"},{"ms","stripe_middle"},{"drs","stripe_downright"},
        {"dls","stripe_downleft"},{"ss","small_stripes"},{"cr","cross"},{"sc","straight_cross"},
        {"bt","triangle_bottom"},{"tt","triangle_top"},{"bts","triangles_bottom"},{"tts","triangles_top"},
        {"ld","diagonal_left"},{"rd","diagonal_up_right"},{"lud","diagonal_up_left"},{"rud","diagonal_right"},
        {"mc","circle"},{"mr","rhombus"},{"vh","half_vertical"},{"hh","half_horizontal"},
        {"vhr","half_vertical_right"},{"hhb","half_horizontal_bottom"},{"bo","border"},{"cbo","curly_border"},
        {"gra","gradient"},{"gru","gradient_up"},{"bri","bricks"},{"glb","globe"},{"cre","creeper"},
        {"sku","skull"},{"flo","flower"},{"moj","mojang"},{"pig","piglin"},{"flw","flow"},{"gus","guster"},
    };
    string StripNs(const string& s) { size_t c = s.find(':'); return c == string::npos ? s : s.substr(c + 1); }

    struct Layer { string pattern; int color; };

    int ReadInt(const NbtTagPtr& t) {
        if (!t) return -1;
        switch (t->type) {
        case TagType::BYTE: return bytesToByte(t->payload);
        case TagType::SHORT: return bytesToShort(t->payload);
        case TagType::INT: return bytesToInt(t->payload);
        default: return -1;
        }
    }

    vector<Layer> ReadPatterns(const NbtTagPtr& be) {
        vector<Layer> out;
        if (!be) return out;
        NbtTagPtr list = getChildByName(be, "patterns");   // 1.20.5+
        bool modern = list != nullptr;
        if (!list) list = getChildByName(be, "Patterns");  // 旧格式
        if (!list) return out;
        for (const NbtTagPtr& e : getChildren(list)) {
            if (out.size() >= 16) break;  // 原版最多渲染 16 层
            Layer l; l.color = -1;
            if (modern) {
                NbtTagPtr p = getChildByName(e, "pattern"), c = getChildByName(e, "color");
                if (p && p->type == TagType::STRING) l.pattern = StripNs(getStringTag(p));
                else if (p && p->type == TagType::COMPOUND) {  // 内联定义: asset_id
                    NbtTagPtr a = getChildByName(p, "asset_id");
                    if (a) l.pattern = StripNs(getStringTag(a));
                }
                if (c && c->type == TagType::STRING) l.color = DyeIndex(getStringTag(c));
            } else {
                NbtTagPtr p = getChildByName(e, "Pattern"), c = getChildByName(e, "Color");
                string code = p ? getStringTag(p) : "";
                for (auto& sc : kShortCodes) if (code == sc[0]) { l.pattern = sc[1]; break; }
                l.color = ReadInt(c);
            }
            if (!l.pattern.empty() && l.color >= 0 && l.color < 16) out.push_back(l);
        }
        return out;
    }

    string ExeDirectory() {
        wchar_t buffer[MAX_PATH];
        GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        string exePath = wstring_to_string(wstring(buffer));
        size_t pos = exePath.find_last_of("\\/");
        return pos == string::npos ? string() : exePath.substr(0, pos);
    }

    // 底色 + 图案按原版 bannerPattern(半透明混合)逐层叠到旗面; 返回贴图路径(entity/banner/xxx)
    string BakeFlagTexture(const string& ns, int baseColor, const vector<Layer>& layers) {
        string name = string("baked_") + kDyes[baseColor];
        unsigned long long h = 1469598103934665603ULL;
        for (const Layer& l : layers) {
            for (char ch : l.pattern + "/" + to_string(l.color) + ";") { h ^= (unsigned char)ch; h *= 1099511628211ULL; }
        }
        if (!layers.empty()) { char buf[24]; snprintf(buf, sizeof(buf), "_%016llx", h); name += buf; }
        string texPath = "entity/banner/" + name;

        static mutex mtx;
        static unordered_set<string> done;
        lock_guard<mutex> lock(mtx);
        if (done.count(texPath)) return texPath;

        vector<unsigned char> img; int w = 0, hgt = 0;
        if (!LoadTexturePixels(ns, "entity/banner/base", img, w, hgt) || w <= 0 || hgt <= 0) return "";
        // 先导出 base 图案以建立 textures/<ns>/entity/banner 目录
        string dir = "textures";
        SaveTextureToFile(ns, "entity/banner/base", dir);
        // 原版先画 banner_base, 再叠底色层与图案层
        vector<unsigned char> out((size_t)w * hgt * 4, 0);
        {
            vector<unsigned char> bp; int bw = 0, bh = 0;
            if ((LoadTexturePixels(ns, "entity/banner/banner_base", bp, bw, bh) ||
                 LoadTexturePixels(ns, "entity/banner_base", bp, bw, bh)) && bw == w && bh == hgt) out = bp;
        }
        auto blend = [&](const vector<unsigned char>& px, int pw, int ph, int color) {
            if (pw != w || ph != hgt) return;
            float cr = ((kDyeRgb[color] >> 16) & 255) / 255.0f, cg = ((kDyeRgb[color] >> 8) & 255) / 255.0f, cb = (kDyeRgb[color] & 255) / 255.0f;
            for (size_t i = 0; i < (size_t)w * hgt; ++i) {
                float a = px[i * 4 + 3] / 255.0f;
                if (a <= 0.0f) continue;
                float src[3] = { px[i * 4] / 255.0f * cr, px[i * 4 + 1] / 255.0f * cg, px[i * 4 + 2] / 255.0f * cb };
                for (int k = 0; k < 3; ++k) out[i * 4 + k] = (unsigned char)(src[k] * 255.0f * a + out[i * 4 + k] * (1.0f - a) + 0.5f);
                float oa = out[i * 4 + 3] / 255.0f;
                out[i * 4 + 3] = (unsigned char)((a + oa * (1.0f - a)) * 255.0f + 0.5f);
            }
        };
        blend(img, w, hgt, baseColor);
        for (const Layer& l : layers) {
            vector<unsigned char> px; int pw = 0, ph = 0;
            if (LoadTexturePixels(ns, "entity/banner/" + l.pattern, px, pw, ph)) blend(px, pw, ph, l.color);
        }
        string full = JoinPath({ ExeDirectory(), "textures", ns, "entity", "banner", name + ".png" });
        if (!stbi_write_png(full.c_str(), w, hgt, 4, out.data(), w * 4)) return "";
        RegisterTexture(ns, texPath, "textures/" + ns + "/" + texPath + ".png");
        done.insert(texPath);
        return texPath;
    }
}

bool TryGenerateBannerModel(const string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& outModel) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return false;
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);
    auto endsWith = [&](const string& suf) { return blockID.size() > suf.size() && blockID.compare(blockID.size() - suf.size(), suf.size(), suf) == 0; };
    bool wall;
    string color;
    if (endsWith("_wall_banner")) { wall = true; color = blockID.substr(0, blockID.size() - 12); }
    else if (endsWith("_banner")) { wall = false; color = blockID.substr(0, blockID.size() - 7); }
    else return false;
    int baseColor = DyeIndex(color);
    if (baseColor < 0) return false;

    int rotation = 0; string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1);
        if (!statePart.empty() && statePart.back() == ']') statePart.pop_back();
        stringstream ss(statePart); string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "rotation") { try { rotation = stoi(v); } catch (...) {} }
            else if (k == "facing") facing = v;
        }
    }

    ModelData model;
    // 旗杆/横杆: banner_base(26.x 移到 entity/banner/banner_base)
    int baseMat = AddMaterial(model, ns, ns + ":" + blockID, "entity/banner/banner_base");
    if (baseMat < 0) { model.materials.clear(); baseMat = AddMaterial(model, ns, ns + ":" + blockID, "entity/banner_base"); }
    if (baseMat < 0) return false;
    string flagTex = BakeFlagTexture(ns, baseColor, ReadPatterns(blockEntityNbt));
    int flagMat = baseMat;
    if (!flagTex.empty()) {
        Material m;
        m.name = ns + ":" + blockID + "#flag~" + flagTex.substr(flagTex.rfind('/') + 1);
        m.texturePath = "textures/" + ns + "/" + flagTex + ".png";
        m.tintIndex = -1;
        model.materials.push_back(m);
        flagMat = (int)model.materials.size() - 1;
    }

    // BannerRenderer: T(.5,0,.5) RY(-angle) S(2/3,-2/3,-2/3); 地面 angle=rot*22.5, 墙上 angle=facing.toYRot
    float angle;
    if (wall) {
        angle = 0.0f;
        if (facing == "west") angle = 90.0f; else if (facing == "north") angle = 180.0f; else if (facing == "east") angle = 270.0f;
    } else {
        angle = rotation * 22.5f;
    }
    const float k = 2.0f / 3.0f;
    Aff root = T(0.5f, 0.0f, 0.5f) * RY(Deg(-angle)) * S(k, -k, -k);
    if (!wall) AddCube(model, baseMat, root, 44, 0, -1, -42, -1, 2, 42, 2, 0, 64, 64);           // pole
    AddCube(model, baseMat, root, 0, 42, -10, wall ? -20.5f : -44.0f, wall ? 9.5f : -1.0f, 20, 2, 2, 0, 64, 64);  // bar
    // flag: offset(0, -44 / -20.5, 0 / 10.5), xRot 取 phase=0 时的静止角
    const float flagRot = (-0.0125f + 0.01f) * kPi;
    AddCube(model, flagMat, root * Pose(0, wall ? -20.5f : -44.0f, wall ? 10.5f : 0.0f, flagRot), 0, 0, -10, 0, -2, 20, 40, 1, 0, 64, 64);
    outModel = std::move(model);
    return true;
}
