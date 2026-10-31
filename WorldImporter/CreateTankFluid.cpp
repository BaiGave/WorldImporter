#include "CreateTankFluid.h"
#include "fluid.h"
#include "texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace std;

namespace {
    string TankNbtString(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
    }

    bool ReadNumber(const NbtTagPtr& parent, const string& name, double& out) {
        auto tag = getChildByName(parent, name);
        if (!tag || tag->payload.empty()) return false;
        switch (tag->type) {
        case TagType::BYTE: out = bytesToByte(tag->payload); return true;
        case TagType::SHORT:
            if (tag->payload.size() < 2) return false;
            out = bytesToShort(tag->payload); return true;
        case TagType::INT:
            if (tag->payload.size() < 4) return false;
            out = bytesToInt(tag->payload); return true;
        case TagType::LONG:
            if (tag->payload.size() < 8) return false;
            out = static_cast<double>(bytesToLong(tag->payload)); return true;
        case TagType::FLOAT:
            if (tag->payload.size() < 4) return false;
            out = bytesToFloat(tag->payload); return true;
        case TagType::DOUBLE:
            if (tag->payload.size() < 8) return false;
            out = bytesToDouble(tag->payload); return true;
        default: return false;
        }
    }

    // One SmartFluidTankBehaviour.TankSegment: TankContent + lerped Level.
    struct TankSlot {
        string fluid;
        double amount = 0;
        float level = 0;
        bool hasLevel = false;
        bool fabric = false;
    };

    TankSlot ReadTankSlot(const NbtTagPtr& seg) {
        TankSlot slot;
        if (!seg) return slot;
        auto content = getChildByName(seg, "TankContent");
        if (!content) return slot;
        // Forge: {FluidName, Amount}. Fabric: {Variant:{fluid}, Amount, Capacity} in droplets.
        if (auto variant = getChildByName(content, "Variant")) {
            slot.fluid = TankNbtString(variant, "fluid");
            slot.fabric = true;
        } else {
            slot.fluid = TankNbtString(content, "FluidName");
        }
        ReadNumber(content, "Amount", slot.amount);
        if (auto level = getChildByName(seg, "Level")) {
            double value = 0;
            if (ReadNumber(level, "Value", value)) {
                slot.level = static_cast<float>(value);
                slot.hasLevel = true;
            }
        }
        return slot;
    }

    bool TankEmpty(const TankSlot& slot) {
        return slot.fluid.empty() || slot.fluid == "minecraft:empty" || slot.amount <= 0.0;
    }

    template <typename Fn>
    void ForEachCompound(const NbtTagPtr& list, Fn&& fn) {
        if (!list || list->type != TagType::LIST) return;
        for (const auto& child : list->children)
            if (child && child->type == TagType::COMPOUND) fn(child);
    }

    float Clamp01(float v) {
        if (v < 0.0f) return 0.0f;
        if (v > 1.0f) return 1.0f;
        return v;
    }

    // renderFluidBox: still texture tiled per block, no bottom face.
    // invertGravity only swaps which face is the "top" for flow UVs; a static
    // box keeps the still texture on the top and omits the bottom either way.
    void AppendFluidBox(ModelData& model, const string& fluid,
        float x0, float y0, float z0, float x1, float y1, float z1) {
        if (!(x1 > x0 && y1 > y0 && z1 > z0)) return;
        // Still texture (fluids.json folder/suffix, else block/<id>_still).
        string ns = "minecraft", id = fluid;
        size_t colon = fluid.find(':');
        if (colon != string::npos) { ns = fluid.substr(0, colon); id = fluid.substr(colon + 1); }
        string still = "block/" + id + "_still";
        auto def = fluidDefinitions.find(ns + ":" + id);
        if (def != fluidDefinitions.end()) still = def->second.folder + "/" + id + def->second.still_texture;
        string root = "textures";
        if (!SaveTextureToFile(ns, still, root)) {
            still = "fluid/" + id + "_still";
            if (!SaveTextureToFile(ns, still, root)) return;
        }
        string savedPath = "textures/" + ns + "/" + still + ".png";
        RegisterTexture(ns, still, savedPath);
        // Same name as the world-fluid still material (fluid.cpp) so both share one material.
        Material material(ns + ":" + id + "#still~" + still.substr(still.find_last_of('/') + 1), savedPath, -1);
        float aspect = 1.0f;
        material.type = DetectMaterialType(ns, still, aspect);
        if (aspect < 1.0f) aspect = 1.0f;
        material.aspectRatio = aspect;
        if (id.find("water") != string::npos) material.tintIndex = 2;
        const float maxV = 1.0f / aspect, startV = 1.0f - maxV;
        const int materialIndex = static_cast<int>(model.materials.size());
        model.materials.push_back(material);

        // One quad per 1x1 tile; corners given in (u, v) of the face plane, wound CCW from outside.
        auto quad = [&](const array<array<float, 3>, 4>& p, const array<array<float, 2>, 4>& uv) {
            Face face{};
            face.materialIndex = materialIndex;
            face.faceDirection = FaceType::DO_NOT_CULL;
            for (int i = 0; i < 4; ++i) {
                face.vertexIndices[i] = static_cast<int>(model.vertices.size() / 3);
                model.vertices.insert(model.vertices.end(), p[i].begin(), p[i].end());
                face.uvIndices[i] = static_cast<int>(model.uvCoordinates.size() / 2);
                model.uvCoordinates.push_back(uv[i][0]);
                model.uvCoordinates.push_back(startV + (1.0f - uv[i][1]) * maxV);
            }
            model.faces.push_back(face);
        };
        auto tiles = [](float a, float b) {
            vector<pair<float, float>> out;
            for (float s = a; s < b - 1e-4f;) {
                float e = min(b, floorf(s) + 1.0f);
                out.emplace_back(s, e);
                s = e;
            }
            return out;
        };
        auto fr = [](float v, float cell) { return v - cell; };
        // Top (y1): u = x, v = z.
        for (auto [xa, xb] : tiles(x0, x1)) for (auto [za, zb] : tiles(z0, z1)) {
            float cx = floorf(xa), cz = floorf(za);
            quad({{{xa, y1, za}, {xa, y1, zb}, {xb, y1, zb}, {xb, y1, za}}},
                {{{fr(xa, cx), fr(za, cz)}, {fr(xa, cx), fr(zb, cz)}, {fr(xb, cx), fr(zb, cz)}, {fr(xb, cx), fr(za, cz)}}});
        }
        // Sides: v measured downward from the tile top (texture v=0 at top).
        for (auto [ya, yb] : tiles(y0, y1)) {
            float cy = floorf(ya);
            float va = 1.0f - fr(ya, cy), vb = 1.0f - fr(yb, cy);
            for (auto [xa, xb] : tiles(x0, x1)) {
                float cx = floorf(xa);
                // north (z0, normal -z) and south (z1, normal +z)
                quad({{{xb, ya, z0}, {xa, ya, z0}, {xa, yb, z0}, {xb, yb, z0}}},
                    {{{1.0f - fr(xb, cx), va}, {1.0f - fr(xa, cx), va}, {1.0f - fr(xa, cx), vb}, {1.0f - fr(xb, cx), vb}}});
                quad({{{xa, ya, z1}, {xb, ya, z1}, {xb, yb, z1}, {xa, yb, z1}}},
                    {{{fr(xa, cx), va}, {fr(xb, cx), va}, {fr(xb, cx), vb}, {fr(xa, cx), vb}}});
            }
            for (auto [za, zb] : tiles(z0, z1)) {
                float cz = floorf(za);
                // west (x0, normal -x) and east (x1, normal +x)
                quad({{{x0, ya, za}, {x0, ya, zb}, {x0, yb, zb}, {x0, yb, za}}},
                    {{{fr(za, cz), va}, {fr(zb, cz), va}, {fr(zb, cz), vb}, {fr(za, cz), vb}}});
                quad({{{x1, ya, zb}, {x1, ya, za}, {x1, yb, za}, {x1, yb, zb}}},
                    {{{1.0f - fr(zb, cz), va}, {1.0f - fr(za, cz), va}, {1.0f - fr(za, cz), vb}, {1.0f - fr(zb, cz), vb}}});
            }
        }
    }

    // FluidTankRenderer.renderSafe: controller (min corner) only, window on. Box from
    // (hull, cap+puddle, hull) to (width-hull, cap+puddle+level*(height-2*cap-puddle), width-hull).
    void AppendTank(const string& base, const NbtTagPtr& nbt, ModelData& model) {
        if (getChildByName(nbt, "Controller")) return; // not the controller
        auto window = getChildByName(nbt, "Window");
        if (!window || window->type != TagType::BYTE || bytesToByte(window->payload) == 0) return;
        auto intTag = [&](const NbtTagPtr& parent, const string& name, int fallback) {
            auto tag = getChildByName(parent, name);
            return tag && tag->type == TagType::INT ? bytesToInt(tag->payload) : fallback;
        };
        auto content = getChildByName(nbt, "TankContent");
        if (!content) return;
        const int width = max(1, intTag(nbt, "Size", 1));
        const int height = max(1, intTag(nbt, "Height", 1));
        // Forge: {FluidName, Amount(mB)}, capacity = fluidTankCapacity (default 8 buckets) per block.
        // Fabric: {Variant:{fluid}, Amount, Capacity} as longs in droplets.
        string fluid = TankNbtString(content, "FluidName");
        double amount = intTag(content, "Amount", 0);
        double capacity = 8000.0 * width * width * height;
        if (auto variant = getChildByName(content, "Variant")) {
            fluid = TankNbtString(variant, "fluid");
            auto longTag = [&](const string& name) {
                auto tag = getChildByName(content, name);
                return tag && tag->type == TagType::LONG ? static_cast<double>(bytesToLong(tag->payload)) : 0.0;
            };
            amount = longTag("Amount");
            capacity = longTag("Capacity");
        }
        if (fluid.empty() || fluid == "minecraft:empty" || amount <= 0 || capacity <= 0) return;
        float level = base == "create:creative_fluid_tank" ? 1.0f :
            static_cast<float>(min(1.0, amount / capacity));
        const float cap = 0.25f, hull = 0.0703125f, puddle = 0.0625f;
        const float totalHeight = height - 2.0f * cap - puddle;
        if (totalHeight <= 0.0f || level < 1.0f / (512.0f * totalHeight)) return;
        const float x0 = hull, x1 = width - hull, z0 = hull, z1 = width - hull;
        const float y0 = cap + puddle, y1 = y0 + min(level * totalHeight, totalHeight);
        AppendFluidBox(model, fluid, x0, y0, z0, x1, y1, z1);
    }

    // BasinRenderer.renderFluids. getTotalUnits is Amount (static, not the lerped level).
    // Forge: each tank holds one bucket (1000 mB); the column is two buckets tall.
    // Fabric droplets use 81000 per bucket, so the divisor is 162000.
    void AppendBasin(const NbtTagPtr& nbt, ModelData& model) {
        struct Seg { string fluid; double units; };
        vector<Seg> segs;
        double total = 0;
        bool fabric = false;
        auto consume = [&](const NbtTagPtr& list) {
            ForEachCompound(list, [&](const NbtTagPtr& seg) {
                TankSlot slot = ReadTankSlot(seg);
                if (TankEmpty(slot) || slot.amount < 1.0) return;
                if (slot.fabric) fabric = true;
                segs.push_back({slot.fluid, slot.amount});
                total += slot.amount;
            });
        };
        consume(getChildByName(nbt, "InputTanks"));
        consume(getChildByName(nbt, "OutputTanks"));
        if (total < 1.0 || segs.empty()) return;
        const float divisor = fabric ? 162000.0f : 2000.0f;
        float fluidLevel = Clamp01(static_cast<float>(total) / divisor);
        fluidLevel = 1.0f - (1.0f - fluidLevel) * (1.0f - fluidLevel);
        const float yMax = 0.125f + 0.75f * fluidLevel;
        float xMin = 0.125f;
        for (const auto& seg : segs) {
            float partial = Clamp01(static_cast<float>(seg.units) / static_cast<float>(total));
            float xMax = xMin + partial * (12.0f / 16.0f);
            AppendFluidBox(model, seg.fluid, xMin, 0.125f, 0.125f, xMax, yMax, 0.875f);
            xMin = xMax;
        }
    }

    // SpoutRenderer.renderSafe fluid column. Level.Value is the 0..1 fill (static).
    // Lighter-than-air is treated as false, so the column sits on the lower lip:
    // translate (0, yOffset, 0) then box y from (min - yOffset) to min.
    void AppendSpout(const NbtTagPtr& nbt, ModelData& model) {
        auto tanks = getChildByName(nbt, "Tanks");
        if (!tanks || tanks->type != TagType::LIST || tanks->children.empty()) return;
        TankSlot slot = ReadTankSlot(tanks->children[0]);
        if (TankEmpty(slot) || !slot.hasLevel || slot.level == 0.0f) return;
        float level = max(slot.level, 0.175f);
        const float minV = 2.5f / 16.0f;          // 0.15625
        const float maxV = minV + (11.0f / 16.0f); // 0.84375
        const float yOffset = (11.0f / 16.0f) * level;
        AppendFluidBox(model, slot.fluid, minV, minV, minV, maxV, minV + yOffset, maxV);
    }

    // ItemDrainRenderer.renderFluid: box (2/16, yMin - yOffset, 2/16)..(14/16, yMin, 14/16)
    // translated by yOffset = 7/16 * level, yMin = 5/16. Level.Value is the 0..1 fill.
    void AppendItemDrain(const NbtTagPtr& nbt, ModelData& model) {
        auto tanks = getChildByName(nbt, "Tanks");
        if (!tanks || tanks->type != TagType::LIST || tanks->children.empty()) return;
        TankSlot slot = ReadTankSlot(tanks->children[0]);
        if (TankEmpty(slot) || !slot.hasLevel || slot.level == 0.0f) return;
        const float yMin = 0.3125f, minV = 0.125f, maxV = minV + 0.75f;
        const float yOffset = 0.4375f * slot.level;
        AppendFluidBox(model, slot.fluid, minV, yMin, minV, maxV, yMin + yOffset, maxV);
    }
}

void AppendCreateTankFluid(const string& blockName, const NbtTagPtr& nbt,
    ModelData& model) {
    if (!nbt) return;
    string base = blockName.substr(0, blockName.find('['));
    if (base == "create:fluid_tank" || base == "create:creative_fluid_tank")
        AppendTank(base, nbt, model);
    else if (base == "create:basin")
        AppendBasin(nbt, model);
    else if (base == "create:spout")
        AppendSpout(nbt, model);
    else if (base == "create:item_drain")
        AppendItemDrain(nbt, model);
}
