// CreateCT.cpp
// ==================== Create 机械动力 连接纹理实现 ====================
// 移植自 Create 的 CTSpriteShiftEntry + AllCTTypes + ConnectedTextureBehaviour:
//   - 方块模型引用原始贴图(如 block/brass_casing)的面, 运行时把 UV 重映射到
//     * 专用连接贴图(如 block/brass_casing_connected.png)的 sheet 格子上。
//   - 连接判定: 相邻方块 baseName 相同即连接 (同 Create 的 connectsTo 简化版)。
//   - sheet 格子编号由 getTextureIndex 根据 8 方向连接状态计算。
#include "CreateCT.h"
#include "block.h"          // GetBlockId / GetBlockById / Block
#include "GlobalCache.h"    // 贴图缓存
#include "fileutils.h"      // string_to_wstring
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

// ========= 注册表 (ns, 贴图路径, CTType) =========
// 数据来源: Create AllSpriteShifts (mc1.20.1)
// 注意: 原贴图路径不含 textures/ 前缀, connected 贴图 = 原贴图 + "_connected"
static const CreateCTEntry kCreateCTRegistry[] = {
    // ---------- 机壳 (OMNIDIRECTIONAL 8x8) ----------
    { "create", "block/andesite_casing",          CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/brass_casing",             CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/copper_casing",            CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/shadow_steel_casing",      CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/refined_radiance_casing",  CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/railway_casing",           CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/railway_casing_side",      CreateCTType::OMNIDIRECTIONAL, 8 },

    // ---------- 玻璃 (OMNIDIRECTIONAL / 变体) ----------
    { "create", "block/palettes/framed_glass",            CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/palettes/horizontal_framed_glass", CreateCTType::HORIZONTAL_KRYPPERS, 2 },
    { "create", "block/palettes/vertical_framed_glass",   CreateCTType::VERTICAL, 2 },

    // ---------- 窗户 (VERTICAL / RECTANGLE) ----------
    { "create", "block/palettes/oak_window",          CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/spruce_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/birch_window",        CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/jungle_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/acacia_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/dark_oak_window",     CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/mangrove_window",     CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/cherry_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/crimson_window",      CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/warped_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/bamboo_window",       CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/ornate_iron_window",   CreateCTType::VERTICAL, 2 },
    { "create", "block/palettes/industrial_iron_window", CreateCTType::RECTANGLE, 4 },
    { "create", "block/palettes/weathered_iron_window",   CreateCTType::RECTANGLE, 4 },
    { "create", "block/palettes/weathered_iron_window_1", CreateCTType::RECTANGLE, 4 },
    { "create", "block/palettes/weathered_iron_window_2", CreateCTType::RECTANGLE, 4 },
    { "create", "block/palettes/weathered_iron_window_3", CreateCTType::RECTANGLE, 4 },
    { "create", "block/palettes/weathered_iron_window_4", CreateCTType::RECTANGLE, 4 },

    // ---------- 脚手架 (HORIZONTAL 2x2) ----------
    { "create", "block/scaffold/andesite_scaffold",        CreateCTType::HORIZONTAL, 2 },
    { "create", "block/scaffold/brass_scaffold",           CreateCTType::HORIZONTAL, 2 },
    { "create", "block/scaffold/copper_scaffold",          CreateCTType::HORIZONTAL, 2 },
    { "create", "block/scaffold/andesite_scaffold_inside", CreateCTType::HORIZONTAL, 2 },
    { "create", "block/scaffold/brass_scaffold_inside",    CreateCTType::HORIZONTAL, 2 },
    { "create", "block/scaffold/copper_scaffold_inside",   CreateCTType::HORIZONTAL, 2 },

    // ---------- 齿轮箱外壳 (VERTICAL) ----------
    { "create", "block/andesite_encased_cogwheel_side", CreateCTType::VERTICAL, 2 },
    { "create", "block/brass_encased_cogwheel_side",    CreateCTType::VERTICAL, 2 },

    // ---------- 流体罐 (RECTANGLE 4x4) ----------
    { "create", "block/fluid_tank",          CreateCTType::RECTANGLE, 4 },
    { "create", "block/fluid_tank_top",      CreateCTType::RECTANGLE, 4 },
    { "create", "block/fluid_tank_inner",    CreateCTType::RECTANGLE, 4 },
    { "create", "block/creative_fluid_tank", CreateCTType::RECTANGLE, 4 },
    { "create", "block/creative_casing",     CreateCTType::RECTANGLE, 4 },

    // ---------- 底盘 (OMNIDIRECTIONAL) ----------
    { "create", "block/linear_chassis_side",            CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/secondary_linear_chassis_side",  CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/linear_chassis_end",             CreateCTType::OMNIDIRECTIONAL, 8 },
    { "create", "block/linear_chassis_end_sticky",      CreateCTType::OMNIDIRECTIONAL, 8 },

    // ---------- 铜瓦 / 铜屋顶 (ROOF 4x4, 各氧化状态) ----------
    { "create", "block/copper/copper_shingles_top",            CreateCTType::ROOF, 4 },
    { "create", "block/copper/exposed_copper_shingles_top",    CreateCTType::ROOF, 4 },
    { "create", "block/copper/weathered_copper_shingles_top",  CreateCTType::ROOF, 4 },
    { "create", "block/copper/oxidized_copper_shingles_top",   CreateCTType::ROOF, 4 },
    { "create", "block/copper/copper_tiles_top",               CreateCTType::ROOF, 4 },
    { "create", "block/copper/exposed_copper_tiles_top",       CreateCTType::ROOF, 4 },
    { "create", "block/copper/weathered_copper_tiles_top",     CreateCTType::ROOF, 4 },
    { "create", "block/copper/oxidized_copper_tiles_top",      CreateCTType::ROOF, 4 },

    // ---------- 其他 ----------
    { "create", "block/girder_pole_side",    CreateCTType::VERTICAL, 2 },
    { "create", "block/tunnel/brass_tunnel_top", CreateCTType::VERTICAL, 2 },
    { "create", "block/crafter_side",        CreateCTType::VERTICAL, 2 },
};

static const size_t kCreateCTCount = sizeof(kCreateCTRegistry) / sizeof(kCreateCTRegistry[0]);

// ========= sheet 大小 =========
static int GetSheetSize(CreateCTType type) {
    switch (type) {
    case CreateCTType::HORIZONTAL:
    case CreateCTType::HORIZONTAL_KRYPPERS:
    case CreateCTType::VERTICAL:
        return 2;
    case CreateCTType::OMNIDIRECTIONAL:
        return 8;
    case CreateCTType::ROOF:
    case CreateCTType::ROOF_STAIR:
    case CreateCTType::CROSS:
    case CreateCTType::RECTANGLE:
        return 4;
    }
    return 2;
}

void InitializeCreateCT() {
    // 静态注册表, 无需初始化
}

const CreateCTEntry* FindCreateCTEntry(const std::string& ns, const std::string& texturePath) {
    for (size_t i = 0; i < kCreateCTCount; ++i) {
        if (kCreateCTRegistry[i].ns == ns && kCreateCTRegistry[i].texturePath == texturePath) {
            return &kCreateCTRegistry[i];
        }
    }
    return nullptr;
}

// ========= 连接状态 =========
struct CTContext {
    bool up = false, down = false, left = false, right = false;
    bool topLeft = false, topRight = false, bottomLeft = false, bottomRight = false;
};

// 邻居 baseName 是否与当前方块相同 (简化版 connectsTo, 不做遮挡检查)
static bool IsConnected(int x, int y, int z, int dx, int dy, int dz,
    const std::string& curBaseName) {
    int id = GetBlockId(x + dx, y + dy, z + dz);
    if (id < 0) return false;
    Block nb = GetBlockById(id);
    std::string nbBase = nb.GetNameAndNameSpaceWithoutState();
    return nbBase == curBaseName;
}

// ========= getTextureIndex 移植 (AllCTTypes.java) =========
static int GetTextureIndex(CreateCTType type, const CTContext& c) {
    switch (type) {
    case CreateCTType::HORIZONTAL:
        return (c.right ? 1 : 0) + (c.left ? 2 : 0);
    case CreateCTType::HORIZONTAL_KRYPPERS:
        return !c.right && !c.left ? 0 : !c.right ? 3 : !c.left ? 2 : 1;
    case CreateCTType::VERTICAL:
        return (c.up ? 1 : 0) + (c.down ? 2 : 0);
    case CreateCTType::OMNIDIRECTIONAL: {
        int tileX = 0, tileY = 0;
        int borders = (!c.up ? 1 : 0) + (!c.down ? 1 : 0) + (!c.left ? 1 : 0) + (!c.right ? 1 : 0);
        if (c.up) tileX++;
        if (c.down) tileX += 2;
        if (c.left) tileY++;
        if (c.right) tileY += 2;
        if (borders == 0) {
            if (c.topRight) tileX++;
            if (c.topLeft) tileX += 2;
            if (c.bottomRight) tileY += 2;
            if (c.bottomLeft) tileY++;
        }
        if (borders == 1) {
            if (!c.right) {
                if (c.topLeft || c.bottomLeft) { tileY = 4; tileX = -1 + (c.bottomLeft ? 1 : 0) + (c.topLeft ? 1 : 0) * 2; }
            }
            if (!c.left) {
                if (c.topRight || c.bottomRight) { tileY = 5; tileX = -1 + (c.bottomRight ? 1 : 0) + (c.topRight ? 1 : 0) * 2; }
            }
            if (!c.down) {
                if (c.topLeft || c.topRight) { tileY = 6; tileX = -1 + (c.topLeft ? 1 : 0) + (c.topRight ? 1 : 0) * 2; }
            }
            if (!c.up) {
                if (c.bottomLeft || c.bottomRight) { tileY = 7; tileX = -1 + (c.bottomLeft ? 1 : 0) + (c.bottomRight ? 1 : 0) * 2; }
            }
        }
        if (borders == 2) {
            if ((c.up && c.left && c.topLeft) || (c.down && c.left && c.bottomLeft)
                || (c.up && c.right && c.topRight) || (c.down && c.right && c.bottomRight))
                tileX += 3;
        }
        return tileX + 8 * tileY;
    }
    case CreateCTType::ROOF: {
        bool upDrops = c.down && !c.up && (c.left || c.right);
        bool downDrops = !c.down && c.up && (c.left || c.right);
        bool leftDrops = !c.left && c.right && (c.up || c.down);
        bool rightDrops = c.left && !c.right && (c.up || c.down);
        if (upDrops) {
            if (leftDrops) return c.bottomRight ? 0 : 5;
            if (rightDrops) return c.bottomLeft ? 2 : 5;
            return 1;
        }
        if (downDrops) {
            if (leftDrops) return c.topRight ? 8 : 5;
            if (rightDrops) return c.topLeft ? 10 : 5;
            return 9;
        }
        if (leftDrops) return 4;
        if (rightDrops) return 6;
        if (!c.up || !c.down || !c.left || !c.right) return 5;
        if (c.bottomLeft && c.topRight) {
            if (c.topLeft && !c.bottomRight) return 12;
            if (c.bottomRight && !c.topLeft) return 15;
            if (!c.bottomRight && !c.topLeft) return 7;
        }
        if (c.bottomRight && c.topLeft) {
            if (c.topRight && !c.bottomLeft) return 13;
            if (c.bottomLeft && !c.topRight) return 14;
            if (!c.bottomLeft && !c.topRight) return 11;
        }
        return 5;
    }
    case CreateCTType::ROOF_STAIR: {
        static const int MAPPING[4][4] = { { 1, 6, 9, 4 }, { 14, 12, 13, 15 }, { 2, 10, 8, 0 }, { 5, 5, 5, 5 } };
        int type = (c.up ? 2 : 0) + (c.right ? 1 : 0);
        int rot = (c.left ? 2 : 0) + (c.down ? 1 : 0);
        return MAPPING[type][rot];
    }
    case CreateCTType::CROSS:
        return (c.up ? 1 : 0) + (c.down ? 2 : 0) + (c.left ? 4 : 0) + (c.right ? 8 : 0);
    case CreateCTType::RECTANGLE: {
        int x = c.left && c.right ? 2 : c.left ? 3 : c.right ? 1 : 0;
        int y = c.up && c.down ? 1 : c.up ? 2 : c.down ? 0 : 3;
        return x + y * 4;
    }
    }
    return 0;
}

// ========= 面方向 → up/down/left/right 世界偏移 (Create buildContext) =========
struct Offset { int x, y, z; };
static Offset Opposite(Offset o) { return { -o.x, -o.y, -o.z }; }

struct FaceDirs {
    Offset up, down, left, right;
    Offset upLeft, upRight, downLeft, downRight;
};

static FaceDirs GetFaceDirs(FaceType ft) {
    FaceDirs d{};
    // getUpDirection: 水平面 → UP, 否则 NORTH
    bool horizontal = (ft == FaceType::NORTH || ft == FaceType::SOUTH ||
        ft == FaceType::WEST || ft == FaceType::EAST);
    Offset v = horizontal ? Offset{ 0, 1, 0 } : Offset{ 0, 0, -1 };          // UP / NORTH
    // getRightDirection: X轴面 → SOUTH, 否则 WEST
    Offset h = (ft == FaceType::WEST || ft == FaceType::EAST)
        ? Offset{ 0, 0, 1 } : Offset{ -1, 0, 0 };                             // SOUTH / WEST

    // positive: face 的轴方向
    bool positive = (ft == FaceType::UP || ft == FaceType::SOUTH || ft == FaceType::EAST);
    if (positive) h = Opposite(h);
    if (ft == FaceType::DOWN) { v = Opposite(v); h = Opposite(h); }

    d.up = v; d.down = Opposite(v);
    d.right = h; d.left = Opposite(h);
    d.upLeft    = { d.up.x + d.left.x,   d.up.y + d.left.y,   d.up.z + d.left.z };
    d.upRight   = { d.up.x + d.right.x,  d.up.y + d.right.y,  d.up.z + d.right.z };
    d.downLeft  = { d.down.x + d.left.x, d.down.y + d.left.y, d.down.z + d.left.z };
    d.downRight = { d.down.x + d.right.x,d.down.y + d.right.y,d.down.z + d.right.z };
    return d;
}

// 根据面方向构建连接上下文 (Create buildContext)
static CTContext BuildContext(const FaceDirs& d, int x, int y, int z,
    const std::string& curBaseName) {
    CTContext c;
    c.up    = IsConnected(x, y, z, d.up.x,    d.up.y,    d.up.z,    curBaseName);
    c.down  = IsConnected(x, y, z, d.down.x,  d.down.y,  d.down.z,  curBaseName);
    c.left  = IsConnected(x, y, z, d.left.x,  d.left.y,  d.left.z,  curBaseName);
    c.right = IsConnected(x, y, z, d.right.x, d.right.y, d.right.z, curBaseName);
    c.topLeft    = c.up && c.left    && IsConnected(x, y, z, d.upLeft.x,    d.upLeft.y,    d.upLeft.z,    curBaseName);
    c.topRight   = c.up && c.right   && IsConnected(x, y, z, d.upRight.x,   d.upRight.y,   d.upRight.z,   curBaseName);
    c.bottomLeft = c.down && c.left  && IsConnected(x, y, z, d.downLeft.x,  d.downLeft.y,  d.downLeft.z,  curBaseName);
    c.bottomRight= c.down && c.right && IsConnected(x, y, z, d.downRight.x, d.downRight.y, d.downRight.z, curBaseName);
    return c;
}

// ========= 保存 connected 贴图 =========
static std::mutex g_ctPngMutex;
static std::unordered_map<std::string, std::string> g_ctPngCache; // key -> texturePath
static std::unordered_set<std::string> g_ctPbrDone;               // 已处理过 PBR 变体的 key

static void EnsureDir(const std::string& path) {
    std::wstring wpath = string_to_wstring(path);
    if (GetFileAttributesW(wpath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wpath.c_str(), NULL);
    }
}

static std::string GetExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring ws(buffer);
    std::string s = wstring_to_string(ws);
    size_t pos = s.find_last_of("\\/");
    return (pos != std::string::npos) ? s.substr(0, pos) : s;
}

// 从 GlobalCache 读 connected PNG 并保存到 textures/ 目录。
// 返回材质 texturePath (相对路径), 失败返回空串。
static std::string GetOrSaveConnectedTexture(const std::string& ns,
    const std::string& texturePath) {
    std::string connected = texturePath + "_connected";
    {
        std::lock_guard<std::mutex> lk(g_ctPngMutex);
        auto it = g_ctPngCache.find(ns + ":" + connected);
        if (it != g_ctPngCache.end()) return it->second;
    }

    // 从缓存取 PNG 字节
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto idxIt = GlobalCache::textureIndex.find("textures:" + ns + ":" + connected);
        if (idxIt != GlobalCache::textureIndex.end()) {
            auto texIt = GlobalCache::textures.find(idxIt->second);
            if (texIt != GlobalCache::textures.end()) pngData = texIt->second;
        }
        if (pngData.empty()) {
            for (const auto& modId : GlobalCache::jarOrder) {
                std::string cacheKey = modId + ":" + ns + ":" + connected;
                auto it = GlobalCache::textures.find(cacheKey);
                if (it != GlobalCache::textures.end()) { pngData = it->second; break; }
            }
        }
    }
    if (pngData.empty()) return "";

    // 保存到 <exe>/textures/<ns>/<path>_connected.png
    std::string exeDir = GetExeDir();
    std::string root = exeDir + "\\textures\\" + ns;
    EnsureDir(exeDir + "\\textures");
    EnsureDir(root);
    std::string dir = root;
    std::stringstream ss(texturePath);
    std::string seg;
    while (std::getline(ss, seg, '/')) {
        if (seg.empty()) continue;
        dir += "\\" + seg;
        EnsureDir(dir);
    }
    std::string fullPath = dir + "_connected.png";
    {
        std::lock_guard<std::mutex> lk(g_ctPngMutex);
        // 双检, 避免重复写
        auto it = g_ctPngCache.find(ns + ":" + connected);
        if (it != g_ctPngCache.end()) return it->second;
        std::ofstream out(fullPath, std::ios::binary);
        if (out.is_open()) {
            out.write(reinterpret_cast<const char*>(pngData.data()), pngData.size());
            out.close();
        }
        std::string rel = "textures/" + ns + "/" + texturePath + "_connected.png";
        g_ctPngCache[ns + ":" + connected] = rel;

        // PBR 变体: 资源包提供 <path>_connected_n/_s/_a 就一并拷出, 没有就跳过(只做一次)
        if (g_ctPbrDone.insert(ns + ":" + connected).second) {
            const char* pbrSuffixes[3] = { "_n", "_s", "_a" };
            for (const char* suffix : pbrSuffixes) {
                std::vector<unsigned char> pbrData;
                {
                    std::shared_lock<std::shared_mutex> cacheLock(GlobalCache::cacheMutex);
                    std::string pbrName = connected + suffix;
                    auto pbrIdxIt = GlobalCache::textureIndex.find("textures:" + ns + ":" + pbrName);
                    if (pbrIdxIt != GlobalCache::textureIndex.end()) {
                        auto pbrTexIt = GlobalCache::textures.find(pbrIdxIt->second);
                        if (pbrTexIt != GlobalCache::textures.end()) pbrData = pbrTexIt->second;
                    }
                    if (pbrData.empty()) {
                        for (const auto& modId : GlobalCache::jarOrder) {
                            auto pbrTexIt = GlobalCache::textures.find(modId + ":" + ns + ":" + pbrName);
                            if (pbrTexIt != GlobalCache::textures.end()) { pbrData = pbrTexIt->second; break; }
                        }
                    }
                }
                if (pbrData.empty()) continue;
                std::ofstream pbrOut(dir + "_connected" + suffix + ".png", std::ios::binary);
                if (pbrOut.is_open()) {
                    pbrOut.write(reinterpret_cast<const char*>(pbrData.data()), pbrData.size());
                    pbrOut.close();
                }
            }
        }
        return rel;
    }
}

// ========= 主入口 =========
bool ApplyCreateCTToBlockModel(ModelData& model, const std::string& ns,
    const std::string& blockName, int x, int y, int z) {
    if (model.faces.empty() || model.materials.empty()) return false;
    if (ns != "create") return false;

    // 当前方块的 baseName (用于连接判定)
    std::string baseBlockName = blockName;
    size_t bracketPos = baseBlockName.find('[');
    if (bracketPos != std::string::npos) {
        baseBlockName = baseBlockName.substr(0, bracketPos);
    }
    std::string curBaseName = ns + ":" + baseBlockName;

    // 材质名 -> 新材质索引 (避免重复添加 connected 材质)
    std::unordered_map<std::string, int> ctMatIndex;
    bool applied = false;

    for (auto& face : model.faces) {
        if (face.materialIndex < 0 || face.materialIndex >= (int)model.materials.size()) continue;
        const Material& mat = model.materials[face.materialIndex];

        // 从材质名提取命名空间 + 贴图路径, 例如 "create:block/brass_casing"
        std::string matNs = ns;
        std::string texturePath;
        {
            const std::string& mn = mat.name;
            size_t colon = mn.find(':');
            if (colon != std::string::npos) { matNs = mn.substr(0, colon); texturePath = mn.substr(colon + 1); }
            else texturePath = mn;
            // 去掉可能的 "textures/" 前缀和 ".png" 后缀
            if (texturePath.rfind("textures/", 0) == 0) texturePath = texturePath.substr(9);
            if (texturePath.size() > 4 && texturePath.compare(texturePath.size() - 4, 4, ".png") == 0)
                texturePath = texturePath.substr(0, texturePath.size() - 4);
        }

        const CreateCTEntry* entry = FindCreateCTEntry(matNs, texturePath);
        if (!entry) continue;

        // 计算连接上下文
        FaceType ft = face.faceDirection;
        FaceDirs dirs = GetFaceDirs(ft);
        CTContext ctx = BuildContext(dirs, x, y, z, curBaseName);
        int index = GetTextureIndex(entry->type, ctx);
        int sheetSize = entry->sheetSize;

        // connected 贴图
        std::string connectedRel = GetOrSaveConnectedTexture(matNs, texturePath);
        if (connectedRel.empty()) continue;

        // 材质: 每个"原材质名"对应一个 connected 材质 (tint 沿用原材质)
        // 名字形如 "create:block/brass_casing@create_ct", 插件按 @ 前的 base 分类
        std::string ctMatKey = matNs + ":" + texturePath + "@create_ct";
        int ctMatIdx;
        auto it = ctMatIndex.find(ctMatKey);
        if (it != ctMatIndex.end()) {
            ctMatIdx = it->second;
        } else {
            Material cm;
            cm.name = ctMatKey;
            cm.texturePath = connectedRel;
            cm.tintIndex = mat.tintIndex;
            cm.type = mat.type;
            cm.aspectRatio = mat.aspectRatio;
            ctMatIdx = (int)model.materials.size();
            model.materials.push_back(cm);
            ctMatIndex[ctMatKey] = ctMatIdx;
        }

        // 重映射 UV: u'=(u+col)/size, v'=(v+size-1-row)/size
        int col = index % sheetSize;
        int row = index / sheetSize;
        for (int i = 0; i < 4; ++i) {
            int uvIdx = face.uvIndices[i];
            if (uvIdx < 0 || uvIdx * 2 + 1 >= (int)model.uvCoordinates.size()) continue;
            float u = model.uvCoordinates[uvIdx * 2];
            float v = model.uvCoordinates[uvIdx * 2 + 1];
            float nu = (u + (float)col) / (float)sheetSize;
            float nv = (v + (float)(sheetSize - 1 - row)) / (float)sheetSize;
            // 写入新 UV 条目并更新索引
            int newIdx = (int)model.uvCoordinates.size() / 2;
            model.uvCoordinates.push_back(nu);
            model.uvCoordinates.push_back(nv);
            face.uvIndices[i] = newIdx;
        }

        face.materialIndex = ctMatIdx;
        applied = true;
    }
    return applied;
}
