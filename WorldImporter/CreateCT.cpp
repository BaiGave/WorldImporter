// CreateCT.cpp
// ==================== Create 机械动力 连接纹理实现 ====================
// 移植自 Create 的 CTSpriteShiftEntry + AllCTTypes + ConnectedTextureBehaviour:
//   - 方块模型引用原始贴图(如 block/brass_casing)的面, 运行时把 UV 重映射到
//     * 专用连接贴图(如 block/brass_casing_connected.png)的 sheet 格子上。
//   - 连接判定: 相邻方块 baseName 相同即连接 (同 Create 的 connectsTo 简化版)。
//   - sheet 格子编号由 getTextureIndex 根据 8 方向连接状态计算。
#include "CreateCT.h"
#include "block.h"          // GetBlockId / GetBlockById / Block
#include "nbtutils.h"       // GetBlockEntityNbt / bytesToInt
#include "GlobalCache.h"    // 贴图缓存
#include "fileutils.h"      // string_to_wstring
#include <Windows.h>
#include <fstream>
#include <sstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

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
    // 方块行为表在首次使用时构建 (见 GetBehaviour)
}

// ========= 连接状态 =========
struct CTContext {
    bool up = false, down = false, left = false, right = false;
    bool topLeft = false, topRight = false, bottomLeft = false, bottomRight = false;
};

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

// ========= 方向工具 =========
struct Offset { int x, y, z; };
static Offset Add(Offset a, Offset b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static Offset Mul(Offset a, int k) { return { a.x * k, a.y * k, a.z * k }; }
static Offset Opposite(Offset o) { return { -o.x, -o.y, -o.z }; }
static bool IsZero(Offset o) { return o.x == 0 && o.y == 0 && o.z == 0; }

static Offset DirVec(FaceType f) {
    switch (f) {
    case FaceType::UP: return { 0, 1, 0 };
    case FaceType::DOWN: return { 0, -1, 0 };
    case FaceType::NORTH: return { 0, 0, -1 };
    case FaceType::SOUTH: return { 0, 0, 1 };
    case FaceType::WEST: return { -1, 0, 0 };
    case FaceType::EAST: return { 1, 0, 0 };
    default: return { 0, 0, 0 };
    }
}
static char AxisOf(FaceType f) {
    if (f == FaceType::WEST || f == FaceType::EAST) return 'x';
    if (f == FaceType::UP || f == FaceType::DOWN) return 'y';
    return 'z';
}
static char AxisOfVec(Offset o) { return o.x != 0 ? 'x' : (o.y != 0 ? 'y' : 'z'); }
static bool IsPositive(FaceType f) { return f == FaceType::UP || f == FaceType::SOUTH || f == FaceType::EAST; }
static bool IsHorizontalFace(FaceType f) { return AxisOf(f) != 'y'; }
static Offset AxisDir(char axis, bool positive) {
    int s = positive ? 1 : -1;
    if (axis == 'x') return { s, 0, 0 };
    if (axis == 'y') return { 0, s, 0 };
    return { 0, 0, s };
}
// Direction.getClockWise / getCounterClockWise (绕 Y 轴, 仅水平方向)
static Offset ClockWise(Offset o) { return { -o.z, o.y, o.x }; }        // N(0,0,-1) -> E(1,0,0)
static Offset CounterClockWise(Offset o) { return { o.z, o.y, -o.x }; } // N -> W

static const char* DirName(Offset o) {
    if (o.y > 0) return "up";
    if (o.y < 0) return "down";
    if (o.z < 0) return "north";
    if (o.z > 0) return "south";
    if (o.x < 0) return "west";
    return "east";
}
static Offset DirFromName(const std::string& n) {
    if (n == "up") return { 0, 1, 0 };
    if (n == "down") return { 0, -1, 0 };
    if (n == "north") return { 0, 0, -1 };
    if (n == "south") return { 0, 0, 1 };
    if (n == "west") return { -1, 0, 0 };
    if (n == "east") return { 1, 0, 0 };
    return { 0, 0, 0 };
}

// 模型面无 cullface 时(DO_NOT_CULL, 如玻璃板中间的面), Create 的 CTModel 用 quad
// 自身朝向 (lightFace) 决定 CT, 这里按几何法线推断 (同 CTM.cpp InferDirectionFromFace)。
static FaceType InferFaceDirection(const ModelData& model, const Face& face) {
    for (int i = 0; i < 3; ++i) {
        if (face.vertexIndices[i] < 0 || face.vertexIndices[i] * 3 + 2 >= (int)model.vertices.size())
            return FaceType::DO_NOT_CULL;
    }
    const float* v0 = &model.vertices[face.vertexIndices[0] * 3];
    const float* v1 = &model.vertices[face.vertexIndices[1] * 3];
    const float* v2 = &model.vertices[face.vertexIndices[2] * 3];
    float ax = v1[0] - v0[0], ay = v1[1] - v0[1], az = v1[2] - v0[2];
    float bx = v2[0] - v0[0], by = v2[1] - v0[1], bz = v2[2] - v0[2];
    float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
    float axf = std::abs(nx), ayf = std::abs(ny), azf = std::abs(nz);
    if (axf < 1e-8f && ayf < 1e-8f && azf < 1e-8f) return FaceType::DO_NOT_CULL;
    if (axf >= ayf && axf >= azf) return nx > 0 ? FaceType::EAST : FaceType::WEST;
    if (ayf >= axf && ayf >= azf) return ny > 0 ? FaceType::UP : FaceType::DOWN;
    return nz > 0 ? FaceType::SOUTH : FaceType::NORTH;
}

// ========= 方块信息 (id + 状态) =========
struct BlockInfo {
    bool valid = false;
    std::string id;   // "create:xxx" (不含状态)
    std::unordered_map<std::string, std::string> st;
    const std::string& Get(const std::string& k) const {
        static const std::string empty;
        auto it = st.find(k);
        return it == st.end() ? empty : it->second;
    }
};

static BlockInfo ParseBlockInfo(const std::string& full, const std::string& defaultNs) {
    BlockInfo b;
    b.valid = true;
    size_t br = full.find('[');
    std::string base = br == std::string::npos ? full : full.substr(0, br);
    b.id = base.find(':') == std::string::npos ? defaultNs + ":" + base : base;
    if (br != std::string::npos) {
        size_t close = full.find(']', br);
        std::string props = full.substr(br + 1, (close == std::string::npos ? full.size() : close) - br - 1);
        std::stringstream ss(props);
        std::string kv;
        while (std::getline(ss, kv, ',')) {
            size_t eq = kv.find('=');
            if (eq != std::string::npos) b.st[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    return b;
}

static BlockInfo GetBlockInfoAt(int x, int y, int z) {
    int id = GetBlockId(x, y, z);
    if (id < 0) return BlockInfo();
    Block nb = GetBlockById(id);
    if (nb.air) { BlockInfo b; b.valid = true; b.id = "minecraft:air"; return b; }
    return ParseBlockInfo(nb.name, "minecraft");
}

// ========= Create 的 CT 行为 (ConnectedTextureBehaviour 子类) =========
// 数据来源: Create 1.20.1 (Fabric 6.0.8) AllSpriteShifts / AllBlocks / AllPaletteBlocks /
// PaletteBlockPattern / BuilderTransformers。Create 按"方块"注册 CT 行为, 行为再按面和
// 原贴图(sprite)选择 sprite shift; 同一张原贴图在不同方块上可映射到不同的 connected
// sheet (例如 framed_glass 贴图: 普通/横向/竖向框架玻璃), 所以这里按方块查表。
struct ShiftDef {
    std::string original;   // 原贴图路径, 如 "block/brass_casing"
    std::string target;     // connected sheet 路径, 如 "block/brass_casing_connected"
    CreateCTType type = CreateCTType::OMNIDIRECTIONAL;
    bool Valid() const { return !original.empty(); }
};

enum class CTKind {
    ENCASED,        // EncasedCTBehaviour (机壳 / 齿轮箱 / 套管)
    SIMPLE,         // SimpleCTBehaviour (框架玻璃)
    HORIZONTAL,     // HorizontalCTBehaviour(layer=a, top=b) (窗户 / 层叠石材 / 铁路机壳)
    PANE,           // GlassPaneCTBehaviour (玻璃板 / 窗格)
    WEATHERED,      // WeatheredIronWindowCTBehaviour (4 张 sheet 随机)
    WEATHERED_PANE, // WeatheredIronWindowPaneCTBehaviour
    ROOF,           // RoofBlockCTBehaviour (铜瓦/铜砖, 仅顶面)
    PILLAR,         // RotatedPillarCTBehaviour (石柱)
    CHASSIS,        // LinearChassisBlock.ChassisCTBehaviour
    SCAFFOLD,       // MetalScaffoldingCTBehaviour
    TANK,           // FluidTankCTBehaviour
    VAULT,          // ItemVaultCTBehaviour
    GIRDER          // GirderCTBehaviour
};

struct BehaviourDef {
    CTKind kind = CTKind::SIMPLE;
    ShiftDef a, b, c;            // 含义随 kind (见 GetShiftFor)
    std::vector<ShiftDef> list;  // WEATHERED: 4 张; VAULT: front/top/side/bottom x (medium,large)
};

static ShiftDef MakeCT(CreateCTType t, const std::string& src, const std::string& dst = "") {
    ShiftDef s;
    s.original = "block/" + src;
    s.target = "block/" + (dst.empty() ? src : dst) + "_connected";
    s.type = t;
    return s;
}

// CasingConnectivity: 能与机壳连接的方块及其有效面 (cc.makeCasing / cc.make)
enum class CasingSide { ALL, AXIS_ONLY, NOT_AXIS, PIPE_CLOSED, COG_AXIS };
struct CasingEntry { std::string casing; CasingSide side; };

static std::unordered_map<std::string, BehaviourDef> g_behaviours;
static std::unordered_map<std::string, CasingEntry> g_casingEntries;
static std::once_flag g_behaviourOnce;

static void BuildBehaviours() {
    using T = CreateCTType;
    auto add = [](const std::string& id, CTKind kind, ShiftDef a = ShiftDef(), ShiftDef b = ShiftDef(), ShiftDef c = ShiftDef()) {
        BehaviourDef d; d.kind = kind; d.a = a; d.b = b; d.c = c;
        g_behaviours["create:" + id] = d;
    };
    // 机壳 (BuilderTransformers.casing -> EncasedCTBehaviour + cc.makeCasing)
    for (const char* c : { "andesite", "brass", "copper", "shadow_steel", "refined_radiance" }) {
        std::string casing = std::string(c) + "_casing";
        add(casing, CTKind::ENCASED, MakeCT(T::OMNIDIRECTIONAL, casing));
        g_casingEntries["create:" + casing] = { casing, CasingSide::ALL };
    }
    // 铁路机壳: layeredCasing -> HorizontalCTBehaviour(RAILWAY_CASING_SIDE, RAILWAY_CASING)
    add("railway_casing", CTKind::HORIZONTAL, MakeCT(T::OMNIDIRECTIONAL, "railway_casing_side"), MakeCT(T::OMNIDIRECTIONAL, "railway_casing"));
    g_casingEntries["create:railway_casing"] = { "railway_casing_side", CasingSide::ALL };
    // 齿轮箱 / 铜套管: EncasedCTBehaviour + cc.make(有效面受状态限制)
    add("gearbox", CTKind::ENCASED, MakeCT(T::OMNIDIRECTIONAL, "andesite_casing"));
    g_casingEntries["create:gearbox"] = { "andesite_casing", CasingSide::AXIS_ONLY };
    add("encased_fluid_pipe", CTKind::ENCASED, MakeCT(T::OMNIDIRECTIONAL, "copper_casing"));
    g_casingEntries["create:encased_fluid_pipe"] = { "copper_casing", CasingSide::PIPE_CLOSED };
    // 套管传动杆/齿轮 (本身走 SpecialBlock, 这里仅作为机壳的连接邻居)
    for (const char* c : { "andesite", "brass" }) {
        std::string casing = std::string(c) + "_casing";
        g_casingEntries[std::string("create:") + c + "_encased_shaft"] = { casing, CasingSide::NOT_AXIS };
        g_casingEntries[std::string("create:") + c + "_encased_cogwheel"] = { casing, CasingSide::COG_AXIS };
        g_casingEntries[std::string("create:") + c + "_encased_large_cogwheel"] = { casing, CasingSide::COG_AXIS };
    }

    // 框架玻璃 (AllPaletteBlocks)
    ShiftDef fg = MakeCT(T::OMNIDIRECTIONAL, "palettes/framed_glass");
    ShiftDef hfg = MakeCT(T::HORIZONTAL_KRYPPERS, "palettes/framed_glass", "palettes/horizontal_framed_glass");
    ShiftDef vfg = MakeCT(T::VERTICAL, "palettes/framed_glass", "palettes/vertical_framed_glass");
    add("framed_glass", CTKind::SIMPLE, fg);
    add("horizontal_framed_glass", CTKind::HORIZONTAL, hfg, fg);
    add("vertical_framed_glass", CTKind::HORIZONTAL, vfg);
    add("framed_glass_pane", CTKind::PANE, fg);
    add("horizontal_framed_glass_pane", CTKind::PANE, hfg);
    add("vertical_framed_glass_pane", CTKind::PANE, vfg);

    // 窗户 (WindowGen: 方块 HorizontalCTBehaviour(ct), 窗格 GlassPaneCTBehaviour(ct))
    for (const char* w : { "oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove",
                           "crimson", "warped", "cherry", "bamboo" }) {
        ShiftDef s = MakeCT(T::VERTICAL, std::string("palettes/") + w + "_window");
        add(std::string(w) + "_window", CTKind::HORIZONTAL, s);
        add(std::string(w) + "_window_pane", CTKind::PANE, s);
    }
    ShiftDef ornate = MakeCT(T::VERTICAL, "palettes/ornate_iron_window");
    add("ornate_iron_window", CTKind::HORIZONTAL, ornate);
    add("ornate_iron_window_pane", CTKind::PANE, ornate);
    ShiftDef industrial = MakeCT(T::RECTANGLE, "palettes/industrial_iron_window");
    add("industrial_iron_window", CTKind::HORIZONTAL, industrial);
    add("industrial_iron_window_pane", CTKind::PANE, industrial);
    {
        BehaviourDef w; w.kind = CTKind::WEATHERED;
        for (int i = 1; i <= 4; ++i)
            w.list.push_back(MakeCT(T::RECTANGLE, "palettes/weathered_iron_window", "palettes/weathered_iron_window_" + std::to_string(i)));
        g_behaviours["create:weathered_iron_window"] = w;
        w.kind = CTKind::WEATHERED_PANE;
        g_behaviours["create:weathered_iron_window_pane"] = w;
    }

    // 金属脚手架: MetalScaffoldingCTBehaviour(outside, inside, casing)
    for (const char* m : { "andesite", "brass", "copper" }) {
        add(std::string(m) + "_scaffolding", CTKind::SCAFFOLD,
            MakeCT(T::HORIZONTAL, std::string("scaffold/") + m + "_scaffold"),
            MakeCT(T::HORIZONTAL, std::string("scaffold/") + m + "_scaffold_inside"),
            MakeCT(T::OMNIDIRECTIONAL, std::string(m) + "_casing"));
    }

    // 底盘: a=侧面, b=端面, c=粘性端面
    add("linear_chassis", CTKind::CHASSIS, MakeCT(T::OMNIDIRECTIONAL, "linear_chassis_side"),
        MakeCT(T::OMNIDIRECTIONAL, "linear_chassis_end"), MakeCT(T::OMNIDIRECTIONAL, "linear_chassis_end_sticky"));
    add("secondary_linear_chassis", CTKind::CHASSIS, MakeCT(T::OMNIDIRECTIONAL, "secondary_linear_chassis_side"),
        MakeCT(T::OMNIDIRECTIONAL, "linear_chassis_end"), MakeCT(T::OMNIDIRECTIONAL, "linear_chassis_end_sticky"));

    // 流体储罐: FluidTankModel(side, top, inner)
    add("fluid_tank", CTKind::TANK, MakeCT(T::RECTANGLE, "fluid_tank"), MakeCT(T::RECTANGLE, "fluid_tank_top"),
        MakeCT(T::RECTANGLE, "fluid_tank_inner"));
    add("creative_fluid_tank", CTKind::TANK, MakeCT(T::RECTANGLE, "creative_fluid_tank"),
        MakeCT(T::RECTANGLE, "creative_casing"), MakeCT(T::RECTANGLE, "creative_casing"));

    // 保险库: VAULT_<part> = Couple(medium, large), 原贴图均为 vault_<part>_small
    {
        BehaviourDef v; v.kind = CTKind::VAULT;
        for (const char* part : { "front", "top", "side", "bottom" }) {
            for (const char* size : { "medium", "large" }) {
                ShiftDef s;
                s.original = std::string("block/vault/vault_") + part + "_small";
                s.target = std::string("block/vault/vault_") + part + "_" + size;
                s.type = T::RECTANGLE;
                v.list.push_back(s);
            }
        }
        g_behaviours["create:item_vault"] = v;
    }

    // 钢梁立柱
    add("metal_girder", CTKind::GIRDER, MakeCT(T::VERTICAL, "girder_pole_side"));

    // 铜瓦/铜砖 (CopperBlockSet: 方块/台阶/楼梯, 含涂蜡; 涂蜡与未涂蜡共用贴图)
    for (const char* w : { "", "exposed_", "weathered_", "oxidized_" }) {
        std::string pref = std::string("copper/") + w;
        ShiftDef shingles = MakeCT(T::ROOF, pref + "copper_roof_top", pref + "copper_shingles_top");
        ShiftDef tiles = MakeCT(T::ROOF, pref + "copper_roof_top", pref + "copper_tiles_top");
        for (const char* wax : { "", "waxed_" }) {
            std::string p = std::string(wax) + w;
            add(p + "copper_shingles", CTKind::ROOF, shingles);
            add(p + "copper_shingle_slab", CTKind::ROOF, shingles);
            add(p + "copper_shingle_stairs", CTKind::ROOF, shingles);
            add(p + "copper_tiles", CTKind::ROOF, tiles);
            add(p + "copper_tile_slab", CTKind::ROOF, tiles);
            add(p + "copper_tile_stairs", CTKind::ROOF, tiles);
        }
    }

    // 调色板石材: LAYERED -> HorizontalCTBehaviour(LAYERED, CAP); PILLAR -> RotatedPillarCTBehaviour(PILLAR, CAP)
    for (const char* s : { "andesite", "asurine", "calcite", "crimsite", "deepslate", "diorite", "dripstone",
                           "granite", "limestone", "ochrum", "scorchia", "scoria", "tuff", "veridium" }) {
        std::string dir = "palettes/stone_types/";
        ShiftDef cap = MakeCT(T::OMNIDIRECTIONAL, dir + "cap/" + s + "_cut_cap");
        add(std::string("layered_") + s, CTKind::HORIZONTAL, MakeCT(T::HORIZONTAL_KRYPPERS, dir + "layered/" + s + "_cut_layered"), cap);
        add(std::string(s) + "_pillar", CTKind::PILLAR, MakeCT(T::RECTANGLE, dir + "pillar/" + s + "_cut_pillar"), cap);
    }
}

static const BehaviourDef* GetBehaviour(const std::string& id) {
    std::call_once(g_behaviourOnce, BuildBehaviours);
    auto it = g_behaviours.find(id);
    return it == g_behaviours.end() ? nullptr : &it->second;
}

static char StateAxis(const BlockInfo& b, char def = 'y') {
    const std::string& a = b.Get("axis");
    return a.empty() ? def : a[0];
}

// position -> 随机 (WEATHERED 的 4 张 sheet 选择; 游戏里由模型随机源决定, 这里用
// Mth.getSeed(pos) 做确定性近似)
static int PositionRandom(int x, int y, int z, int bound) {
    long long seed = (long long)(x * 3129871) ^ ((long long)z * 116129781LL) ^ (long long)y;
    seed = seed * seed * 42317861LL + seed * 11LL;
    seed >>= 16;
    unsigned long long u = (unsigned long long)seed;
    return (int)(u % (unsigned long long)bound);
}


// Item vault multiblock: Create sets LARGE when radius > 2 (Size == 3). Connectivity
// requires ConnectivityHandler; we approximate via BE Controller / Size.
static bool VaultControllerAt(int x, int y, int z, int& cx, int& cy, int& cz, int& sizeOut) {
    NbtTagPtr nbt = GetBlockEntityNbt(x, y, z);
    if (!nbt) return false;
    auto sizeTag = getChildByName(nbt, "Size");
    if (sizeTag && sizeTag->type == TagType::INT) {
        cx = x; cy = y; cz = z;
        sizeOut = bytesToInt(sizeTag->payload);
        return true;
    }
    auto ctrl = getChildByName(nbt, "Controller");
    if (!ctrl) return false;
    auto xt = getChildByName(ctrl, "X");
    auto yt = getChildByName(ctrl, "Y");
    auto zt = getChildByName(ctrl, "Z");
    if (!xt || !yt || !zt) return false;
    cx = bytesToInt(xt->payload);
    cy = bytesToInt(yt->payload);
    cz = bytesToInt(zt->payload);
    NbtTagPtr cNbt = GetBlockEntityNbt(cx, cy, cz);
    if (!cNbt) return false;
    auto cSize = getChildByName(cNbt, "Size");
    if (!cSize || cSize->type != TagType::INT) return false;
    sizeOut = bytesToInt(cSize->payload);
    return true;
}

static bool SameVaultMultiblock(int x, int y, int z, Offset otherPos) {
    int cx1, cy1, cz1, s1, cx2, cy2, cz2, s2;
    if (!VaultControllerAt(x, y, z, cx1, cy1, cz1, s1)) return false;
    if (!VaultControllerAt(otherPos.x, otherPos.y, otherPos.z, cx2, cy2, cz2, s2)) return false;
    return cx1 == cx2 && cy1 == cy2 && cz1 == cz2;
}

// getShift(state, direction, sprite) 的移植。返回 nullptr 表示该面不做 CT。
static const ShiftDef* GetShiftFor(const BehaviourDef& d, const BlockInfo& self, FaceType face,
    const std::string& sprite, int x, int y, int z) {
    auto ok = [&](const ShiftDef& s) -> const ShiftDef* { return s.Valid() ? &s : nullptr; };
    switch (d.kind) {
    case CTKind::ENCASED:
    case CTKind::SIMPLE:
    case CTKind::PANE:
        return ok(d.a);
    case CTKind::HORIZONTAL:
        return IsHorizontalFace(face) ? ok(d.a) : ok(d.b);
    case CTKind::WEATHERED:
    case CTKind::WEATHERED_PANE:
        if (!IsHorizontalFace(face) || d.list.empty()) return nullptr;
        return &d.list[PositionRandom(x, y, z, (int)d.list.size())];
    case CTKind::ROOF:
        return face == FaceType::UP ? ok(d.a) : nullptr;
    case CTKind::PILLAR:
        return AxisOf(face) == StateAxis(self) ? ok(d.b) : ok(d.a);
    case CTKind::CHASSIS: {
        char axis = StateAxis(self);
        if (AxisOf(face) != axis) return ok(d.a);
        const std::string& sticky = self.Get(IsPositive(face) ? "sticky_top" : "sticky_bottom");
        return sticky == "true" ? ok(d.c) : ok(d.b);
    }
    case CTKind::SCAFFOLD:
        if (IsHorizontalFace(face) && sprite == d.b.original) return ok(d.b);
        return IsHorizontalFace(face) ? ok(d.a) : ok(d.c);
    case CTKind::TANK:
        if (!IsHorizontalFace(face) && sprite == d.c.original) return ok(d.c);
        return IsHorizontalFace(face) ? ok(d.a) : ok(d.b);
    case CTKind::VAULT: {
        const std::string& ax = self.Get("axis");
        if (ax.empty() || d.list.size() < 8) return nullptr;
        int part = AxisOf(face) == ax[0] ? 0 : face == FaceType::UP ? 1 : face == FaceType::DOWN ? 3 : 2;
        // Create: LARGE iff radius > 2 (i.e. Size == 3). Prefer BE Size over blockstate.
        bool large = self.Get("large") == "true";
        int cx, cy, cz, size = -1;
        if (VaultControllerAt(x, y, z, cx, cy, cz, size) && size > 0)
            large = size > 2;
        return &d.list[part * 2 + (large ? 1 : 0)];
    }
    case CTKind::GIRDER:
        if (self.Get("x") == "false" && self.Get("z") == "false" && IsHorizontalFace(face)) return ok(d.a);
        return nullptr;
    }
    return nullptr;
}

// ---- 朝向 (getUpDirection / getRightDirection / reverseUVs*) ----
static Offset DefaultUp(FaceType face) { return IsHorizontalFace(face) ? Offset{ 0, 1, 0 } : Offset{ 0, 0, -1 }; }
static Offset DefaultRight(FaceType face) { return AxisOf(face) == 'x' ? Offset{ 0, 0, 1 } : Offset{ -1, 0, 0 }; }

// RotatedPillarCTBehaviour / ItemVaultCTBehaviour 共用的 up/right 规则
static Offset AxisAlignedUp(char axis, FaceType face) {
    if (axis == 'y') return DefaultUp(face);
    bool alongX = axis == 'x';
    if (!IsHorizontalFace(face) && alongX) return ClockWise(DefaultUp(face));
    if (AxisOf(face) == axis || !IsHorizontalFace(face)) return DefaultUp(face);
    return AxisDir(axis, alongX);
}
static Offset AxisAlignedRight(char axis, FaceType face) {
    if (axis == 'y') return DefaultRight(face);
    if (!IsHorizontalFace(face) && axis == 'x') return ClockWise(DefaultRight(face));
    if (AxisOf(face) == axis || !IsHorizontalFace(face)) return DefaultRight(face);
    return AxisDir('y', IsPositive(face));
}

struct FrameInfo { Offset up, right; bool flipH = false, flipV = false; };

static FrameInfo GetFrame(const BehaviourDef& d, const BlockInfo& self, FaceType face) {
    FrameInfo f{ DefaultUp(face), DefaultRight(face), false, false };
    switch (d.kind) {
    case CTKind::PANE:
    case CTKind::WEATHERED_PANE:
        f.flipH = !IsPositive(face);
        break;
    case CTKind::PILLAR: {
        char axis = StateAxis(self);
        f.up = AxisAlignedUp(axis, face);
        f.right = AxisAlignedRight(axis, face);
        bool rev = false;
        if (axis == 'x') rev = !IsPositive(face) && AxisOf(face) != 'x';
        else if (axis == 'z') rev = face != FaceType::NORTH && !IsPositive(face);
        f.flipH = rev;
        f.flipV = rev;
        if (axis == 'x' && face == FaceType::NORTH) f.flipV = false;
        if (axis == 'z' && face == FaceType::WEST) f.flipV = false;
        break;
    }
    case CTKind::VAULT: {
        const std::string& ax = self.Get("axis");
        if (!ax.empty()) { f.up = AxisAlignedUp(ax[0], face); f.right = AxisAlignedRight(ax[0], face); }
        break;
    }
    case CTKind::CHASSIS: {
        char axis = StateAxis(self);
        if (AxisOf(face) != axis) f.up = AxisDir(axis, true);
        if (axis != AxisOf(face) && axis != 'y')
            f.right = IsHorizontalFace(face) ? Offset{ 0, -1, 0 } : (axis == 'x' ? Offset{ 0, 0, -1 } : Offset{ 1, 0, 0 });
        bool end = AxisOf(face) == axis;
        bool rev = (end && axis != 'y' && IsPositive(face)) || (!end && axis != 'y' && face == FaceType::DOWN);
        f.flipH = (!end && axis == 'x' && IsHorizontalFace(face)) ? true : rev;
        f.flipV = rev;
        break;
    }
    default:
        break;
    }
    return f;
}

// ---- 连接判定 (connectsTo / isBeingBlocked) ----
struct CTQuery {
    const BehaviourDef* def;
    const BlockInfo* self;
    int x, y, z;
    FaceType face;
};

static bool CasingSideValid(const CasingEntry& e, const BlockInfo& b, FaceType face) {
    switch (e.side) {
    case CasingSide::ALL: return true;
    case CasingSide::AXIS_ONLY: return AxisOf(face) == StateAxis(b);
    case CasingSide::NOT_AXIS: return AxisOf(face) != StateAxis(b);
    case CasingSide::PIPE_CLOSED: return b.Get(DirName(DirVec(face))) != "true";
    case CasingSide::COG_AXIS:
        return AxisOf(face) == StateAxis(b) &&
            b.Get(IsPositive(face) ? "top_shaft" : "bottom_shaft") != "true";
    }
    return false;
}

static bool ConnectsTo(const CTQuery& q, const BlockInfo& other, Offset otherPos,
    Offset primary, Offset secondary);

// ConnectedTextureBehaviour.isBeingBlocked: 邻居前方(沿面法线)若有可连接的满方块, 视为被挡。
// 参与连接的方块均为满方块形状, 故 isFaceFull 由 ConnectsTo 本身隐含。
static bool IsBeingBlocked(const CTQuery& q, Offset otherPos) {
    Offset n = DirVec(q.face);
    Offset blockingPos = Add(otherPos, n);
    char ax = AxisOf(q.face);
    int selfC = ax == 'x' ? q.x : ax == 'y' ? q.y : q.z;
    int otherC = ax == 'x' ? otherPos.x : ax == 'y' ? otherPos.y : otherPos.z;
    if (selfC != otherC) return false;
    BlockInfo blocking = GetBlockInfoAt(blockingPos.x, blockingPos.y, blockingPos.z);
    if (!blocking.valid) return false;
    return ConnectsTo(q, blocking, blockingPos, Offset{ 0, 0, 0 }, Offset{ 0, 0, 0 });
}

static bool DefaultConnects(const CTQuery& q, const BlockInfo& other, Offset otherPos) {
    return !IsBeingBlocked(q, otherPos) && other.id == q.self->id;
}

// RoofBlockCTBehaviour 辅助: 碰撞箱顶部高度近似 (台阶 bottom=0.5, 空气=0, 其余满方块=1)
static double CollisionTop(const BlockInfo& b) {
    if (!b.valid || b.id == "minecraft:air" || b.id == "minecraft:cave_air" || b.id == "minecraft:void_air") return 0.0;
    if (b.Get("type") == "bottom" && b.id.find("slab") != std::string::npos) return 0.5;
    return 1.0;
}
static bool IsSlab(const BlockInfo& b) { return b.id.find("slab") != std::string::npos && !b.Get("type").empty(); }
static bool IsCopperSound(const BlockInfo& b) { return b.id.find("copper") != std::string::npos; }
static bool RoofConnects(const BlockInfo& self, const BlockInfo& other) {
    double top = CollisionTop(self);
    double topOther = IsCopperSound(other) ? CollisionTop(other) : 0.0;
    return std::abs(top - topOther) < 1e-5;
}
static bool RoofConnectsHigh(const BlockInfo& self, const BlockInfo& other, const BlockInfo& aboveOther) {
    if (IsSlab(self) && IsSlab(other))
        if (self.Get("type") == "bottom" && other.Get("type") != "bottom") return true;
    if (IsSlab(self) && self.Get("type") == "bottom") {
        double top = CollisionTop(self), topOther = CollisionTop(other);
        return std::abs(top - topOther) >= 1e-5 && topOther > top;
    }
    return CollisionTop(aboveOther) > 0;
}

static bool PillarConnection(const BlockInfo& b, Offset side) {
    char axis = StateAxis(b);
    char sa = AxisOfVec(side);
    if (sa == axis) return false;
    const char* prop = nullptr;
    std::string n = DirName(side);
    if (axis == 'x') {
        if (n == "up") prop = "east"; else if (n == "north") prop = "north";
        else if (n == "south") prop = "south"; else if (n == "down") prop = "west";
    } else if (axis == 'y') {
        if (n == "east") prop = "east"; else if (n == "north") prop = "north";
        else if (n == "south") prop = "south"; else if (n == "west") prop = "west";
    } else {
        if (n == "up") prop = "west"; else if (n == "west") prop = "south";
        else if (n == "east") prop = "north"; else if (n == "down") prop = "east";
    }
    return prop != nullptr && b.Get(prop) == "true";
}

static bool ConnectsTo(const CTQuery& q, const BlockInfo& other, Offset otherPos,
    Offset primary, Offset secondary) {
    const BlockInfo& self = *q.self;
    const BehaviourDef& d = *q.def;
    if (!other.valid) return false;
    switch (d.kind) {
    case CTKind::ENCASED: {
        if (IsBeingBlocked(q, otherPos)) return false;
        auto e1 = g_casingEntries.find(self.id);
        auto e2 = g_casingEntries.find(other.id);
        if (e1 == g_casingEntries.end() || e2 == g_casingEntries.end()) return false;
        if (!CasingSideValid(e1->second, self, q.face) || !CasingSideValid(e2->second, other, q.face)) return false;
        return e1->second.casing == e2->second.casing;
    }
    case CTKind::SIMPLE:
    case CTKind::HORIZONTAL:
    case CTKind::WEATHERED:
        return DefaultConnects(q, other, otherPos);
    case CTKind::PANE:
    case CTKind::WEATHERED_PANE:
        return other.id == self.id;
    case CTKind::ROOF: {
        Offset above = Add(otherPos, Offset{ 0, 1, 0 });
        if (RoofConnects(self, other) ||
            RoofConnectsHigh(self, other, GetBlockInfoAt(above.x, above.y, above.z)))
            return true;
        if (!IsZero(primary) && !IsZero(secondary)) return false;
        for (Offset off : { primary, secondary }) {
            if (IsZero(off) || off.y != 0) continue;
            for (Offset side : { ClockWise(off), CounterClockWise(off) }) {
                Offset p = { q.x + side.x, q.y + side.y, q.z + side.z };
                if (RoofConnectsHigh(self, GetBlockInfoAt(p.x, p.y, p.z), GetBlockInfoAt(p.x, p.y + 1, p.z)))
                    return true;
            }
        }
        return false;
    }
    case CTKind::PILLAR: {
        if (other.id != self.id) return false;
        char axis = StateAxis(self);
        if (StateAxis(other) != axis) return false;
        if (AxisOf(q.face) == axis && IsBeingBlocked(q, otherPos)) return false;
        if (!IsZero(primary) && AxisOfVec(primary) != axis && !PillarConnection(self, primary)) return false;
        if (!IsZero(secondary) && AxisOfVec(secondary) != axis) {
            if (!PillarConnection(self, secondary)) return false;
            if (!PillarConnection(other, Opposite(secondary))) return false;
        }
        return true;
    }
    case CTKind::CHASSIS: {
        char axis = StateAxis(self);
        bool superConnect = AxisOf(q.face) == axis ? DefaultConnects(q, other, otherPos) : other.id == self.id;
        return superConnect && StateAxis(other) == axis;
    }
    case CTKind::SCAFFOLD: {
        if (AxisOf(q.face) == 'y' && IsBeingBlocked(q, otherPos)) return false;
        return other.id == self.id && self.Get("bottom") == "true" && other.Get("bottom") == "true";
    }
    case CTKind::TANK:
        // 游戏里还要求两者属于同一个多方块储罐 (ConnectivityHandler, 依赖方块实体数据)
        return other.id == self.id;
    case CTKind::VAULT:
        // Create: state == other && ConnectivityHandler.isConnected (same multiblock).
        if (other.id != self.id) return false;
        if (SameVaultMultiblock(q.x, q.y, q.z, otherPos)) return true;
        // Fallback when BE missing: same blockstate (axis+large).
        return other.st == self.st;
    case CTKind::GIRDER:
        return other.id == self.id && other.Get("x") == "false" && other.Get("z") == "false";
    }
    return false;
}

// RoofBlockCTBehaviour.getStairMapping (直立楼梯顶面)
static CTContext RoofStairContext(const BlockInfo& self) {
    CTContext c;
    const std::string& shape = self.Get("shape");
    Offset facing = DirFromName(self.Get("facing"));
    if (shape == "outer_left" || shape == "inner_left") facing = CounterClockWise(facing);
    int type = shape == "straight" ? 0 : (shape == "inner_left" || shape == "inner_right") ? 1 : 2;
    // Direction.get2DDataValue: SOUTH=0, WEST=1, NORTH=2, EAST=3
    int rot = facing.z > 0 ? 0 : facing.x < 0 ? 1 : facing.z < 0 ? 2 : 3;
    c.up = type >= 2;
    c.right = type % 2 == 1;
    c.left = rot >= 2;
    c.down = rot % 2 == 1;
    return c;
}

// ConnectedTextureBehaviour.buildContext 移植
static CTContext BuildContext(const CTQuery& q, const FrameInfo& fr) {
    bool positive = IsPositive(q.face);
    Offset h = positive ? Opposite(fr.right) : fr.right;
    Offset v = fr.up;
    if (q.face == FaceType::DOWN) { v = Opposite(v); h = Opposite(h); }
    int sh = fr.flipH ? -1 : 1;
    int sv = fr.flipV ? -1 : 1;
    auto test = [&](int dh, int dv) -> bool {
        Offset target = Add(Add(Offset{ q.x, q.y, q.z }, Mul(h, dh)), Mul(v, dv));
        BlockInfo other = GetBlockInfoAt(target.x, target.y, target.z);
        Offset primary = dh == 0 ? Offset{ 0, 0, 0 } : (dh == -1 ? Opposite(h) : h);
        Offset secondary = dv == 0 ? Offset{ 0, 0, 0 } : (dv == -1 ? Opposite(v) : v);
        return ConnectsTo(q, other, target, primary, secondary);
    };
    CTContext c;
    c.up = test(0, sv);
    c.down = test(0, -sv);
    c.left = test(-sh, 0);
    c.right = test(sh, 0);
    c.topLeft = c.up && c.left && test(-sh, sv);
    c.topRight = c.up && c.right && test(sh, sv);
    c.bottomLeft = c.down && c.left && test(-sh, -sv);
    c.bottomRight = c.down && c.right && test(sh, -sv);
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
    const std::string& connected) {
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

    // 保存到 <exe>/textures/<ns>/<connected>.png (connected 例如 block/brass_casing_connected)
    std::string exeDir = GetExeDir();
    std::string root = JoinPath(exeDir, "textures", ns);
    EnsureDir(JoinPath(exeDir, "textures"));
    EnsureDir(root);
    std::string dir = root;
    {
        size_t lastSlash = connected.find_last_of('/');
        std::stringstream ss(lastSlash == std::string::npos ? std::string() : connected.substr(0, lastSlash));
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty()) continue;
            dir = JoinPath(dir, seg);
            EnsureDir(dir);
        }
    }
    // connected may contain '/' resource segments; convert to native seps for the FS path
    std::string connectedFs = connected;
    for (char& ch : connectedFs) { if (ch == '/') ch = PathSep(); }
    std::string base = JoinPath(root, connectedFs);
    std::string fullPath = base + ".png";
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
        std::string rel = "textures/" + ns + "/" + connected + ".png";
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
                std::ofstream pbrOut(base + suffix + ".png", std::ios::binary);
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

    // 方块 CT 行为 (Create 按方块注册; 未注册的方块不做 CT)
    BlockInfo self = ParseBlockInfo(blockName, ns);
    const BehaviourDef* def = GetBehaviour(self.id);
    if (!def) return false;

    // 材质名 -> 新材质索引 (避免重复添加 connected 材质)
    std::unordered_map<std::string, int> ctMatIndex;
    bool applied = false;

    for (auto& face : model.faces) {
        if (face.materialIndex < 0 || face.materialIndex >= (int)model.materials.size()) continue;
        const Material& mat = model.materials[face.materialIndex];

        // 提取命名空间 + 贴图路径, 例如 "create" + "block/brass_casing"。
        // 区块缓存里材质名已被 RenameBlockMaterials 改成 "create:andesite_casing" /
        // "create:blockId#key~texshort", 只有 texturePath ("textures/<ns>/<path>.png")
        // 还保留完整贴图路径, 优先用它 (同 CTM.cpp ApplyCtmToBlockModel)。
        std::string matNs = ns;
        std::string texturePath;
        {
            const std::string& tp = mat.texturePath;
            if (tp.size() > 4 && tp.compare(tp.size() - 4, 4, ".png") == 0) {
                std::string p = tp.substr(0, tp.size() - 4);
                if (p.rfind("textures/", 0) == 0) p = p.substr(9);
                size_t slash = p.find('/');
                if (slash != std::string::npos) { matNs = p.substr(0, slash); texturePath = p.substr(slash + 1); }
            }
            if (texturePath.empty()) {
                const std::string& mn = mat.name;
                size_t colon = mn.find(':');
                if (colon != std::string::npos) { matNs = mn.substr(0, colon); texturePath = mn.substr(colon + 1); }
                else texturePath = mn;
                // 去掉可能的 "textures/" 前缀和 ".png" 后缀
                if (texturePath.rfind("textures/", 0) == 0) texturePath = texturePath.substr(9);
                if (texturePath.size() > 4 && texturePath.compare(texturePath.size() - 4, 4, ".png") == 0)
                    texturePath = texturePath.substr(0, texturePath.size() - 4);
            }
        }

        // 面朝向: cullface 优先, 无 cullface 的面按几何法线 (Create 用 quad 朝向)
        FaceType ft = face.faceDirection;
        if (ft == FaceType::DO_NOT_CULL || ft == FaceType::UNKNOWN) {
            ft = InferFaceDirection(model, face);
            if (ft == FaceType::DO_NOT_CULL) continue;
        }

        // getShift + "sprite == shift.getOriginal()" (CTModel)
        if (matNs != "create") continue;
        const ShiftDef* shift = GetShiftFor(*def, self, ft, texturePath, x, y, z);
        if (!shift || shift->original != texturePath) continue;

        // 计算连接上下文
        CreateCTType type = shift->type;
        CTContext ctx;
        if (def->kind == CTKind::ROOF && !self.Get("shape").empty() && self.Get("half") == "bottom") {
            type = CreateCTType::ROOF_STAIR;   // RoofBlockCTBehaviour: 直立楼梯
            ctx = RoofStairContext(self);
        } else {
            CTQuery q{ def, &self, x, y, z, ft };
            ctx = BuildContext(q, GetFrame(*def, self, ft));
        }
        int index = GetTextureIndex(type, ctx);
        int sheetSize = GetSheetSize(type);

        // connected 贴图
        std::string connectedRel = GetOrSaveConnectedTexture(matNs, shift->target);
        if (connectedRel.empty()) continue;

        // 材质: 每个"原材质名"对应一个 connected 材质 (tint 沿用原材质)
        // 名字形如 "create:block/brass_casing@create_ct", 插件按 @ 前的 base 分类。
        // 同一原贴图可对应不同 sheet (如 framed_glass), 故按 sheet 命名。
        std::string sheetName = shift->target;
        if (sheetName.size() > 10 && sheetName.compare(sheetName.size() - 10, 10, "_connected") == 0)
            sheetName = sheetName.substr(0, sheetName.size() - 10);
        std::string ctMatKey = matNs + ":" + sheetName + "@create_ct";
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
