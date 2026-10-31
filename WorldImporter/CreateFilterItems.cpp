#include "CreateFilterItems.h"
#include "ItemRender.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using namespace std;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Direction.from3DDataValue: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
enum Dir : int { Down = 0, Up = 1, North = 2, South = 3, West = 4, East = 5 };

struct V3 { double x, y, z; };

int AxisOf(Dir d) { return (d == Down || d == Up) ? 1 : (d == North || d == South) ? 2 : 0; }
bool IsHorizontal(Dir d) { return AxisOf(d) != 1; }

Dir Opposite(Dir d) {
    switch (d) {
    case Down: return Up;
    case Up: return Down;
    case North: return South;
    case South: return North;
    case West: return East;
    default: return West;
    }
}

// Direction.getClockWise() around Y.
Dir ClockWise(Dir d) {
    switch (d) {
    case North: return East;
    case East: return South;
    case South: return West;
    case West: return North;
    default: return d;
    }
}

// AngleHelper.horizontalAngle / verticalAngle (degrees).
float HorizontalAngle(Dir d) {
    switch (d) {
    case South: return 0.f;
    case West: return -90.f;
    case North: return 180.f;
    case East: return -270.f;
    default: return 0.f;
    }
}
float VerticalAngle(Dir d) { return d == Up ? -90.f : d == Down ? 90.f : 0.f; }

const int kNormal[6][3] = { {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0} };

// VecHelper.rotate / rotateCentered / voxelSpace.
V3 Rotate(V3 v, double deg, int axis) {
    if (deg == 0.0) return v;
    const double a = deg * kPi / 180.0;
    const double s = std::sin(a), c = std::cos(a);
    if (axis == 0) return { v.x, v.y * c - v.z * s, v.z * c + v.y * s };
    if (axis == 1) return { v.x * c + v.z * s, v.y, v.z * c - v.x * s };
    return { v.x * c - v.y * s, v.y * c + v.x * s, v.z };
}
V3 RotateCentered(V3 v, double deg, int axis) {
    V3 r = Rotate({ v.x - 0.5, v.y - 0.5, v.z - 0.5 }, deg, axis);
    return { r.x + 0.5, r.y + 0.5, r.z + 0.5 };
}
V3 Voxel(double x, double y, double z) { return { x * 0.0625, y * 0.0625, z * 0.0625 }; }

EntityAffine RXDeg(double d) { return EntityAffine::RX(d * kPi / 180.0); }

// ValueBoxTransform.Sided: translate(getLocalOffset(southLocation)) then rotateY/rotateX.
struct Slot {
    double x = 0, y = 0, z = 0;
    EntityAffine rot;
    float scale = 0.5f;
};

Slot SidedSlot(Dir side, V3 south, float scale = 0.5f) {
    V3 loc = RotateCentered(south, HorizontalAngle(side), 1);
    loc = RotateCentered(loc, VerticalAngle(side), 0);
    const float yRot = HorizontalAngle(side) + 180.f;
    const float xRot = side == Up ? 90.f : side == Down ? 270.f : 0.f;
    Slot s;
    s.x = loc.x; s.y = loc.y; s.z = loc.z;
    s.rot = EntityAffine::RYDeg(yRot) * RXDeg(xRot);
    s.scale = scale;
    return s;
}

NbtTagPtr Child(const NbtTagPtr& parent, const string& name) {
    return parent && parent->type == TagType::COMPOUND ? getChildByName(parent, name) : nullptr;
}
int IntOf(const NbtTagPtr& tag, int fallback = 0) {
    if (!tag) return fallback;
    if (tag->type == TagType::INT) return bytesToInt(tag->payload);
    if (tag->type == TagType::BYTE) return bytesToByte(tag->payload);
    if (tag->type == TagType::SHORT) return bytesToShort(tag->payload);
    return fallback;
}
int Int(const NbtTagPtr& parent, const string& name, int fallback = 0) {
    return IntOf(Child(parent, name), fallback);
}

unordered_map<string, string> States(const string& blockName) {
    unordered_map<string, string> m;
    const size_t b = blockName.find('[');
    if (b == string::npos) return m;
    size_t i = b + 1;
    while (i < blockName.size() && blockName[i] != ']') {
        const size_t eq = blockName.find('=', i);
        if (eq == string::npos) break;
        const size_t end = blockName.find_first_of(",]", eq + 1);
        const size_t stop = end == string::npos ? blockName.size() : end;
        m.emplace(blockName.substr(i, eq - i), blockName.substr(eq + 1, stop - eq - 1));
        if (end == string::npos || blockName[end] == ']') break;
        i = end + 1;
    }
    return m;
}
string Prop(const unordered_map<string, string>& m, const string& key, const string& fallback) {
    auto it = m.find(key);
    return it == m.end() ? fallback : it->second;
}
bool PropBool(const unordered_map<string, string>& m, const string& key, bool fallback) {
    auto it = m.find(key);
    if (it == m.end()) return fallback;
    return it->second == "true";
}
Dir ParseDir(const string& s, Dir fallback) {
    if (s == "down") return Down;
    if (s == "up") return Up;
    if (s == "north") return North;
    if (s == "south") return South;
    if (s == "west") return West;
    if (s == "east") return East;
    return fallback;
}

// BakedModel.isGui3d: resolved item model gui_light is not "front".
bool IsGui3d(const string& itemId) {
    static mutex m;
    static unordered_map<string, bool> cache;
    {
        lock_guard<mutex> lock(m);
        auto it = cache.find(itemId);
        if (it != cache.end()) return it->second;
    }
    size_t colon = itemId.find(':');
    string ns = colon == string::npos ? "minecraft" : itemId.substr(0, colon);
    string path = "item/" + (colon == string::npos ? itemId : itemId.substr(colon + 1));
    bool gui3d = true;
    for (int depth = 0; depth < 16; ++depth) {
        if (path == "builtin/generated") { gui3d = false; break; }
        if (path == "builtin/entity") break;
        nlohmann::json j = GetModelJson(ns, path);
        if (!j.is_object()) break;
        if (j.contains("gui_light") && j["gui_light"].is_string()) {
            gui3d = j["gui_light"].get<string>() != "front";
            break;
        }
        if (!j.contains("parent") || !j["parent"].is_string()) break;
        string parent = j["parent"].get<string>();
        size_t c = parent.find(':');
        if (c == string::npos) ns = "minecraft", path = parent;
        else ns = parent.substr(0, c), path = parent.substr(c + 1);
    }
    lock_guard<mutex> lock(m);
    cache[itemId] = gui3d;
    return gui3d;
}

// ValueBoxRenderer.customZOffset: shafts, fences, doors, cactus.
bool CustomNudge(const string& id) {
    if (id == "create:shaft" || id == "create:cogwheel" || id == "create:large_cogwheel" ||
        id == "minecraft:cactus")
        return true;
    const size_t colon = id.find(':');
    const string path = colon == string::npos ? id : id.substr(colon + 1);
    if (path.size() > 5 && path.compare(path.size() - 5, 5, "_door") == 0) return true;
    if (path.size() > 6 && path.compare(path.size() - 6, 6, "_fence") == 0) return true;
    return false;
}

struct PreparedItem {
    bool ok = false;
    ModelData mesh;
    float itemScale = 1.f;
    float zOff = 0.f;
};

// renderItemIntoValueBox: FIXED display (already in Build) plus scale and z translate.
PreparedItem Prepare(const NbtTagPtr& stackTag) {
    PreparedItem p;
    ItemStackInfo item = ItemRender::ParseItemStack(stackTag);
    if (item.empty()) return p;
    if (!ItemRender::Build(item, ItemDisplay::Fixed, false, p.mesh)) return p;
    const bool blockItem = IsGui3d(item.id);
    p.itemScale = (blockItem ? 1.f : 0.5f) + 0.015625f;
    p.zOff = (blockItem ? 0.f : -0.15f) + (CustomNudge(item.id) ? -0.1f : 0.f);
    p.ok = true;
    return p;
}

void Place(ModelData& model, const PreparedItem& prep, const Slot& slot) {
    if (!prep.ok) return;
    ModelData mesh = prep.mesh;
    // transform() is translate, then rotate, then scale(getScale). renderItemIntoValueBox
    // then scale(itemScale) and translate(0, 0, zOffset).
    const EntityAffine pose = EntityAffine::T(slot.x, slot.y, slot.z) * slot.rot *
        EntityAffine::S(static_cast<double>(slot.scale) * prep.itemScale) *
        EntityAffine::T(0.0, 0.0, prep.zOff);
    ItemRender::Transform(mesh, pose);
    for (auto& face : mesh.faces) face.faceDirection = DO_NOT_CULL;
    if (model.vertices.empty()) model = std::move(mesh);
    else MergeModelsDirectly(model, mesh);
}

void PlaceStack(ModelData& model, const NbtTagPtr& stack, const Slot& slot) {
    Place(model, Prepare(stack), slot);
}

// --- per-block slot transforms -------------------------------------------------

// FunnelFilterSlotPositioning. Brass only (FilteringBehaviour.onlyActiveWhen(supportsFiltering)).
void RenderFunnel(ModelData& model, const string& base, const unordered_map<string, string>& st,
    const NbtTagPtr& nbt) {
    auto filter = Child(nbt, "Filter");
    PreparedItem prep = Prepare(filter);
    if (!prep.ok) return;
    const bool belt = base == "create:brass_belt_funnel";
    const Dir facing = ParseDir(Prop(st, "facing", "down"), Down);
    const string shape = Prop(st, "shape", "retracted");
    const bool vertical = !IsHorizontal(facing);
    for (int i = 0; i < 6; ++i) {
        const Dir side = static_cast<Dir>(i);
        bool active;
        if (vertical) active = IsHorizontal(side);
        else if (belt && shape == "extended") active = side == Up;
        else active = side == facing;
        if (!active) continue;

        Slot slot;
        slot.scale = 0.5f;
        if (belt) {
            if (shape == "extended") {
                V3 loc = RotateCentered(Voxel(8, 15.5, 13), HorizontalAngle(facing), 1);
                slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
                const float yRot = HorizontalAngle(facing) + (facing == Down ? 180.f : 0.f);
                const float xRot = facing == Down ? -90.f : 90.f;
                slot.rot = EntityAffine::RYDeg(yRot) * RXDeg(xRot);
            } else if (shape == "pulling" || shape == "pushing") {
                V3 loc = RotateCentered(Voxel(8, 12, 8.675f), HorizontalAngle(side), 1);
                slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
                Slot sided = SidedSlot(side, V3{ 0, 0, 0 });
                slot.rot = sided.rot * RXDeg(-22.5);
            } else {
                V3 loc = RotateCentered(Voxel(8, 13, 7.5), HorizontalAngle(side), 1);
                slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
                slot.rot = SidedSlot(side, V3{ 0, 0, 0 }).rot;
            }
        } else if (vertical) {
            V3 loc = RotateCentered(Voxel(8, facing == Down ? 14.0 : 2.0, 15.5), HorizontalAngle(side), 1);
            slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
            slot.rot = SidedSlot(side, V3{ 0, 0, 0 }).rot;
        } else {
            V3 loc = RotateCentered(Voxel(8, 12.2, 8.55f), HorizontalAngle(side), 1);
            slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
            slot.rot = SidedSlot(side, V3{ 0, 0, 0 }).rot * RXDeg(-22.5);
        }
        Place(model, prep, slot);
    }
}

// SmartChuteFilterSlotPositioning: horizontal sides, default Sided.rotate.
void RenderChute(ModelData& model, const NbtTagPtr& nbt) {
    PreparedItem prep = Prepare(Child(nbt, "Filter"));
    if (!prep.ok) return;
    for (int i = 0; i < 6; ++i) {
        const Dir side = static_cast<Dir>(i);
        if (!IsHorizontal(side)) continue;
        V3 loc = RotateCentered(Voxel(8, 11, 15.5), HorizontalAngle(side), 1);
        Slot slot = SidedSlot(side, Voxel(8, 8, 15.5));
        slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
        Place(model, prep, slot);
    }
}

// DirectionalAxisKineticBlock.getRotationAxis.
int DeployerAxis(Dir facing, bool alongFirst) {
    const int a = AxisOf(facing);
    if (a == 0) return alongFirst ? 1 : 2;
    if (a == 1) return alongFirst ? 0 : 2;
    return alongFirst ? 0 : 1;
}

// DeployerFilterSlot.
void RenderDeployer(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt) {
    PreparedItem prep = Prepare(Child(nbt, "Filter"));
    if (!prep.ok) return;
    const Dir facing = ParseDir(Prop(st, "facing", "up"), Up);
    const bool along = PropBool(st, "axis_along_first", false);
    const int rotAxis = DeployerAxis(facing, along);
    for (int i = 0; i < 6; ++i) {
        const Dir side = static_cast<Dir>(i);
        if (AxisOf(side) == AxisOf(facing)) continue;
        if (rotAxis == AxisOf(side)) continue;
        V3 vec = Voxel(8, 8, 15.5);
        vec = RotateCentered(vec, HorizontalAngle(side), 1);
        vec = RotateCentered(vec, VerticalAngle(side), 0);
        vec.x -= kNormal[facing][0] * 0.125;
        vec.y -= kNormal[facing][1] * 0.125;
        vec.z -= kNormal[facing][2] * 0.125;
        Slot slot;
        slot.x = vec.x; slot.y = vec.y; slot.z = vec.z;
        const float yRot = HorizontalAngle(side) + 180.f;
        const float xRot = side == Up ? 90.f : side == Down ? 270.f : 0.f;
        slot.rot = EntityAffine();
        if (AxisOf(side) == 1)
            slot.rot = slot.rot * EntityAffine::RYDeg(180.0 + HorizontalAngle(facing));
        slot.rot = slot.rot * EntityAffine::RYDeg(yRot) * RXDeg(xRot);
        Place(model, prep, slot);
    }
}

// DirectedDirectionalBlock.getTargetDirection.
Dir TargetDirection(const unordered_map<string, string>& st) {
    const string target = Prop(st, "target", "wall");
    if (target == "ceiling") return Up;
    if (target == "floor") return Down;
    return ParseDir(Prop(st, "facing", "north"), North);
}

// FilteredDetectorFilterSlot. hasBottom is true for the stockpile switch.
void RenderDetector(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt,
    bool hasBottom) {
    PreparedItem prep = Prepare(Child(nbt, "Filter"));
    if (!prep.ok) return;
    const Dir target = TargetDirection(st);
    const Dir facing = ParseDir(Prop(st, "facing", "north"), North);
    const V3 south = Voxel(8, 8, 15.5);
    for (int i = 0; i < 6; ++i) {
        const Dir side = static_cast<Dir>(i);
        bool active;
        if (side == target) active = false;
        else if (Opposite(target) == side) active = true;
        else if (AxisOf(target) != 1)
            active = side == Up || (side == Down && hasBottom);
        else {
            Dir d = side;
            if (target == Up) d = Opposite(d);
            active = !hasBottom ? d == facing : AxisOf(d) == AxisOf(ClockWise(facing));
        }
        if (!active) continue;
        Slot slot = SidedSlot(side, south);
        // Extra Z spin only on the top face (horizontal FACING axis is never Y).
        if (AxisOf(facing) != 1 && side == Up)
            slot.rot = slot.rot * EntityAffine::RZDeg(-HorizontalAngle(facing) + 180.0);
        Place(model, prep, slot);
    }
}

// BrassTunnelFilterSlot + SidedFilteringBehaviour. Sides list gates which faces exist.
void RenderTunnel(ModelData& model, const NbtTagPtr& nbt) {
    auto filters = Child(nbt, "Filters");
    if (!filters || filters->type != TagType::LIST) return;
    unordered_set<int> sides;
    bool haveSides = false;
    if (auto sideList = Child(nbt, "Sides"); sideList && sideList->type == TagType::LIST) {
        haveSides = true;
        for (const auto& t : sideList->children) sides.insert(IntOf(t));
    }
    for (const auto& entry : filters->children) {
        if (!entry || entry->type != TagType::COMPOUND) continue;
        const int side = Int(entry, "Side", -1);
        if (side < 0 || side > 5) continue;
        if (haveSides && !sides.count(side)) continue;
        PlaceStack(model, Child(entry, "Filter"), SidedSlot(static_cast<Dir>(side), Voxel(8, 13, 15.5)));
    }
}

// SawFilterSlot: only the upward saw has a slot.
void RenderSaw(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt) {
    if (Prop(st, "facing", "up") != "up") return;
    const bool flipped = PropBool(st, "flipped", false);
    const bool along = PropBool(st, "axis_along_first", false);
    const int offset = flipped ? -3 : 3;
    V3 loc = along ? Voxel(8 + offset, 12.5, 8) : Voxel(8, 12.5, 8 + offset);
    const float yRot = (along ? 90.f : 0.f) + (flipped ? 0.f : 180.f);
    Slot slot;
    slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
    slot.rot = EntityAffine::RYDeg(yRot) * RXDeg(90);
    PlaceStack(model, Child(nbt, "Filter"), slot);
}

// SmartFluidPipeBlockEntity.SmartPipeFilterSlot.
void RenderPipe(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt) {
    const string face = Prop(st, "face", "wall");
    const Dir facing = ParseDir(Prop(st, "facing", "north"), North);
    const float y = face == "ceiling" ? 0.55f : face == "wall" ? 11.4f : 15.45f;
    const float z = face == "ceiling" ? 4.6f : face == "wall" ? 0.55f : 4.625f;
    float angleY = HorizontalAngle(facing);
    if (face == "wall") angleY += 180.f;
    V3 loc = RotateCentered(Voxel(8, y, z), angleY, 1);
    Slot slot;
    slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
    slot.scale = 0.5f * 1.02f;
    slot.rot = EntityAffine::RYDeg(angleY) * RXDeg(face == "ceiling" ? -45.0 : 45.0);
    PlaceStack(model, Child(nbt, "Filter"), slot);
}

// HatchFilterSlot.
void RenderHatch(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt) {
    const float angle = HorizontalAngle(ParseDir(Prop(st, "facing", "north"), North));
    V3 loc = RotateCentered(Voxel(8, 5.15, 9.5), angle, 1);
    Slot slot;
    slot.x = loc.x; slot.y = loc.y; slot.z = loc.z;
    slot.scale = 0.5f * 0.965f;
    slot.rot = EntityAffine::RYDeg(angle) * RXDeg(-45);
    PlaceStack(model, Child(nbt, "Filter"), slot);
}

// BasinBlockEntity.BasinValueBox: horizontal sides, south location (8, 12, 16.05).
void RenderBasin(ModelData& model, const NbtTagPtr& nbt) {
    PreparedItem prep = Prepare(Child(nbt, "Filter"));
    if (!prep.ok) return;
    for (int i = 0; i < 6; ++i) {
        const Dir side = static_cast<Dir>(i);
        if (!IsHorizontal(side)) continue;
        Place(model, prep, SidedSlot(side, Voxel(8, 12, 16.05)));
    }
}

void RenderFixed(ModelData& model, const NbtTagPtr& nbt, double x, double y, double z, EntityAffine rot,
    float scale) {
    Slot slot;
    slot.x = x; slot.y = y; slot.z = z;
    slot.rot = std::move(rot);
    slot.scale = scale;
    PlaceStack(model, Child(nbt, "Filter"), slot);
}

// RollerBlockEntity.RollerValueBox(3).
void RenderRoller(ModelData& model, const unordered_map<string, string>& st, const NbtTagPtr& nbt) {
    const Dir facing = ParseDir(Prop(st, "facing", "south"), South);
    const float stateAngle = HorizontalAngle(facing) + 180.f;
    V3 loc = RotateCentered(Voxel(8 + 3, 15.5, 11), stateAngle, 1);
    RenderFixed(model, nbt, loc.x, loc.y, loc.z,
        EntityAffine::RYDeg(stateAngle) * RXDeg(90), 0.5f);
}

// Contraption controls use renderFlatItemIntoValueBox (not ported).

string BaseId(const string& blockName) {
    return blockName.substr(0, blockName.find('['));
}

}  // namespace

void AppendCreateFilterItems(const string& blockName, const NbtTagPtr& nbt, ModelData& model) {
    if (!nbt) return;
    const string base = BaseId(blockName);
    const auto st = States(blockName);
    if (base == "create:brass_funnel" || base == "create:brass_belt_funnel")
        RenderFunnel(model, base, st, nbt);
    else if (base == "create:smart_chute")
        RenderChute(model, nbt);
    else if (base == "create:deployer")
        RenderDeployer(model, st, nbt);
    else if (base == "create:content_observer")
        RenderDetector(model, st, nbt, false);
    else if (base == "create:stockpile_switch")
        RenderDetector(model, st, nbt, true);
    else if (base == "create:brass_tunnel")
        RenderTunnel(model, nbt);
    else if (base == "create:mechanical_saw")
        RenderSaw(model, st, nbt);
    else if (base == "create:smart_fluid_pipe")
        RenderPipe(model, st, nbt);
    else if (base == "create:item_hatch")
        RenderHatch(model, st, nbt);
    else if (base == "create:basin")
        RenderBasin(model, nbt);
    else if (base == "create:creative_crate")
        RenderFixed(model, nbt, 0.5, 0.84375, 0.5, RXDeg(90), 0.5f);
    else if (base == "create:track_observer")
        RenderFixed(model, nbt, 0.5, 15.5 / 16.0, 0.5, RXDeg(90), 0.5f);
    else if (base == "create:mechanical_roller")
        RenderRoller(model, st, nbt);
}
