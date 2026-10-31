#include "CreateDepotItems.h"
#include "ItemRender.h"
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>

using namespace std;

namespace {
    // java.util.Random (only nextFloat is needed).
    struct JavaRandom {
        int64_t seed;
        explicit JavaRandom(int64_t s) : seed((s ^ 0x5DEECE66DLL) & ((1LL << 48) - 1)) {}
        int Next(int bits) {
            seed = (seed * 0x5DEECE66DLL + 0xBLL) & ((1LL << 48) - 1);
            return static_cast<int>(seed >> (48 - bits));
        }
        float NextFloat() { return Next(24) / static_cast<float>(1 << 24); }
    };

    NbtTagPtr Child(const NbtTagPtr& parent, const string& name) {
        return parent && parent->type == TagType::COMPOUND ? getChildByName(parent, name) : nullptr;
    }
    int Int(const NbtTagPtr& parent, const string& name, int fallback = 0) {
        auto t = Child(parent, name);
        if (!t) return fallback;
        if (t->type == TagType::INT) return bytesToInt(t->payload);
        if (t->type == TagType::BYTE) return bytesToByte(t->payload);
        return fallback;
    }
    float Float(const NbtTagPtr& parent, const string& name, float fallback) {
        auto t = Child(parent, name);
        return t && t->type == TagType::FLOAT ? bytesToFloat(t->payload) : fallback;
    }

    // BakedModel.isGui3d: the resolved item model's gui_light is not "front".
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

    // BeltHelper.isItemUpright: create:upright_on_belt tag or fluid-handler items (approximated).
    bool IsUpright(const string& id) {
        static const unordered_set<string> upright = {
            "create:blaze_cake_base", "create:blaze_cake", "create:creative_blaze_cake",
            "create:builders_tea", "minecraft:glass_bottle", "minecraft:potion",
            "minecraft:splash_potion", "minecraft:lingering_potion", "minecraft:honey_bottle",
            "minecraft:cake", "minecraft:bowl", "minecraft:mushroom_stew", "minecraft:rabbit_stew",
            "minecraft:beetroot_soup", "minecraft:suspicious_stew" };
        if (upright.count(id)) return true;
        return id.size() > 7 && id.compare(id.size() - 7, 7, "_bucket") == 0;
    }

    bool IsPackage(const string& id) {
        return id.rfind("create:cardboard_package", 0) == 0 || id.rfind("create:rare_", 0) == 0;
    }

    void Emit(ModelData& model, ModelData mesh, const EntityAffine& pose) {
        ItemRender::Transform(mesh, pose);
        for (auto& face : mesh.faces) face.faceDirection = FaceType::DO_NOT_CULL;
        if (model.vertices.empty()) model = std::move(mesh);
        else MergeModelsDirectly(model, mesh);
    }

    // DepotRenderer.renderItem with alwaysUpright = false and no camera (upright yaw fixed to 0).
    void RenderItem(ModelData& model, const NbtTagPtr& stackTag, int angle, JavaRandom& r,
        EntityAffine ms) {
        ItemStackInfo item = ItemRender::ParseItemStack(stackTag);
        if (item.empty()) return;
        ModelData mesh;
        if (!ItemRender::Build(item, ItemDisplay::Fixed, false, mesh)) return;
        int count = 0; // Mth.log2(count) / 2 (floor log2)
        for (int c = item.count; c > 1; c >>= 1) ++count;
        count /= 2;
        const bool blockItem = IsGui3d(item.id);
        const bool upright = IsUpright(item.id);
        const bool package = IsPackage(item.id);
        ms = ms * EntityAffine::RYDeg(angle);
        if (upright) ms = ms * EntityAffine::T(0.0, 0.09375, -0.0625);
        for (int i = 0; i <= count; ++i) {
            EntityAffine p = ms;
            if (blockItem) {
                float ox = r.NextFloat() * 0.0625f * i;
                float oz = r.NextFloat() * 0.0625f * i;
                p = p * EntityAffine::T(ox, 0.0, oz);
            }
            if (package) p = p * EntityAffine::T(0.0, 0.25, 0.0) * EntityAffine::S(1.5);
            else p = p * EntityAffine::S(0.5);
            if (!blockItem && !upright)
                p = p * EntityAffine::T(0.0, -0.1875, 0.0) * EntityAffine::RX(M_PI / 2.0);
            Emit(model, mesh, p);
            if (!upright) {
                if (!blockItem) ms = ms * EntityAffine::RYDeg(10.0);
                ms = ms * EntityAffine::T(0.0, blockItem ? 0.015625 : 0.0625, 0.0);
            } else {
                ms = ms * EntityAffine::T(0.0, 0.0, -0.0625);
            }
        }
    }

    // Direction.from3DDataValue: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
    const int kDirs[6][3] = { {0,-1,0}, {0,1,0}, {0,0,-1}, {0,0,1}, {-1,0,0}, {1,0,0} };

    void RenderItemsOf(ModelData& model, const NbtTagPtr& nbt, const EntityAffine& base) {
        EntityAffine root = base * EntityAffine::T(0.5, 0.9375, 0.5);
        auto renderTransported = [&](const NbtTagPtr& tis) {
            auto stack = Child(tis, "Item");
            if (!stack) return;
            EntityAffine ms = root;
            int dir = Int(tis, "InDirection", 0);
            if (dir >= 2 && dir <= 5) {
                float offset = Float(tis, "Pos", 0.5f);
                float side = Float(tis, "Offset", 0.0f);
                const int* from = kDirs[dir];
                float k = 0.5f - offset; // insertedFrom.getOpposite() * (0.5 - offset)
                ms = ms * EntityAffine::T(-from[0] * k, 0.0, -from[2] * k);
                // insertedFrom.getClockWise().getAxis() == X  <=>  insertedFrom on Z axis
                bool alongX = from[2] != 0;
                if (!alongX) side = -side;
                ms = ms * EntityAffine::T(alongX ? side : 0.0, 0.0, alongX ? 0.0 : side);
            }
            JavaRandom r(0);
            RenderItem(model, stack, Int(tis, "Angle", 0), r, ms);
        };
        if (auto held = Child(nbt, "HeldItem")) renderTransported(held);
        if (auto incoming = Child(nbt, "Incoming"); incoming && incoming->type == TagType::LIST)
            for (const auto& tis : incoming->children) renderTransported(tis);

        auto items = Child(Child(nbt, "OutputBuffer"), "Items");
        if (!items || items->type != TagType::LIST) return;
        for (const auto& stack : items->children) {
            int slot = Int(stack, "Slot", -1);
            if (slot < 0) continue;
            ItemStackInfo info = ItemRender::ParseItemStack(stack);
            if (info.empty()) continue;
            bool upright = IsUpright(info.id);
            EntityAffine ms = root * EntityAffine::RYDeg(45.0 * slot) * EntityAffine::T(0.35, 0.0, 0.0);
            if (upright) ms = ms * EntityAffine::RYDeg(-45.0 * slot);
            JavaRandom r(slot + 1);
            int angle = static_cast<int>(360.0f * r.NextFloat());
            RenderItem(model, stack, upright ? angle + 90 : angle, r, ms);
        }
    }

    // BeltRenderer.renderItem (static: speed 0 -> Pos/Offset as stored; all stack copies drawn,
    // upright items without the camera-facing yaw).
    void RenderBeltItem(ModelData& model, const NbtTagPtr& tis, const int dir[3], const string& facing,
        const string& slope, int length) {
        auto stack = Child(tis, "Item");
        ItemStackInfo item = ItemRender::ParseItemStack(stack);
        if (item.empty()) return;
        ModelData mesh;
        if (!ItemRender::Build(item, ItemDisplay::Fixed, false, mesh)) return;
        const int verticality = slope == "downward" ? -1 : slope == "upward" ? 1 : 0;
        const bool slopeAlongX = dir[0] != 0;
        const bool positive = dir[0] + dir[2] > 0;
        float offset = Float(tis, "Pos", 0.0f);
        float side = Float(tis, "Offset", 0.0f);
        const int angle = Int(tis, "Angle", 0);
        float vertical = offset < 0.5f ? 0.0f : verticality * (min(offset, length - 0.5f) - 0.5f);
        const bool onSlope = slope != "horizontal" && offset >= 0.5f && offset <= length - 0.5f;
        const bool tiltForward = ((slope == "downward") ^ positive) == (dir[2] != 0);
        const float slopeAngle = onSlope ? (tiltForward ? -45.0f : 45.0f) : 0.0f;

        EntityAffine ms = EntityAffine::T(0.5 - 0.5 * dir[0], 0.9375, 0.5 - 0.5 * dir[2]);
        ms = ms * EntityAffine::T(dir[0] * offset, vertical, dir[2] * offset);
        const bool alongX = dir[2] != 0; // facing.getClockWise().getAxis() == X
        if (!alongX) side = -side;
        ms = ms * EntityAffine::T(alongX ? side : 0.0, 0.0, alongX ? 0.0 : side);

        int count = 0;
        for (int c = item.count; c > 1; c >>= 1) ++count;
        count /= 2;
        const bool upright = IsUpright(item.id);
        const bool blockItem = IsGui3d(item.id);
        const bool box = IsPackage(item.id);
        JavaRandom r(angle);
        if (!upright && slopeAngle != 0.0f)
            ms = ms * (slopeAlongX ? EntityAffine::RZDeg(slopeAngle) : EntityAffine::RX(slopeAngle * M_PI / 180.0));
        if (onSlope) ms = ms * EntityAffine::T(0.0, 0.125, 0.0);
        if (upright) ms = ms * EntityAffine::T(0.0, 0.09375, 0.0625);
        for (int i = 0; i <= count; ++i) {
            EntityAffine p = ms * EntityAffine::RYDeg(angle);
            if (!blockItem && !upright) p = p * EntityAffine::T(0.0, -0.09375, 0.0) * EntityAffine::RX(M_PI / 2.0);
            if (blockItem && !box) {
                float ox = r.NextFloat() * 0.0625f * i;
                float oz = r.NextFloat() * 0.0625f * i;
                p = p * EntityAffine::T(ox, 0.0, oz);
            }
            p = box ? p * EntityAffine::T(0.0, 0.25, 0.0) * EntityAffine::S(1.5) : p * EntityAffine::S(0.5);
            Emit(model, mesh, p);
            if (!upright) {
                if (!blockItem) ms = ms * EntityAffine::RYDeg(10.0);
                ms = ms * EntityAffine::T(0.0, blockItem ? 0.015625 : 0.0625, 0.0);
            } else {
                ms = ms * EntityAffine::T(0.0, 0.0, -0.0625);
            }
        }
    }

    string StateValue(const string& blockName, const string& key) {
        size_t p = blockName.find(key + "=");
        if (p == string::npos) return "";
        p += key.size() + 1;
        size_t e = blockName.find_first_of(",]", p);
        return blockName.substr(p, e == string::npos ? string::npos : e - p);
    }

    // AngleHelper.horizontalAngle: toYRot, then negated when the axis is X.
    // south 0, west -90, north 180, east -270. EntityAffine::RY matches
    // Axis.YP.rotation (CCW looking down), so the angle is used as-is.
    double HorizontalAngle(const string& facing) {
        if (facing == "west") return -90.0;
        if (facing == "north") return 180.0;
        if (facing == "east") return -270.0;
        return 0.0; // south, or vertical
    }

    // PlacardRenderer.renderSafe. Plate sits on the attached face (opposite
    // facing); the item is translated +Z in local space, just in front of it.
    void RenderPlacard(ModelData& model, const string& blockName, const NbtTagPtr& nbt) {
        ItemStackInfo item = ItemRender::ParseItemStack(Child(nbt, "Item"));
        if (item.empty()) return;
        ModelData mesh;
        if (!ItemRender::Build(item, ItemDisplay::Fixed, false, mesh)) return;
        const string face = StateValue(blockName, "face");
        const bool ceiling = face == "ceiling";
        const bool floor = face == "floor";
        const double yaw = (ceiling ? M_PI : 0.0) + (180.0 + HorizontalAngle(StateValue(blockName, "facing"))) * M_PI / 180.0;
        const double pitch = ceiling ? -M_PI / 2.0 : floor ? M_PI / 2.0 : 0.0;
        const double scale = IsGui3d(item.id) ? 0.5 : 0.375;
        // center, Ry(yaw), Rx(pitch), T(0,0,4.5/16), scale. Fixed display is already in the mesh.
        const EntityAffine pose = EntityAffine::T(0.5, 0.5, 0.5) * EntityAffine::RY(yaw) * EntityAffine::RX(pitch) *
            EntityAffine::T(0.0, 0.0, 0.28125) * EntityAffine::S(scale);
        Emit(model, std::move(mesh), pose);
    }

    // Vec3i.hashCode / BlockPos.hashCode: (y + z * 31) * 31 + x, 32-bit wrap.
    int32_t BlockPosHash(int x, int y, int z) {
        uint32_t h = (static_cast<uint32_t>(y) + static_cast<uint32_t>(z) * 31u) * 31u + static_cast<uint32_t>(x);
        return static_cast<int32_t>(h);
    }

    // ItemStackHandler slots, indexed by Slot (missing slots stay null).
    void FillSlots(const NbtTagPtr& inv, vector<NbtTagPtr>& slots) {
        auto items = Child(inv, "Items");
        if (!items || items->type != TagType::LIST) return;
        for (const auto& stack : items->children) {
            int slot = Int(stack, "Slot", -1);
            if (slot >= 0 && slot < static_cast<int>(slots.size())) slots[slot] = stack;
        }
    }

    // VecHelper.rotate(vec, deg, Axis.Y): x' = x*cos + z*sin, z' = z*cos - x*sin.
    void RotateYDeg(double deg, double& x, double& z) {
        const double rad = deg * M_PI / 180.0;
        const double c = std::cos(rad), s = std::sin(rad);
        const double nx = x * c + z * s;
        z = z * c - x * s;
        x = nx;
    }

    // BasinRenderer.renderSafe, items only (fluids, visualized outputs and the filter are skipped).
    void RenderBasin(ModelData& model, const NbtTagPtr& nbt) {
        // Input slots 0..8, then output slots 0..8 (CombinedStorage order).
        vector<NbtTagPtr> in(9), out(9), slots(18);
        FillSlots(Child(nbt, "InputItems"), in);
        FillSlots(Child(nbt, "OutputItems"), out);
        for (int i = 0; i < 9; ++i) { slots[i] = in[i]; slots[9 + i] = out[i]; }
        vector<NbtTagPtr> stacks;
        for (const auto& s : slots) {
            if (!ItemRender::ParseItemStack(s).empty()) stacks.push_back(s);
        }
        if (stacks.empty()) return;
        // fluidLevel 0 -> clamp(0 - 0.3, 0.125, 0.6) = 0.125. ingredientRotation is 0 when static.
        const float level = 0.125f;
        const int total = static_cast<int>(stacks.size());
        const float anglePartition = 360.0f / static_cast<float>(total);
        double baseX = total == 1 ? 0.0 : 0.125, baseZ = 0.0;
        const double baseY = level;
        // LegacyRandomSource(pos.hashCode()), seed sign-extended.
        JavaRandom r(static_cast<int64_t>(BlockPosHash(Int(nbt, "x"), Int(nbt, "y"), Int(nbt, "z"))));
        int itemCount = total;
        for (const auto& stack : stacks) {
            ItemStackInfo item = ItemRender::ParseItemStack(stack);
            ModelData mesh;
            const bool ok = ItemRender::Build(item, ItemDisplay::Ground, false, mesh);
            const float ang = anglePartition * static_cast<float>(itemCount);
            double px = baseX, pz = baseZ;
            RotateYDeg(ang, px, pz);
            const EntityAffine ms = EntityAffine::T(0.5, 0.2, 0.5) * EntityAffine::T(px, baseY, pz) *
                EntityAffine::RYDeg(ang + 35.0) * EntityAffine::RX(65.0 * M_PI / 180.0);
            const int copies = item.count / 8; // i = 0 .. count/8 inclusive
            for (int i = 0; i <= copies; ++i) {
                // offsetRandomly(ZERO, r, 1/16): (nextFloat-0.5)*2*(1/16) on x, then y, then z.
                const float j = 2.0f / 16.0f;
                const double ox = (r.NextFloat() - 0.5f) * j;
                const double oy = (r.NextFloat() - 0.5f) * j;
                const double oz = (r.NextFloat() - 0.5f) * j;
                if (ok) Emit(model, mesh, ms * EntityAffine::T(ox, oy, oz));
            }
            --itemCount;
        }
    }
}

void AppendCreateDepotItems(const string& blockName, const NbtTagPtr& nbt, ModelData& model) {
    if (!nbt) return;
    string base = blockName.substr(0, blockName.find('['));
    if (base == "create:placard") {
        RenderPlacard(model, blockName, nbt);
        return;
    }
    if (base == "create:basin") {
        RenderBasin(model, nbt);
        return;
    }
    if (base == "create:belt") {
        // BeltRenderer.renderItems: only the controller draws, items of the whole belt.
        auto isController = Child(nbt, "IsController");
        if (!isController || isController->type != TagType::BYTE || bytesToByte(isController->payload) == 0) return;
        const int length = Int(nbt, "Length", 0);
        auto items = Child(Child(nbt, "Inventory"), "Items");
        if (length <= 0 || !items || items->type != TagType::LIST || items->children.empty()) return;
        const string facing = StateValue(blockName, "facing");
        const string slope = StateValue(blockName, "slope");
        if (slope == "vertical" || slope == "sideways") return; // these belts carry no items
        int dir[3] = {0, 0, 0};
        if (facing == "east") dir[0] = 1; else if (facing == "west") dir[0] = -1;
        else if (facing == "south") dir[2] = 1; else if (facing == "north") dir[2] = -1; else return;
        for (const auto& tis : items->children) RenderBeltItem(model, tis, dir, facing, slope, length);
        return;
    }
    if (base == "create:depot") {
        RenderItemsOf(model, nbt, EntityAffine());
    } else if (base == "create:weighted_ejector") {
        // EjectorRenderer: applyLidAngle(Lid.Value * 70), then center, Ry(-180 - hA), uncenter.
        string f = StateValue(blockName, "facing");
        double hA = f == "west" ? -90.0 : f == "north" ? 180.0 : f == "east" ? -270.0 : 0.0;
        double angle = Float(Child(nbt, "Lid"), "Value", 0.0f) * 70.0;
        const EntityAffine c = EntityAffine::T(0.5, 0.5, 0.5), uc = EntityAffine::T(-0.5, -0.5, -0.5);
        const double py = 11.25 / 16.0, pz = 0.75 / 16.0;
        const EntityAffine lid = c * EntityAffine::RYDeg(180.0 + hA) * uc * EntityAffine::T(0.0, py, pz) *
            EntityAffine::RX(-angle * M_PI / 180.0) * EntityAffine::T(0.0, -py, -pz);
        // EJECTOR_TOP partial at the lid angle, and the KineticBlockEntityRenderer shaft on
        // facing.getClockWise().getAxis() (rest angle 0).
        ModelData top = ProcessModelJson("create", "block/weighted_ejector/top", 0, 0, false, 304);
        if (!top.vertices.empty()) Emit(model, std::move(top), lid);
        ModelData shaft = ProcessModelJson("create", "block/shaft", 0, 0, false, 305);
        if (!shaft.vertices.empty()) {
            const bool alongX = f == "north" || f == "south";
            Emit(model, std::move(shaft), alongX ?
                c * EntityAffine::RZDeg(90.0) * uc : c * EntityAffine::RX(M_PI / 2.0) * uc);
        }
        const EntityAffine m = lid * c * EntityAffine::RYDeg(-180.0 - hA) * uc;
        RenderItemsOf(model, nbt, m);
    }
}
