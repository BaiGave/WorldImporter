#include "BeaconBeam.h"

#include "Occlusion.h"
#include "block.h"
#include "config.h"
#include "fileutils.h"
#include "texture.h"
#include "include/stb_image_write.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// DyeColor.getTextureDiffuseColor() (1.21+ / 26.3). Not the text color.
// WHITE is 16383998 (0xF9FFFE), not 0xFFFFFF. Alpha of these ints is 0;
// ARGB32.average still averages that alpha, and the solid beam is drawn
// with blending off, so the bake keeps the texture alpha.
constexpr int kDyeWhite = 16383998;
constexpr int kDyeOrange = 16351261;
constexpr int kDyeMagenta = 13061821;
constexpr int kDyeLightBlue = 3847130;
constexpr int kDyeYellow = 16701501;
constexpr int kDyeLime = 8439583;
constexpr int kDyePink = 15961002;
constexpr int kDyeGray = 4673362;
constexpr int kDyeLightGray = 10329495;
constexpr int kDyeCyan = 1481884;
constexpr int kDyePurple = 8991416;
constexpr int kDyeBlue = 3949738;
constexpr int kDyeBrown = 8606770;
constexpr int kDyeGreen = 6192150;
constexpr int kDyeRed = 11546150;
constexpr int kDyeBlack = 1908001;

// 26.3 BeaconRenderer.MAX_RENDER_Y. The region clip (config.maxY + 1) binds
// first for a normal export; this is only the vanilla height parameter.
constexpr int kMaxRenderY = 2048;
constexpr float kBeamRadius = 0.2f;
constexpr float kGlowRadius = 0.25f;
constexpr float kTextureScale = 1.0f;
// Java (float)Math.PI / 180. Do not use M_PI (MSVC).
constexpr float kDegToRad = 0.01745329251994329577f;

struct Section {
    int color = 0;
    int height = 0;
    int start = 0; // local Y of the section bottom (0 = beacon block)
};

struct XZ {
    float x;
    float z;
};

struct BakePaths {
    bool ok = false;
    std::string solidName;
    std::string glowName;
    std::string solidPath;
    std::string glowPath;
};

std::mutex g_bakeMutex;
std::unordered_map<int, BakePaths> g_bakeCache;
std::vector<unsigned char> g_srcPixels;
int g_srcW = 0;
int g_srcH = 0;
int g_srcState = 0; // 0 unknown, 1 loaded, -1 missing
std::string g_exeDir;
bool g_dirsReady = false;

std::mutex g_logMutex;

int AverageColor(int a, int b) {
    const int aa = (a >> 24) & 255;
    const int ar = (a >> 16) & 255;
    const int ag = (a >> 8) & 255;
    const int ab = a & 255;
    const int ba = (b >> 24) & 255;
    const int br = (b >> 16) & 255;
    const int bg = (b >> 8) & 255;
    const int bb = b & 255;
    return (((aa + ba) / 2) << 24) | (((ar + br) / 2) << 16) |
           (((ag + bg) / 2) << 8) | ((ab + bb) / 2);
}

std::string Hex6(int color) {
    std::ostringstream os;
    os << std::hex << std::nouppercase << std::setw(6) << std::setfill('0')
       << (color & 0xFFFFFF);
    return os.str();
}

bool EndsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

void BlockParts(int id, std::string& ns, std::string& local) {
    Block block = GetBlockById(id);
    std::string name = block.name;
    const size_t bracket = name.find('[');
    if (bracket != std::string::npos) name.resize(bracket);
    const size_t colon = name.find(':');
    if (colon == std::string::npos) {
        ns = "minecraft";
        local = std::move(name);
    } else {
        ns = name.substr(0, colon);
        local = name.substr(colon + 1);
    }
}

int DyeOf(const std::string& dye) {
    if (dye == "white") return kDyeWhite;
    if (dye == "orange") return kDyeOrange;
    if (dye == "magenta") return kDyeMagenta;
    if (dye == "light_blue") return kDyeLightBlue;
    if (dye == "yellow") return kDyeYellow;
    if (dye == "lime") return kDyeLime;
    if (dye == "pink") return kDyePink;
    if (dye == "gray") return kDyeGray;
    if (dye == "light_gray") return kDyeLightGray;
    if (dye == "cyan") return kDyeCyan;
    if (dye == "purple") return kDyePurple;
    if (dye == "blue") return kDyeBlue;
    if (dye == "brown") return kDyeBrown;
    if (dye == "green") return kDyeGreen;
    if (dye == "red") return kDyeRed;
    if (dye == "black") return kDyeBlack;
    return -1;
}

// BeaconBeamBlock: the beacon itself (white) plus every *_stained_glass
// and *_stained_glass_pane. Checked before occlusion so glass is never
// treated as a solid block that clears the beam.
bool TryBeamColor(int id, int& color) {
    if (id <= 0) return false;
    std::string ns;
    std::string local;
    BlockParts(id, ns, local);
    if (ns == "minecraft" && local == "beacon") {
        color = kDyeWhite;
        return true;
    }
    const char* pane = "_stained_glass_pane";
    const char* glass = "_stained_glass";
    if (EndsWith(local, pane)) {
        const int dye = DyeOf(local.substr(0, local.size() - std::char_traits<char>::length(pane)));
        if (dye < 0) return false;
        color = dye;
        return true;
    }
    if (EndsWith(local, glass)) {
        const int dye = DyeOf(local.substr(0, local.size() - std::char_traits<char>::length(glass)));
        if (dye < 0) return false;
        color = dye;
        return true;
    }
    return false;
}

bool IsBedrock(int id) {
    if (id <= 0) return false;
    std::string ns;
    std::string local;
    BlockParts(id, ns, local);
    return ns == "minecraft" && local == "bedrock";
}

bool IsBaseBlock(int id) {
    if (id <= 0) return false;
    std::string ns;
    std::string local;
    BlockParts(id, ns, local);
    if (ns != "minecraft") return false;
    return local == "iron_block" || local == "gold_block" || local == "emerald_block" ||
           local == "diamond_block" || local == "netherite_block";
}

// Vanilla: for (step = 1; step <= 4; levels = step++) — a failed layer
// breaks before the update, so it does not count.
int ComputeBaseLevels(int x, int y, int z) {
    int levels = 0;
    for (int step = 1; step <= 4; ++step) {
        const int layerY = y - step;
        bool ok = true;
        for (int dx = -step; dx <= step && ok; ++dx) {
            for (int dz = -step; dz <= step; ++dz) {
                if (!IsBaseBlock(GetBlockId(x + dx, layerY, z + dz))) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok) break;
        levels = step;
    }
    return levels;
}

// size() <= 1 always appends a new section (even when the color matches).
// Later blocks average with the previous section color. No reference into
// the vector is held across push_back.
void AddBeamColor(std::vector<Section>& sections, int color) {
    if (sections.size() <= 1) {
        int start = 0;
        if (!sections.empty()) start = sections.back().start + sections.back().height;
        sections.push_back(Section{color, 1, start});
        return;
    }
    const int lastColor = sections.back().color;
    if (color == lastColor) {
        sections.back().height += 1;
        return;
    }
    const int start = sections.back().start + sections.back().height;
    sections.push_back(Section{AverageColor(lastColor, color), 1, start});
}

// Beacon block is the first beam block (white, height 1). Then y+1 upward.
// A fully opaque non-bedrock block clears every section. Other blocks
// extend the current section. id 0 (air or an unloaded section) extends.
void ScanSections(std::vector<Section>& sections, int x, int y, int z) {
    sections.clear();
    AddBeamColor(sections, kDyeWhite);

    int scanTop = std::max(config.maxY, y + 512);
    const int hardTop = y + 4096;
    if (scanTop > hardTop) scanTop = hardTop;

    for (int wy = y + 1; wy <= scanTop; ++wy) {
        const int id = GetBlockId(x, wy, z);
        int color = 0;
        if (TryBeamColor(id, color)) {
            AddBeamColor(sections, color);
            continue;
        }
        // last == null || (lightBlock >= 15 && !bedrock). occludes approximates
        // a fully opaque full cube (light dampening >= 15).
        if (sections.empty() || (GetBlockOcclusion(id).occludes && !IsBedrock(id))) {
            sections.clear();
            break;
        }
        sections.back().height += 1;
    }
}

std::string ExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    const std::string exePath = wstring_to_string(std::wstring(buffer));
    const size_t pos = exePath.find_last_of("\\/");
    return pos == std::string::npos ? exePath : exePath.substr(0, pos);
}

void EnsureDir(const std::string& path) {
    const std::wstring wpath = string_to_wstring(path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wpath.c_str(), nullptr);
    }
}

bool EnsureSourceLocked() {
    if (g_srcState != 0) return g_srcState > 0;
    int w = 0;
    int h = 0;
    std::vector<unsigned char> pixels;
    // 26.x path first, then the <=1.21 path.
    if (!LoadTexturePixels("minecraft", "entity/beacon/beacon_beam", pixels, w, h)) {
        pixels.clear();
        w = 0;
        h = 0;
        if (!LoadTexturePixels("minecraft", "entity/beacon_beam", pixels, w, h)) {
            g_srcState = -1;
            std::cerr << "beacon beam: missing beacon_beam texture\n";
            return false;
        }
    }
    if (w <= 0 || h <= 0 || pixels.size() != static_cast<size_t>(w) * static_cast<size_t>(h) * 4) {
        g_srcState = -1;
        std::cerr << "beacon beam: beacon_beam texture is not RGBA\n";
        return false;
    }
    g_srcPixels = std::move(pixels);
    g_srcW = w;
    g_srcH = h;
    g_srcState = 1;
    return true;
}

void EnsureBakeDirsLocked() {
    if (g_dirsReady) return;
    if (g_exeDir.empty()) g_exeDir = ExeDir();
    EnsureDir(JoinPath(g_exeDir, "textures"));
    EnsureDir(JoinPath(g_exeDir, "textures", "minecraft"));
    EnsureDir(JoinPath({g_exeDir, "textures", "minecraft", "entity"}));
    EnsureDir(JoinPath({g_exeDir, "textures", "minecraft", "entity", "beacon_beam_baked"}));
    g_dirsReady = true;
}

void MultiplyBake(int color, bool glow, std::vector<unsigned char>& out) {
    const int cr = (color >> 16) & 255;
    const int cg = (color >> 8) & 255;
    const int cb = color & 255;
    out.resize(g_srcPixels.size());
    for (size_t i = 0; i < g_srcPixels.size(); i += 4) {
        out[i] = static_cast<unsigned char>((static_cast<int>(g_srcPixels[i]) * cr) / 255);
        out[i + 1] = static_cast<unsigned char>((static_cast<int>(g_srcPixels[i + 1]) * cg) / 255);
        out[i + 2] = static_cast<unsigned char>((static_cast<int>(g_srcPixels[i + 2]) * cb) / 255);
        int alpha = g_srcPixels[i + 3];
        if (glow) alpha = (alpha * 32) / 255;
        out[i + 3] = static_cast<unsigned char>(alpha);
    }
}

bool WriteBake(const std::string& absPath, const std::vector<unsigned char>& pixels) {
    return stbi_write_png(absPath.c_str(), g_srcW, g_srcH, 4, pixels.data(), g_srcW * 4) != 0;
}

// One baked texture per (section color, solid/glow). Solid multiplies RGB
// and keeps texture alpha. Glow also multiplies alpha by 32/255.
BakePaths EnsureBaked(int color) {
    const int key = color & 0xFFFFFF;
    std::lock_guard<std::mutex> lock(g_bakeMutex);
    auto found = g_bakeCache.find(key);
    if (found != g_bakeCache.end()) return found->second;

    BakePaths paths;
    if (!EnsureSourceLocked()) {
        g_bakeCache.emplace(key, paths);
        return paths;
    }
    EnsureBakeDirsLocked();

    const std::string hex = Hex6(key);
    const std::string solidPart = "entity/beacon_beam_baked/" + hex + "_solid";
    const std::string glowPart = "entity/beacon_beam_baked/" + hex + "_glow";
    paths.solidName = "minecraft:" + solidPart;
    paths.glowName = "minecraft:" + glowPart;
    paths.solidPath = "textures/minecraft/" + solidPart + ".png";
    paths.glowPath = "textures/minecraft/" + glowPart + ".png";

    std::vector<unsigned char> solid;
    std::vector<unsigned char> glow;
    MultiplyBake(key, false, solid);
    MultiplyBake(key, true, glow);

    const std::string solidAbs = JoinPath(
        {g_exeDir, "textures", "minecraft", "entity", "beacon_beam_baked", hex + "_solid.png"});
    const std::string glowAbs = JoinPath(
        {g_exeDir, "textures", "minecraft", "entity", "beacon_beam_baked", hex + "_glow.png"});
    if (!WriteBake(solidAbs, solid) || !WriteBake(glowAbs, glow)) {
        std::cerr << "beacon beam: failed to write baked texture " << hex << "\n";
        g_bakeCache.emplace(key, paths);
        return paths;
    }
    RegisterTexture("minecraft", solidPart, paths.solidPath);
    RegisterTexture("minecraft", glowPart, paths.glowPath);
    paths.ok = true;
    g_bakeCache.emplace(key, paths);
    return paths;
}

int AddVertex(ModelData& model, float x, float y, float z) {
    const int index = static_cast<int>(model.vertices.size() / 3);
    model.vertices.push_back(x);
    model.vertices.push_back(y);
    model.vertices.push_back(z);
    return index;
}

int AddUv(ModelData& model, float u, float v) {
    const int index = static_cast<int>(model.uvCoordinates.size() / 2);
    model.uvCoordinates.push_back(u);
    model.uvCoordinates.push_back(v);
    return index;
}

int EnsureMaterial(ModelData& model, const std::string& name, const std::string& texturePath) {
    for (size_t i = 0; i < model.materials.size(); ++i) {
        if (model.materials[i].name == name) return static_cast<int>(i);
    }
    Material material;
    material.name = name;
    material.texturePath = texturePath;
    material.tintIndex = -1;
    material.type = NORMAL;
    material.aspectRatio = 1.0f;
    model.materials.push_back(material);
    return static_cast<int>(model.materials.size() - 1);
}

// Outward winding only. beaconBeam render state keeps the default back-face
// cull, so the opposite winding is not emitted. Vertex order matches vanilla:
// top of the first corner, bottom of the first, bottom of the second, top of
// the second. u is 1 on the first corner and 0 on the second.
void AddQuad(ModelData& model, int material, XZ a, XZ b, float y0, float y1, float v0, float v1) {
    Face face;
    face.vertexIndices = {
        AddVertex(model, a.x, y1, a.z),
        AddVertex(model, a.x, y0, a.z),
        AddVertex(model, b.x, y0, b.z),
        AddVertex(model, b.x, y1, b.z)
    };
    face.uvIndices = {
        AddUv(model, 1.0f, v1),
        AddUv(model, 1.0f, v0),
        AddUv(model, 0.0f, v0),
        AddUv(model, 0.0f, v1)
    };
    face.materialIndex = material;
    face.faceDirection = FaceType::DO_NOT_CULL;
    face.tintIndex = -1;
    model.faces.push_back(face);
}

void AddFourQuads(ModelData& model, int material, XZ wn, XZ en, XZ ws, XZ es,
                  float y0, float y1, float v0, float v1) {
    AddQuad(model, material, wn, en, y0, y1, v0, v1);
    AddQuad(model, material, es, ws, y0, y1, v0, v1);
    AddQuad(model, material, en, es, y0, y1, v0, v1);
    AddQuad(model, material, ws, wn, y0, y1, v0, v1);
}

XZ RotateY(float x, float z, float c, float s) {
    // JOML / Axis.YP: x' = c*x + s*z, z' = -s*x + c*z. Then the pose
    // translation (0.5, 0, 0.5) puts the beam on the block center.
    return XZ{c * x + s * z + 0.5f, -s * x + c * z + 0.5f};
}

float VAt(int localY, int sectionStart, float factor) {
    return -1.0f + static_cast<float>(localY - sectionStart) * factor;
}

} // namespace

void AppendBeaconBeam(ModelData& blockModel, const NbtTagPtr& blockEntityNbt, int x, int y, int z) {
    // The saved "Levels" is not trusted: the server does not load it back
    // and only refreshes it every 80 ticks, so a world saved soon after a
    // restart stores Levels:0 under a complete pyramid. The client computes
    // the base itself, so do the same; NBT is kept for the log only.
    bool nbtPresent = false;
    int nbtLevels = 0;
    if (blockEntityNbt) {
        const NbtTagPtr levelsTag = getChildByName(blockEntityNbt, "Levels");
        if (levelsTag && levelsTag->type == TagType::INT && levelsTag->payload.size() >= 4) {
            nbtLevels = bytesToInt(levelsTag->payload);
            nbtPresent = true;
        }
    }
    const int levels = ComputeBaseLevels(x, y, z);

    std::vector<Section> sections;
    if (levels > 0) ScanSections(sections, x, y, z);

    // Visible beam starts at the top of the beacon (local y = 1) and is
    // clipped to the export region top, config.maxY + 1.
    const int localTop = (config.maxY + 1) - y;
    const float angle = -45.0f * kDegToRad;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float radius = kBeamRadius;
    const XZ wn = RotateY(0.0f, radius, c, s);
    const XZ en = RotateY(radius, 0.0f, c, s);
    const XZ ws = RotateY(-radius, 0.0f, c, s);
    const XZ es = RotateY(0.0f, -radius, c, s);
    const XZ gwn{0.5f - kGlowRadius, 0.5f - kGlowRadius};
    const XZ gen{0.5f + kGlowRadius, 0.5f - kGlowRadius};
    const XZ gws{0.5f - kGlowRadius, 0.5f + kGlowRadius};
    const XZ ges{0.5f + kGlowRadius, 0.5f + kGlowRadius};

    const float solidFactor = kTextureScale * (0.5f / kBeamRadius);
    const float glowFactor = kTextureScale;

    struct Emitted {
        int y0;
        int y1;
        int color;
        int faces;
    };
    std::vector<Emitted> emitted;
    int faceCount = 0;

    if (levels > 0 && localTop > 1) {
        for (size_t i = 0; i < sections.size(); ++i) {
            const Section& section = sections[i];
            const int secStart = section.start;
            const int secEnd = section.start + section.height;
            const int renderEnd = (i + 1 == sections.size()) ? secStart + kMaxRenderY : secEnd;
            const int y0 = std::max(secStart, 1);
            const int y1 = std::min(renderEnd, localTop);
            if (y1 <= y0) continue;

            const BakePaths baked = EnsureBaked(section.color);
            if (!baked.ok) continue;

            const int solidMat = EnsureMaterial(blockModel, baked.solidName, baked.solidPath);
            const int glowMat = EnsureMaterial(blockModel, baked.glowName, baked.glowPath);
            const float yBottom = static_cast<float>(y0);
            const float yTop = static_cast<float>(y1);
            const float solidV0 = VAt(y0, secStart, solidFactor);
            const float solidV1 = VAt(y1, secStart, solidFactor);
            const float glowV0 = VAt(y0, secStart, glowFactor);
            const float glowV1 = VAt(y1, secStart, glowFactor);
            AddFourQuads(blockModel, solidMat, wn, en, ws, es, yBottom, yTop, solidV0, solidV1);
            AddFourQuads(blockModel, glowMat, gwn, gen, gws, ges, yBottom, yTop, glowV0, glowV1);
            emitted.push_back(Emitted{y + y0, y + y1, section.color & 0xFFFFFF, 8});
            faceCount += 8;
        }
    }

    std::ostringstream line;
    line << "beacon beam pos=(" << x << "," << y << "," << z << ")"
         << " nbtLevels=" << (nbtPresent ? std::to_string(nbtLevels) : std::string("absent"))
         << " levels=" << levels
         << " faces=" << faceCount
         << " sections=";
    if (emitted.empty()) {
        line << "none";
    } else {
        for (size_t i = 0; i < emitted.size(); ++i) {
            if (i) line << " ";
            line << "[" << emitted[i].y0 << "," << emitted[i].y1 << "):#"
                 << Hex6(emitted[i].color) << " faces=" << emitted[i].faces;
        }
    }
    std::lock_guard<std::mutex> logLock(g_logMutex);
    std::cerr << line.str() << "\n";
}
