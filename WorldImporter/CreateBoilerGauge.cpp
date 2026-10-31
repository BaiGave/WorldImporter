#include "CreateBoilerGauge.h"
#include "block.h"
#include <cmath>

using namespace std;

namespace {
    int BoilerInt(const NbtTagPtr& parent, const string& name) {
        auto tag = parent ? getChildByName(parent, name) : nullptr;
        return tag && tag->type == TagType::INT ? bytesToInt(tag->payload) : 0;
    }

    void Translate(ModelData& m, float dx, float dy, float dz) {
        for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            m.vertices[i] += dx; m.vertices[i + 1] += dy; m.vertices[i + 2] += dz;
        }
    }

    // Right-handed rotation about the origin (PoseStack rotateX/YDegrees).
    void RotateX(ModelData& m, float degrees) {
        float c = cosf(degrees * (float)M_PI / 180.0f), s = sinf(degrees * (float)M_PI / 180.0f);
        for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            float y = m.vertices[i + 1], z = m.vertices[i + 2];
            m.vertices[i + 1] = y * c - z * s; m.vertices[i + 2] = y * s + z * c;
        }
    }
    void RotateY(ModelData& m, float degrees) {
        float c = cosf(degrees * (float)M_PI / 180.0f), s = sinf(degrees * (float)M_PI / 180.0f);
        for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            float x = m.vertices[i], z = m.vertices[i + 2];
            m.vertices[i] = x * c + z * s; m.vertices[i + 2] = -x * s + z * c;
        }
    }

    // CollisionGetter.noCollision approximation: anything but air / fluids / plants collides.
    bool Collides(int x, int y, int z) {
        Block b = GetBlockById(GetBlockId(x, y, z));
        if (b.air) return false;
        string id = b.name.substr(0, b.name.find('['));
        static const char* passable[] = {"minecraft:water", "minecraft:lava", "minecraft:light",
            "minecraft:grass", "minecraft:short_grass", "minecraft:tall_grass", "minecraft:fern",
            "minecraft:large_fern", "minecraft:dead_bush", "minecraft:snow", "minecraft:torch",
            "minecraft:wall_torch", "minecraft:redstone_wire"};
        for (const char* p : passable) if (id == p) return false;
        return true;
    }
}

void AppendCreateBoilerGauge(const string& blockName, int x, int y, int z,
    const NbtTagPtr& nbt, ModelData& model) {
    if (!nbt) return;
    string base = blockName.substr(0, blockName.find('['));
    if (base != "create:fluid_tank" && base != "create:creative_fluid_tank") return;
    if (getChildByName(nbt, "Controller")) return;
    auto window = getChildByName(nbt, "Window");
    if (window && window->type == TagType::BYTE && bytesToByte(window->payload) != 0) return;
    auto boiler = getChildByName(nbt, "Boiler");
    if (BoilerInt(boiler, "Engines") + BoilerInt(boiler, "Whistles") <= 0) return; // isActive
    auto sizeTag = getChildByName(nbt, "Size");
    const int w = sizeTag && sizeTag->type == TagType::INT ? max(1, bytesToInt(sizeTag->payload)) : 1;
    const float half = w / 2.0f;

    ModelData gaugeModel = ProcessModelJson("create", "block/steam_engine/gauge", 0, 0, false, 280);
    ModelData dialModel = ProcessModelJson("create", "block/steam_engine/gauge_dial", 0, 0, false, 281);
    // Iterate.horizontalDirections: south, west, north, east; toYRot 0/90/180/270.
    static const int dirs[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    for (int i = 0; i < 4; ++i) {
        const int dx = dirs[i][0], dz = dirs[i][1];
        // BoilerData.updateOcclusion: 0.25-wide box centred on the face, 0.125..0.375 out,
        // y 0.125..0.875 of the bottom layer, half a block wider than the face centre each way.
        float bx0 = half - 0.125f + dx * (half + 0.25f), bx1 = half + 0.125f + dx * (half + 0.25f);
        float bz0 = half - 0.125f + dz * (half + 0.25f), bz1 = half + 0.125f + dz * (half + 0.25f);
        if (dz != 0) { bx0 -= 0.5f; bx1 += 0.5f; } else { bz0 -= 0.5f; bz1 += 0.5f; }
        bool occluded = false;
        for (int cx = (int)floorf(bx0); cx <= (int)floorf(bx1 - 1e-4f) && !occluded; ++cx)
            for (int cz = (int)floorf(bz0); cz <= (int)floorf(bz1 - 1e-4f) && !occluded; ++cz)
                occluded = Collides(x + cx, y, z + cz);
        if (occluded) continue;
        const float yRot = -90.0f * i - 90.0f;
        for (int part = 0; part < 2; ++part) {
            ModelData piece = part == 0 ? gaugeModel : dialModel;
            if (piece.vertices.empty()) continue;
            if (part == 1) { // dial at progress 0: rotateX(-145*0 + 90) about (0, .375, .5)
                Translate(piece, 0.0f, -0.375f, -0.5f);
                RotateX(piece, 90.0f);
                Translate(piece, 0.0f, 0.375f, 0.5f);
            }
            Translate(piece, half - 0.375f - 0.5f, -0.5f, -0.5f);
            RotateY(piece, yRot);
            Translate(piece, half, 0.5f, half);
            for (auto& face : piece.faces) face.faceDirection = FaceType::DO_NOT_CULL;
            if (model.vertices.empty()) model = std::move(piece);
            else MergeModelsDirectly(model, piece);
        }
    }
}
