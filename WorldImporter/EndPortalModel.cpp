#include "EndPortalModel.h"
#include "fileutils.h"
#include "texture.h"
#include "include/stb_image_write.h"

#include <Windows.h>
#undef min
#undef max

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

namespace {

// rendertype_end_portal.fsh 的 COLORS（GameTime = 0）
constexpr double kColors[16][3] = {
    {0.022087, 0.098399, 0.110818},
    {0.011892, 0.095924, 0.089485},
    {0.027636, 0.101689, 0.100326},
    {0.046564, 0.109883, 0.114838},
    {0.064901, 0.117696, 0.097189},
    {0.063761, 0.086895, 0.123646},
    {0.084817, 0.111994, 0.166380},
    {0.097489, 0.154120, 0.091064},
    {0.106152, 0.131144, 0.195191},
    {0.097721, 0.110188, 0.187229},
    {0.133516, 0.138278, 0.148582},
    {0.070006, 0.243332, 0.235792},
    {0.196766, 0.142899, 0.214696},
    {0.047281, 0.315338, 0.321970},
    {0.204675, 0.390010, 0.302066},
    {0.080955, 0.314821, 0.661491},
};

// 面 UV 0..1 对应屏幕空间的 0.25（与 endportal_bake_ref.py 的 K 一致）
constexpr int kBakeSize = 256;
constexpr double kScreenK = 0.25;

string ExeDirectory() {
    wchar_t buffer[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) return {};
    string exePath = wstring_to_string(wstring(buffer));
    size_t pos = exePath.find_last_of("\\/");
    return pos == string::npos ? string() : exePath.substr(0, pos);
}

void EnsureDir(const string& path) {
    if (path.empty()) return;
    wstring wpath = string_to_wstring(path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wpath.c_str(), nullptr);
    }
}

// floor(x) 再按 Python 规则取正余数（最近邻 + repeat）
int WrapFloor(double x, int size) {
    long long i = static_cast<long long>(floor(x));
    long long m = i % static_cast<long long>(size);
    if (m < 0) m += size;
    return static_cast<int>(m);
}

void SampleNearest(const vector<unsigned char>& rgba, int w, int h,
                   double u, double v, double& r, double& g, double& b) {
    int x = WrapFloor(u * w, w);
    int y = WrapFloor(v * h, h);
    size_t i = (static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4;
    r = rgba[i] / 255.0;
    g = rgba[i + 1] / 255.0;
    b = rgba[i + 2] / 255.0;
}

bool LoadSource(const string& ns, const string& path, vector<unsigned char>& rgba, int& w, int& h) {
    rgba.clear();
    w = 0;
    h = 0;
    return LoadTexturePixels(ns, path, rgba, w, h) && w > 0 && h > 0;
}

// 烘焙一张 256x256 RGBA。layers: 传送门 15，折跃门 16。
bool WriteBake(int layers, const string& stem, string& outRel) {
    const string ns = "minecraft";
    vector<unsigned char> sky, por;
    int sw = 0, sh = 0, pw = 0, ph = 0;
    if (!LoadSource(ns, "environment/end_sky", sky, sw, sh)) return false;
    // 26.x: entity/end_portal/end_portal；1.21 及更早: entity/end_portal
    if (!LoadSource(ns, "entity/end_portal/end_portal", por, pw, ph) &&
        !LoadSource(ns, "entity/end_portal", por, pw, ph)) {
        return false;
    }
    if (layers < 1 || layers > 16) return false;

    struct LayerXform { double c, sn, s, tx; };
    LayerXform xforms[16];
    for (int i = 0; i < layers; ++i) {
        double l = i + 1.0;
        double a = (l * l * 4321.0 + l * 9.0) * 2.0 * (3.14159265358979323846 / 180.0);
        xforms[i].c = cos(a);
        xforms[i].sn = sin(a);
        xforms[i].s = (4.5 - l / 4.0) * 2.0;
        xforms[i].tx = 17.0 / l;
    }

    const int N = kBakeSize;
    vector<unsigned char> img(static_cast<size_t>(N) * N * 4);
    for (int y = 0; y < N; ++y) {
        double v = (y + 0.5) / static_cast<double>(N) * kScreenK;
        for (int x = 0; x < N; ++x) {
            double u = (x + 0.5) / static_cast<double>(N) * kScreenK;
            double r, g, b;
            SampleNearest(sky, sw, sh, u, v, r, g, b);
            r *= kColors[0][0];
            g *= kColors[0][1];
            b *= kColors[0][2];
            for (int i = 0; i < layers; ++i) {
                const LayerXform& xf = xforms[i];
                double u1 = xf.s * (u * xf.c - v * xf.sn) + xf.tx;
                double v1 = xf.s * (u * xf.sn + v * xf.c);
                double u2 = 0.5 * u1 + 0.25;
                double v2 = 0.5 * v1 + 0.25;
                double pr, pg, pb;
                SampleNearest(por, pw, ph, u2, v2, pr, pg, pb);
                r += pr * kColors[i][0];
                g += pg * kColors[i][1];
                b += pb * kColors[i][2];
            }
            auto quant = [](double c) {
                if (c < 0.0) c = 0.0;
                else if (c > 1.0) c = 1.0;
                return static_cast<unsigned char>(c * 255.0);
            };
            size_t idx = (static_cast<size_t>(y) * N + static_cast<size_t>(x)) * 4;
            img[idx] = quant(r);
            img[idx + 1] = quant(g);
            img[idx + 2] = quant(b);
            img[idx + 3] = 255;
        }
    }

    string exeDir = ExeDirectory();
    if (exeDir.empty()) return false;
    string texRoot = JoinPath(exeDir, "textures");
    string nsDir = JoinPath(texRoot, ns);
    string entityDir = JoinPath(nsDir, "entity");
    string portalDir = JoinPath(entityDir, "end_portal");
    EnsureDir(texRoot);
    EnsureDir(nsDir);
    EnsureDir(entityDir);
    EnsureDir(portalDir);

    string texPath = "entity/end_portal/" + stem;
    string rel = "textures/" + ns + "/" + texPath + ".png";
    string full = JoinPath(portalDir, stem + ".png");
    if (!stbi_write_png(full.c_str(), N, N, 4, img.data(), N * 4)) return false;
    RegisterTexture(ns, texPath, rel);
    outRel = rel;
    return true;
}

// 每种贴图进程内只烘焙一次；工作线程并发调用。
string BakedTexture(bool gateway) {
    static once_flag oncePortal;
    static once_flag onceGateway;
    static mutex bakeMutex;
    static string relPortal;
    static string relGateway;
    static bool okPortal = false;
    static bool okGateway = false;

    if (gateway) {
        call_once(onceGateway, []() {
            lock_guard<mutex> lock(bakeMutex);
            okGateway = WriteBake(16, "baked_end_gateway", relGateway);
        });
        return okGateway ? relGateway : string();
    }
    call_once(oncePortal, []() {
        lock_guard<mutex> lock(bakeMutex);
        okPortal = WriteBake(15, "baked_end_portal", relPortal);
    });
    return okPortal ? relPortal : string();
}

void AddQuad(ModelData& model, FaceType dir,
             float x0, float y0, float z0,
             float x1, float y1, float z1,
             float x2, float y2, float z2,
             float x3, float y3, float z3) {
    int vi = static_cast<int>(model.vertices.size() / 3);
    int ui = static_cast<int>(model.uvCoordinates.size() / 2);
    float verts[] = { x0, y0, z0, x1, y1, z1, x2, y2, z2, x3, y3, z3 };
    // 与 model.cpp 整面 UV 一致：(1,0) (1,1) (0,1) (0,0)
    float uvs[] = { 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    model.vertices.insert(model.vertices.end(), verts, verts + 12);
    model.uvCoordinates.insert(model.uvCoordinates.end(), uvs, uvs + 8);

    Face face;
    face.vertexIndices = { vi, vi + 1, vi + 2, vi + 3 };
    face.uvIndices = { ui, ui + 1, ui + 2, ui + 3 };
    face.materialIndex = 0;
    face.faceDirection = dir;
    face.tintIndex = -1;
    model.faces.push_back(face);
}

}  // namespace

ModelData GenerateEndPortalModel(const string& blockName) {
    string processed = blockName;
    size_t bracket = processed.find('[');
    if (bracket != string::npos) processed = processed.substr(0, bracket);

    string ns = "minecraft";
    string id = processed;
    size_t colon = id.find(':');
    if (colon != string::npos) {
        ns = id.substr(0, colon);
        id = id.substr(colon + 1);
    }
    if (ns != "minecraft") return {};

    bool gateway = false;
    if (id == "end_gateway") gateway = true;
    else if (id != "end_portal") return {};

    string rel = BakedTexture(gateway);
    if (rel.empty()) return {};

    ModelData model;
    Material material;
    material.name = gateway ? "minecraft:end_gateway" : "minecraft:end_portal";
    material.texturePath = rel;
    material.tintIndex = -1;
    model.materials.push_back(material);

    if (!gateway) {
        // 只画 Y 轴两面，且不在方块边界上，不参与邻块剔除
        const float yUp = 0.75f;
        const float yDn = 0.375f;
        AddQuad(model, FaceType::DO_NOT_CULL,
                1.0f, yUp, 1.0f, 1.0f, yUp, 0.0f, 0.0f, yUp, 0.0f, 0.0f, yUp, 1.0f);
        AddQuad(model, FaceType::DO_NOT_CULL,
                1.0f, yDn, 0.0f, 1.0f, yDn, 1.0f, 0.0f, yDn, 1.0f, 0.0f, yDn, 0.0f);
    } else {
        // 单位立方体；cullface 与法线一致，旁边的基岩能剔掉这些面
        AddQuad(model, FaceType::NORTH, 0, 0, 0,  0, 1, 0,  1, 1, 0,  1, 0, 0);
        AddQuad(model, FaceType::SOUTH, 1, 0, 1,  1, 1, 1,  0, 1, 1,  0, 0, 1);
        AddQuad(model, FaceType::EAST,  1, 0, 0,  1, 1, 0,  1, 1, 1,  1, 0, 1);
        AddQuad(model, FaceType::WEST,  0, 0, 1,  0, 1, 1,  0, 1, 0,  0, 0, 0);
        AddQuad(model, FaceType::UP,    1, 1, 1,  1, 1, 0,  0, 1, 0,  0, 1, 1);
        AddQuad(model, FaceType::DOWN,  1, 0, 0,  1, 0, 1,  0, 0, 1,  0, 0, 0);
    }
    return model;
}
