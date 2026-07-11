// ==================== OptiFine CTM 连接材质实现 ====================
#include "CTM.h"
#include "block.h"          // GetBlockId / GetBlockById / Block
#include "model.h"          // ModelData / Face / Material / FaceType
#include "texture.h"        // MaterialType
#include "fileutils.h"      // string_to_wstring / wstring_to_string
#include "include/stb_image.h"
#include "include/stb_image_write.h"

#include <Windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <atomic>
#include <unordered_set>
#include <array>

// ========= 规则索引 =========
static std::vector<CtmRule> g_ctmRules;
// 按 matchBlocks 索引: "ns:blockName" -> 规则下标列表
static std::unordered_map<std::string, std::vector<size_t>> g_rulesByBlock;
// 按 matchTiles 索引: "ns:textureName" -> 规则下标列表
static std::unordered_map<std::string, std::vector<size_t>> g_rulesByTile;
// 通配 matchBlocks(以 '_' 开头)单独存放,避免对普通方块做全量扫描
// 元素: {ns, pattern, ruleIndex}
static std::vector<std::tuple<std::string, std::string, size_t>> g_wildcardBlockRules;
static std::atomic<bool> g_ctmInitialized{ false };
static std::mutex g_ctmPngMutex;        // 保护 PNG 合成/保存
static std::mutex g_ctmRuleMutex;       // 保护规则索引读取(初始化后只读,无需加锁)

// 已合成/保存过的 CTM 贴图模板缓存,避免重复 IO
// key = "ns|baseDir|identifier" -> {materialName, texturePath, saved}
struct CtmTexInfo {
    std::string materialName;   // 例如 minecraft:ctm/optifine/ctm/glass/glass/m123
    std::string texturePath;    // 相对路径,例如 textures/minecraft/ctm/.../m123.png
    bool saved = false;
};
static std::unordered_map<std::string, CtmTexInfo> g_ctmTexCache;
static std::mutex g_ctmTexCacheMutex;

static bool g_hasCtmRules = false;

// ========= 辅助:获取 exe 目录 =========
static std::string GetExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring ws(buffer);
    return wstring_to_string(ws);
}

static std::string ExeDir() {
    static std::string dir;
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::string exePath = GetExeDir();
        size_t pos = exePath.find_last_of("\\/");
        dir = (pos != std::string::npos) ? exePath.substr(0, pos) : exePath;
        });
    return dir;
}

// ========= properties 解析 =========
static std::unordered_map<std::string, std::string> ParseProperties(const std::string& content) {
    std::unordered_map<std::string, std::string> kv;
    std::istringstream ss(content);
    std::string line;
    while (std::getline(ss, line)) {
        // 去除 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // 去除首尾空白
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        if (line[start] == '#') continue; // 注释
        size_t end = line.find_last_not_of(" \t");
        std::string trimmed = line.substr(start, end - start + 1);
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimmed.substr(0, eq);
        std::string val = trimmed.substr(eq + 1);
        // trim
        auto trim = [](std::string& s) {
            size_t a = s.find_first_not_of(" \t");
            size_t b = s.find_last_not_of(" \t");
            if (a == std::string::npos) { s.clear(); return; }
            s = s.substr(a, b - a + 1);
            };
        trim(key); trim(val);
        kv[key] = val;
    }
    return kv;
}

static std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // trim
        size_t a = item.find_first_not_of(" \t");
        size_t b = item.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        out.push_back(item.substr(a, b - a + 1));
    }
    return out;
}

// 解析 tiles 字段,支持 "0-4" / "0 1 2" / "10-14" 混合
static std::vector<int> ParseTiles(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string token;
    while (ss >> token) {
        // 兼容逗号
        std::replace(token.begin(), token.end(), ',', ' ');
        std::stringstream ts(token);
        std::string part;
        while (ts >> part) {
            size_t dash = part.find('-');
            if (dash != std::string::npos && dash > 0) {
                try {
                    int a = std::stoi(part.substr(0, dash));
                    int b = std::stoi(part.substr(dash + 1));
                    if (a > b) std::swap(a, b);
                    for (int i = a; i <= b; ++i) out.push_back(i);
                }
                catch (...) {}
            }
            else {
                try { out.push_back(std::stoi(part)); }
                catch (...) {}
            }
        }
    }
    return out;
}

static CtmMethod ParseMethod(const std::string& m) {
    if (m == "ctm_compact") return CtmMethod::CtmCompact;
    if (m == "ctm") return CtmMethod::Ctm;
    if (m == "horizontal") return CtmMethod::Horizontal;
    if (m == "vertical") return CtmMethod::Vertical;
    if (m == "random") return CtmMethod::Random;
    if (m == "overlay_horizontal") return CtmMethod::OverlayHorizontal;
    if (m == "repeat") return CtmMethod::Repeat;
    return CtmMethod::Unknown;
}

// 从 properties 文件相对路径推导 tile 所在目录(相对 namespace)
// 例如 propsPath="optifine/ctm/glass/glass/glass.properties" -> "optifine/ctm/glass/glass"
static std::string BaseDirFromPropsPath(const std::string& propsPath) {
    size_t slash = propsPath.find_last_of('/');
    if (slash == std::string::npos) return "";
    return propsPath.substr(0, slash);
}

// 规范化 matchTiles 值:去掉可能的路径前缀和 .png
static std::string NormalizeTextureName(const std::string& v) {
    std::string s = v;
    // 去掉 .png
    if (s.size() > 4 && s.substr(s.size() - 4) == ".png") s = s.substr(0, s.size() - 4);
    // 如果以 optifine/ 开头,取最后一段作为 texture 名
    // matchTiles 可能是 "block/glass" 或 "glass" 或 "optifine/ctm/.../1.png"
    return s;
}

// ========= InitializeCtmRules =========
void InitializeCtmRules() {
    if (g_ctmInitialized.exchange(true)) return;

    std::lock_guard<std::shared_mutex> lock(GlobalCache::cacheMutex);

    // 遍历 ctmPropertiesIndex,它的 key = "ctmproperties:ns:propsPath",value = cacheKey
    // 这样能正确提取 ns 和 propsPath(避免被 modId 前缀干扰)
    for (const auto& entry : GlobalCache::ctmPropertiesIndex) {
        const std::string& indexKey = entry.first;   // "ctmproperties:ns:propsPath"
        const std::string& cacheKey = entry.second;  // "modId:ns:propsPath"

        // 去掉 "ctmproperties:" 前缀(长 15)
        const std::string prefix = "ctmproperties:";
        if (indexKey.size() <= prefix.size() || indexKey.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = indexKey.substr(prefix.size());  // "ns:propsPath"

        size_t colon = rest.find(':');
        if (colon == std::string::npos) continue;
        std::string ns = rest.substr(0, colon);
        std::string propsPath = rest.substr(colon + 1);

        // 取 properties 文本
        auto pit = GlobalCache::ctmProperties.find(cacheKey);
        if (pit == GlobalCache::ctmProperties.end()) continue;

        auto kv = ParseProperties(pit->second);
        CtmRule rule;
        rule.ns = ns;
        rule.propertiesPath = propsPath;
        rule.baseDir = BaseDirFromPropsPath(propsPath);

        if (kv.count("matchBlocks")) rule.matchBlocks = SplitComma(kv["matchBlocks"]);
        if (kv.count("matchTiles")) {
            for (auto& t : SplitComma(kv["matchTiles"]))
                rule.matchTiles.push_back(NormalizeTextureName(t));
        }
        if (kv.count("method")) rule.method = ParseMethod(kv["method"]);
        else rule.method = CtmMethod::Unknown;
        if (kv.count("tiles")) rule.tiles = ParseTiles(kv["tiles"]);
        if (kv.count("faces")) rule.faces = SplitComma(kv["faces"]);
        if (kv.count("connect")) rule.connect = kv["connect"];

        if (rule.method == CtmMethod::Unknown || rule.tiles.empty()) {
            continue; // 无法处理的方法或缺 tile
        }

        size_t idx = g_ctmRules.size();
        g_ctmRules.push_back(std::move(rule));

        // 建立索引: 通配 matchBlocks(以 '_' 开头)单独存放,精确的进 hash 索引
        for (const auto& b : g_ctmRules[idx].matchBlocks) {
            if (!b.empty() && b[0] == '_') {
                g_wildcardBlockRules.emplace_back(ns, b, idx);
            }
            else {
                g_rulesByBlock[ns + ":" + b].push_back(idx);
            }
        }
        for (const auto& t : g_ctmRules[idx].matchTiles) {
            g_rulesByTile[ns + ":" + t].push_back(idx);
        }
    }

    g_hasCtmRules = !g_ctmRules.empty();
    if (g_hasCtmRules) {
        std::cout << "[CTM] Loaded " << g_ctmRules.size() << " CTM rules "
            << "(byBlock=" << g_rulesByBlock.size()
            << ", byTile=" << g_rulesByTile.size() << ")" << std::endl;
    }
}

bool HasCtmRules() { return g_hasCtmRules; }

// matchBlocks 通配匹配:以 '_' 开头表示匹配任意前缀(如 _stained_glass 匹配 white_stained_glass)
static bool MatchBlockName(const std::string& pattern, const std::string& blockName) {
    if (pattern.empty()) return false;
    if (!pattern.empty() && pattern[0] == '_') {
        // 后缀匹配: blockName 以 pattern 结尾
        return blockName.size() >= pattern.size() &&
            blockName.compare(blockName.size() - pattern.size(), pattern.size(), pattern) == 0;
    }
    return pattern == blockName;
}

const CtmRule* FindCtmRule(const std::string& ns,
    const std::string& blockName,
    const std::string& textureName) {
    // 优先按 matchTiles 匹配(更具体),再按 matchBlocks
    {
        auto it = g_rulesByTile.find(ns + ":" + textureName);
        if (it != g_rulesByTile.end()) {
            for (size_t idx : it->second) return &g_ctmRules[idx];
        }
        // matchTiles 可能只写了短名(如 "glass"),textureName 可能是 "block/glass"
        // 尝试取 textureName 末尾段
        size_t slash = textureName.find_last_of('/');
        if (slash != std::string::npos) {
            std::string shortName = textureName.substr(slash + 1);
            auto it2 = g_rulesByTile.find(ns + ":" + shortName);
            if (it2 != g_rulesByTile.end()) {
                for (size_t idx : it2->second) return &g_ctmRules[idx];
            }
        }
    }
    {
        auto it = g_rulesByBlock.find(ns + ":" + blockName);
        if (it != g_rulesByBlock.end()) {
            for (size_t idx : it->second) return &g_ctmRules[idx];
        }
        // 通配匹配 _stained_glass 等(只在少量通配规则中后缀匹配)
        for (const auto& wc : g_wildcardBlockRules) {
            const std::string& wns = std::get<0>(wc);
            const std::string& pattern = std::get<1>(wc);
            size_t idx = std::get<2>(wc);
            if (wns != ns) continue;
            if (MatchBlockName(pattern, blockName)) return &g_ctmRules[idx];
        }
    }
    return nullptr;
}

bool CtmRuleMatchesFace(const CtmRule& rule, const std::string& faceName) {
    if (rule.faces.empty()) return true; // 未指定 = 全部面
    for (const auto& f : rule.faces) {
        if (f == "all") return true;
        if (f == "sides") {
            if (faceName == "north" || faceName == "south" ||
                faceName == "east" || faceName == "west") return true;
        }
        else if (f == faceName) return true;
    }
    return false;
}

// ========= 邻居连接判断 =========
// 判断 (x+dx, y+dy, z+dz) 处的方块是否与当前方块"连接"。
// connect=block: 同类方块(去掉状态后命名空间+方块名相同)
// 注意: 不能用 nb.air 判断,因为 glass/玻璃类方块不在 solids 表中会被标记为 air,
// 但它们仍是有效的连接目标。直接比较 baseName 即可(空气的 baseName 是 minecraft:air,
// 不会与普通方块名匹配)。
static bool IsConnected(int x, int y, int z, int dx, int dy, int dz,
    const std::string& curBaseName) {
    int id = GetBlockId(x + dx, y + dy, z + dz);
    Block nb = GetBlockById(id);
    std::string nbBase = nb.GetNameAndNameSpaceWithoutState();
    return nbBase == curBaseName;
}

// ========= 面方向工具 =========
static const char* FaceTypeName(FaceType ft) {
    switch (ft) {
    case FaceType::UP: return "up";
    case FaceType::DOWN: return "down";
    case FaceType::NORTH: return "north";
    case FaceType::SOUTH: return "south";
    case FaceType::WEST: return "west";
    case FaceType::EAST: return "east";
    default: return "";
    }
}

// 从面的 4 个顶点推断几何朝向(用于 DO_NOT_CULL 面)
static FaceType InferDirectionFromFace(const ModelData& model, const Face& face) {
    if (face.vertexIndices[0] < 0 || face.vertexIndices[1] < 0 ||
        face.vertexIndices[2] < 0 || face.vertexIndices[3] < 0) {
        return FaceType::DO_NOT_CULL;
    }
    // 取第一个顶点
    auto v0i = face.vertexIndices[0] * 3;
    auto v1i = face.vertexIndices[1] * 3;
    auto v2i = face.vertexIndices[2] * 3;
    if (v0i + 2 >= model.vertices.size() || v1i + 2 >= model.vertices.size() || v2i + 2 >= model.vertices.size()) {
        return FaceType::DO_NOT_CULL;
    }
    // 两条边叉积得到法线
    float ax = model.vertices[v1i] - model.vertices[v0i];
    float ay = model.vertices[v1i + 1] - model.vertices[v0i + 1];
    float az = model.vertices[v1i + 2] - model.vertices[v0i + 2];
    float bx = model.vertices[v2i] - model.vertices[v0i];
    float by = model.vertices[v2i + 1] - model.vertices[v0i + 1];
    float bz = model.vertices[v2i + 2] - model.vertices[v0i + 2];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    float axf = std::abs(nx), ayf = std::abs(ny), azf = std::abs(nz);
    if (axf >= ayf && axf >= azf) return nx > 0 ? FaceType::EAST : FaceType::WEST;
    if (ayf >= axf && ayf >= azf) return ny > 0 ? FaceType::UP : FaceType::DOWN;
    return nz > 0 ? FaceType::SOUTH : FaceType::NORTH;
}

// 一个面在其"贴图空间"的上/下/左/右 边方向 与 4 个角方向的世界偏移。
// 注意: 方向必须匹配 WorldImporter 的 OBJ UV 映射约定(而非 Continuity 的约定)。
// 经实测, WorldImporter 侧面贴图的 left/right 与 Continuity 相反,
// 这里采用匹配 WorldImporter UV 的方向(之前"大部分没问题"的版本)。
struct FaceLayout {
    std::array<int,3> up, down, left, right;
    std::array<int,3> upLeft, upRight, downLeft, downRight;
};

static FaceLayout GetFaceLayout(FaceType ft) {
    FaceLayout L{};
    switch (ft) {
    case FaceType::UP:
        L.up = { 0,0,-1 }; L.down = { 0,0,1 }; L.left = { -1,0,0 }; L.right = { 1,0,0 };
        break;
    case FaceType::DOWN:
        L.up = { 0,0,-1 }; L.down = { 0,0,1 }; L.left = { 1,0,0 }; L.right = { -1,0,0 };
        break;
    case FaceType::NORTH:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 1,0,0 }; L.right = { -1,0,0 };
        break;
    case FaceType::SOUTH:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { -1,0,0 }; L.right = { 1,0,0 };
        break;
    case FaceType::EAST:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 0,0,1 }; L.right = { 0,0,-1 };
        break;
    case FaceType::WEST:
        L.up = { 0,1,0 }; L.down = { 0,-1,0 }; L.left = { 0,0,-1 }; L.right = { 0,0,1 };
        break;
    default:
        break;
    }
    // 角方向 = 两条相邻边方向之和
    L.upLeft    = { L.up[0]+L.left[0],   L.up[1]+L.left[1],   L.up[2]+L.left[2] };
    L.upRight   = { L.up[0]+L.right[0],  L.up[1]+L.right[1],  L.up[2]+L.right[2] };
    L.downLeft  = { L.down[0]+L.left[0], L.down[1]+L.left[1], L.down[2]+L.left[2] };
    L.downRight = { L.down[0]+L.right[0],L.down[1]+L.right[1],L.down[2]+L.right[2] };
    return L;
}

// ========= PNG 读写 =========
// 从 GlobalCache::ctmTextures 读取指定 tile 的 RGBA 像素。
// tilePath 相对 namespace,例如 "optifine/ctm/glass/glass/0"
static bool LoadCtmTilePixels(const std::string& ns, const std::string& tileRelPath,
    std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        std::string indexKey = "ctmtextures:" + ns + ":" + tileRelPath;
        auto idxIt = GlobalCache::ctmTexturesIndex.find(indexKey);
        if (idxIt != GlobalCache::ctmTexturesIndex.end()) {
            auto it = GlobalCache::ctmTextures.find(idxIt->second);
            if (it != GlobalCache::ctmTextures.end()) pngData = it->second;
        }
        if (pngData.empty()) {
            // 回退线性扫描
            for (const auto& modId : GlobalCache::jarOrder) {
                std::string cacheKey = modId + ":" + ns + ":" + tileRelPath;
                auto it = GlobalCache::ctmTextures.find(cacheKey);
                if (it != GlobalCache::ctmTextures.end()) { pngData = it->second; break; }
            }
        }
    }
    if (pngData.empty()) return false;
    int w = 0, h = 0, ch = 0;
    unsigned char* px = stbi_load_from_memory(pngData.data(), (int)pngData.size(), &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) { if (px) stbi_image_free(px); return false; }
    outPixels.assign(px, px + (size_t)w * h * 4);
    outW = w; outH = h;
    stbi_image_free(px);
    return true;
}

// 确保目录存在(支持中文路径)
static void EnsureDir(const std::string& path) {
    std::wstring wpath = string_to_wstring(path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wpath.c_str(), NULL);
    }
}

static std::string BuildCtmTextureFilePath(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string exeDir = ExeDir();
    std::string root = exeDir + "\\textures\\" + ns + "\\ctm";
    // 逐级创建目录(Windows CreateDirectoryW 不会递归创建)
    EnsureDir(exeDir + "\\textures");
    EnsureDir(exeDir + "\\textures\\" + ns);
    EnsureDir(root);
    std::string dir = root;
    std::stringstream ss(baseDir);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        dir += "\\" + seg;
        EnsureDir(dir);
    }
    return dir + "\\" + fileName + ".png";
}

// 对应的相对路径(写入 MTL 用)
static std::string BuildCtmTextureRelPath(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string rel = "textures/" + ns + "/ctm";
    std::stringstream ss(baseDir);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        rel += "/" + seg;
    }
    rel += "/" + fileName + ".png";
    return rel;
}

static std::string BuildCtmMaterialName(const std::string& ns,
    const std::string& baseDir, const std::string& fileName) {
    std::string name = ns + ":ctm/" + baseDir + "/" + fileName;
    return name;
}

// 获取或创建一个 CTM 材质信息(模板)。仅保存"整张 tile"类型(horizontal/vertical/ctm/compact-fallback)。
// tileIndex: tile 编号
static CtmTexInfo GetOrCreateTileTexInfo(const std::string& ns, const std::string& baseDir, int tileIndex) {
    std::string id = ns + "|" + baseDir + "|t" + std::to_string(tileIndex);
    {
        std::lock_guard<std::mutex> lk(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }
    CtmTexInfo info;
    std::string fileName = "t" + std::to_string(tileIndex);
    info.materialName = BuildCtmMaterialName(ns, baseDir, fileName);
    info.texturePath = BuildCtmTextureRelPath(ns, baseDir, fileName);

    // 保存 PNG(只保存一次)
    std::lock_guard<std::mutex> lkPng(g_ctmPngMutex);
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second; // 双重检查
    }
    std::string fullPath = BuildCtmTextureFilePath(ns, baseDir, fileName);
    std::wstring wfull = string_to_wstring(fullPath);
    bool needSave = GetFileAttributesW(wfull.c_str()) == INVALID_FILE_ATTRIBUTES;
    if (needSave) {
        std::string tileRel = baseDir + "/" + std::to_string(tileIndex);
        std::vector<unsigned char> px; int w = 0, h = 0;
        if (LoadCtmTilePixels(ns, tileRel, px, w, h)) {
            stbi_write_png(fullPath.c_str(), w, h, 4, px.data(), w * 4);
        }
        else {
            // 找不到 tile,标记未保存,后续跳过
        }
    }
    info.saved = true;
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        g_ctmTexCache[id] = info;
    }
    return info;
}

// ========= ctm_compact 合成 =========
// 严格参照 Continuity(https://github.com/PepperCode1/Continuity) 的 CompactCtmQuadProcessor 实现。
// connections 位编码:
//   位 0(i=0): 左边连接
//   位 2(i=1): 下边连接
//   位 4(i=2): 右边连接
//   位 6(i=3): 上边连接
//   位 1(i=0): 左下角连接(仅当左边和下边都连时才检查)
//   位 3(i=1): 下右角连接
//   位 5(i=2): 右上角连接
//   位 7(i=3): 上左角连接
// 象限索引(顺时针): 0=TL 1=BL 2=BR 3=TR
// tile 语义(Continuity 源码注释):
//   0 - Unconnected(孤立)   1 - Fully connected(全连)
//   2 - Up and down/vertical   3 - Left and right/horizontal
//   4 - Unconnected corners(两边连但角不连)
static int CompactGetSpriteIndex(int quadrantIndex, int connections) {
    int index1 = quadrantIndex;
    int index2 = (quadrantIndex + 3) % 4;
    bool connected1 = ((connections >> (index1 * 2)) & 1) == 1;
    bool connected2 = ((connections >> (index2 * 2)) & 1) == 1;
    if (connected1 && connected2) {
        if (((connections >> (index2 * 2 + 1)) & 1) == 1) {
            return 1;  // 两边都连且角连 → 全连
        }
        return 4;      // 两边都连但角不连 → 角缺
    }
    if (connected1) {
        return 3 - quadrantIndex % 2;
    }
    if (connected2) {
        return 2 + quadrantIndex % 2;
    }
    return 0;          // 都不连 → 孤立
}

static CtmTexInfo GetOrCreateCompactTexInfo(const std::string& ns, const std::string& baseDir,
    const FaceLayout& L, int x, int y, int z, const std::string& curBaseName,
    const CtmRule& rule) {
    // 4 个方向偏移: directions[0]=左, [1]=下, [2]=右, [3]=上
    const std::array<int,3>* dirs[4] = { &L.left, &L.down, &L.right, &L.up };

    // 计算 4 条边的连接
    bool edgeConn[4];
    for (int i = 0; i < 4; ++i) {
        edgeConn[i] = IsConnected(x, y, z, (*dirs[i])[0], (*dirs[i])[1], (*dirs[i])[2], curBaseName);
    }

    // 计算 connections 位编码
    int connections = 0;
    for (int i = 0; i < 4; ++i) {
        if (edgeConn[i]) {
            connections |= 1 << (i * 2);
        }
    }
    // 角连接: 仅当两条相邻边都连时才检查(角位置=沿 index1 边走再沿 index2 边走)
    for (int i = 0; i < 4; ++i) {
        int index1 = i;
        int index2 = (i + 1) % 4;
        if (edgeConn[index1] && edgeConn[index2]) {
            // 角位置 = dirs[index1] + dirs[index2]
            int cdx = (*dirs[index1])[0] + (*dirs[index2])[0];
            int cdy = (*dirs[index1])[1] + (*dirs[index2])[1];
            int cdz = (*dirs[index1])[2] + (*dirs[index2])[2];
            if (IsConnected(x, y, z, cdx, cdy, cdz, curBaseName)) {
                connections |= 1 << (i * 2 + 1);
            }
        }
    }

    // 4 个象限选择 tile (象限索引: 0=TL 1=BL 2=BR 3=TR)
    int tileSel[4];
    for (int q = 0; q < 4; ++q) {
        int t = CompactGetSpriteIndex(q, connections);
        if (t >= (int)rule.tiles.size()) {
            t = 1;  // 退化到全连
        }
        tileSel[q] = rule.tiles[t];
    }

    // 构造唯一标识(基于 4 象限 tile 选择)
    std::string sig = std::to_string(tileSel[0]) + "_" +
        std::to_string(tileSel[1]) + "_" +
        std::to_string(tileSel[2]) + "_" +
        std::to_string(tileSel[3]);
    std::string id = ns + "|" + baseDir + "|c_" + sig;
    std::string fileName = "c_" + sig;

    {
        std::lock_guard<std::mutex> lk(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    CtmTexInfo info;
    info.materialName = BuildCtmMaterialName(ns, baseDir, fileName);
    info.texturePath = BuildCtmTextureRelPath(ns, baseDir, fileName);

    std::lock_guard<std::mutex> lkPng(g_ctmPngMutex);
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        auto it = g_ctmTexCache.find(id);
        if (it != g_ctmTexCache.end()) return it->second;
    }

    std::string fullPath = BuildCtmTextureFilePath(ns, baseDir, fileName);
    std::wstring wfull = string_to_wstring(fullPath);
    bool needSave = GetFileAttributesW(wfull.c_str()) == INVALID_FILE_ATTRIBUTES;


    if (needSave) {
        // 读取 4 张需要的 tile 像素
        // 缓存到局部 map 避免重复加载
        std::unordered_map<int, std::pair<std::vector<unsigned char>, std::pair<int, int>>> tilePx;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
            int tn = tileSel[i];
            if (tilePx.count(tn)) continue;
            std::string tileRel = baseDir + "/" + std::to_string(tn);
            std::vector<unsigned char> px; int w = 0, h = 0;
            if (!LoadCtmTilePixels(ns, tileRel, px, w, h) || w != h) {
                ok = false;
                break;
            }
            tilePx[tn] = { px, {w, h} };
        }
        if (ok) {
            // 取 tile 尺寸(假设所有 tile 同尺寸且为正方形)
            int tw = tilePx[tileSel[0]].second.first;
            int half = tw / 2;
            int outW = tw, outH = tw; // 合成图与 tile 同尺寸
            std::vector<unsigned char> out((size_t)outW * outH * 4, 0);

            // 4 象限在合成图和 tile 中的位置(象限索引: 0=TL 1=BL 2=BR 3=TR)
            // [0]=TL: x[0,half),   y[0,half)
            // [1]=BL: x[0,half),   y[half,tw)
            // [2]=BR: x[half,tw),  y[half,tw)
            // [3]=TR: x[half,tw),  y[0,half)
            int qx0[4] = { 0, 0, half, half };
            int qy0[4] = { 0, half, half, 0 };

            for (int i = 0; i < 4; ++i) {
                auto& tp = tilePx[tileSel[i]];
                const std::vector<unsigned char>& px = tp.first;
                int w = tp.second.first;
                for (int yy = 0; yy < half; ++yy) {
                    for (int xx = 0; xx < half; ++xx) {
                        int srcIdx = ((qy0[i] + yy) * w + (qx0[i] + xx)) * 4;
                        int dstIdx = ((qy0[i] + yy) * outW + (qx0[i] + xx)) * 4;
                        out[dstIdx + 0] = px[srcIdx + 0];
                        out[dstIdx + 1] = px[srcIdx + 1];
                        out[dstIdx + 2] = px[srcIdx + 2];
                        out[dstIdx + 3] = px[srcIdx + 3];
                    }
                }
            }
            stbi_write_png(fullPath.c_str(), outW, outH, 4, out.data(), outW * 4);
        }
    }

    info.saved = true;
    {
        std::lock_guard<std::mutex> lk2(g_ctmTexCacheMutex);
        g_ctmTexCache[id] = info;
    }
    return info;
}

// ========= horizontal / vertical =========
// horizontal: 按左右连接选 4 tile(0=无连接,1=左,2=右,3=左右都有)
static int SelectHorizontalTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    bool left = IsConnected(x, y, z, L.left[0], L.left[1], L.left[2], curBaseName);
    bool right = IsConnected(x, y, z, L.right[0], L.right[1], L.right[2], curBaseName);
    if (left && right) return 3;
    if (left) return 1;
    if (right) return 2;
    return 0;
}

// vertical: 按上下连接选 4 tile
static int SelectVerticalTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    bool up = IsConnected(x, y, z, L.up[0], L.up[1], L.up[2], curBaseName);
    bool down = IsConnected(x, y, z, L.down[0], L.down[1], L.down[2], curBaseName);
    if (up && down) return 3;
    if (up) return 1;
    if (down) return 2;
    return 0;
}

// ========= 完整 ctm(47 tile) =========
// 标准 OptiFine CTM tile 索引表: 由 8 位连接掩码(N E S W NE SE SW NW)映射到 0-46
// 连接位定义: bit0=N(上) bit1=E(右) bit2=S(下) bit3=W(左) bit4=NE bit5=SE bit6=SW bit7=NW
static const int kCtmTileMap[256] = {
    0,18,18,18,18,18,18,18,18,18,18,18,18,18,18,18,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
   18, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
};
// 注: 上面是简化表。OptiFine 完整 47 tile 表较复杂,这里用一个保守的简化版,
// 仅区分"无连接(0)"和"有连接(1/18)"。第一阶段保证 method=ctm 不崩溃且产生基本连接感。

static int SelectCtmTile(const FaceLayout& L, int x, int y, int z, const std::string& curBaseName) {
    bool n = IsConnected(x, y, z, L.up[0], L.up[1], L.up[2], curBaseName);
    bool e = IsConnected(x, y, z, L.right[0], L.right[1], L.right[2], curBaseName);
    bool s = IsConnected(x, y, z, L.down[0], L.down[1], L.down[2], curBaseName);
    bool w = IsConnected(x, y, z, L.left[0], L.left[1], L.left[2], curBaseName);
    bool ne = IsConnected(x, y, z, L.upRight[0], L.upRight[1], L.upRight[2], curBaseName);
    bool se = IsConnected(x, y, z, L.downRight[0], L.downRight[1], L.downRight[2], curBaseName);
    bool sw = IsConnected(x, y, z, L.downLeft[0], L.downLeft[1], L.downLeft[2], curBaseName);
    bool nw = IsConnected(x, y, z, L.upLeft[0], L.upLeft[1], L.upLeft[2], curBaseName);
    int mask = (n ? 1 : 0) | (e ? 2 : 0) | (s ? 4 : 0) | (w ? 8 : 0) |
        (ne ? 16 : 0) | (se ? 32 : 0) | (sw ? 64 : 0) | (nw ? 128 : 0);
    return kCtmTileMap[mask & 255];
}

// ========= ApplyCtmToBlockModel =========
void ApplyCtmToBlockModel(ModelData& model,
    const std::string& ns,
    const std::string& blockName,
    int x, int y, int z) {
    if (!g_hasCtmRules) return;
    if (model.faces.empty() || model.materials.empty()) return;

    // blockName 可能带状态(如 "glass[axis=y]"),去掉状态后才能与邻居的
    // GetNameAndNameSpaceWithoutState() 比较。
    std::string baseBlockName = blockName;
    size_t bracketPos = baseBlockName.find('[');
    if (bracketPos != std::string::npos) {
        baseBlockName = baseBlockName.substr(0, bracketPos);
    }
    std::string curBaseName = ns + ":" + baseBlockName;

    // model 内的 CTM 材质名 -> materialIndex
    std::unordered_map<std::string, int> localCtmMatIndex;
    auto getOrAddMaterial = [&](const CtmTexInfo& info) -> int {
        auto it = localCtmMatIndex.find(info.materialName);
        if (it != localCtmMatIndex.end()) return it->second;
        Material m;
        m.name = info.materialName;
        m.texturePath = info.texturePath;
        m.tintIndex = -1;
        m.type = NORMAL;
        m.aspectRatio = 1.0f;
        int idx = (int)model.materials.size();
        model.materials.push_back(m);
        localCtmMatIndex[info.materialName] = idx;
        return idx;
        };

    for (auto& face : model.faces) {
        if (face.materialIndex < 0 || face.materialIndex >= (int)model.materials.size()) continue;
        const Material& mat = model.materials[face.materialIndex];

        // 从材质名/路径提取贴图名
        // mat.name 形如 "minecraft:block/glass", mat.texturePath 形如 "textures/minecraft/block/glass.png"
        std::string textureName;
        std::string matNs = ns;
        {
            // mat.name = "ns:path"
            const std::string& mn = mat.name;
            size_t colon = mn.find(':');
            std::string pathPart;
            if (colon != std::string::npos) { matNs = mn.substr(0, colon); pathPart = mn.substr(colon + 1); }
            else pathPart = mn;
            // 去掉 block/ 前缀? 保留完整 path 作为 textureName
            textureName = pathPart;
        }

        const CtmRule* rule = FindCtmRule(matNs, baseBlockName, textureName);
        if (!rule) continue;

        // 确定面朝向
        FaceType ft = face.faceDirection;
        if (ft == FaceType::DO_NOT_CULL || ft == FaceType::UNKNOWN) {
            ft = InferDirectionFromFace(model, face);
            if (ft == FaceType::DO_NOT_CULL) continue; // 无法判断方向,跳过
        }
        const char* ftn = FaceTypeName(ft);
        if (!CtmRuleMatchesFace(*rule, ftn)) continue;

        FaceLayout L = GetFaceLayout(ft);

        CtmTexInfo info;
        bool got = false;
        switch (rule->method) {
        case CtmMethod::CtmCompact:
            info = GetOrCreateCompactTexInfo(rule->ns, rule->baseDir, L, x, y, z, curBaseName, *rule);
            got = true;
            break;
        case CtmMethod::Horizontal: {
            int sel = SelectHorizontalTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = true;
            }
            break;
        }
        case CtmMethod::Vertical: {
            int sel = SelectVerticalTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = true;
            }
            break;
        }
        case CtmMethod::Ctm: {
            int sel = SelectCtmTile(L, x, y, z, curBaseName);
            if (sel < (int)rule->tiles.size()) {
                info = GetOrCreateTileTexInfo(rule->ns, rule->baseDir, rule->tiles[sel]);
                got = true;
            }
            break;
        }
        default:
            // Random/Overlay/Repeat 第一阶段不处理
            break;
        }

        if (got) {
            face.materialIndex = getOrAddMaterial(info);
        }
    }
}
