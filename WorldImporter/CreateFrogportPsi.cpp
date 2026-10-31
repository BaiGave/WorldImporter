#include "CreateFrogportPsi.h"
#include <cmath>
#include <span>

using namespace std;

namespace {
    const NbtTagPtr Child(const NbtTagPtr& parent, const string& name) {
        return parent && parent->type == TagType::COMPOUND ? getChildByName(parent, name) : nullptr;
    }

    void Translate(ModelData& m, float dx, float dy, float dz) {
        for (size_t i = 0; i + 2 < m.vertices.size(); i += 3) {
            m.vertices[i] += dx; m.vertices[i + 1] += dy; m.vertices[i + 2] += dz;
        }
    }

    // Right-handed, about the block centre: X first, then Y (SuperByteBuffer center/rotate).
    void Rotate(ModelData& m, float rx, float ry) {
        if (!m.vertices.empty())
            ApplyRotationToVertices(span<float>(m.vertices.data(), m.vertices.size()), rx, ry, 0.0f);
    }

    void AppendPart(ModelData& model, ModelData piece) {
        if (piece.vertices.empty()) return;
        for (auto& face : piece.faces) face.faceDirection = FaceType::DO_NOT_CULL;
        if (model.vertices.empty()) model = std::move(piece);
        else MergeModelsDirectly(model, piece);
    }

    string StateValue(const string& blockName, const string& key) {
        size_t p = blockName.find(key + "=");
        if (p == string::npos) return "";
        p += key.size() + 1;
        size_t e = blockName.find_first_of(",]", p);
        return blockName.substr(p, e == string::npos ? string::npos : e - p);
    }

    // FrogportBlockEntity.getYaw: PlacedYaw, or facing the target (atan2(dx, dz) + 180).
    float FrogportYaw(const NbtTagPtr& nbt) {
        auto target = Child(nbt, "Target");
        auto rel = Child(target, "RelativePos");
        if (rel) {
            auto ix = Child(rel, "X"), iz = Child(rel, "Z");
            if (ix && iz && ix->type == TagType::INT && iz->type == TagType::INT) {
                float dx = (float)bytesToInt(ix->payload), dz = (float)bytesToInt(iz->payload);
                if (dx != 0.0f || dz != 0.0f)
                    return atan2f(dx, dz) * 57.2957763671875f + 180.0f;
            }
        }
        auto yaw = Child(nbt, "PlacedYaw");
        return yaw && yaw->type == TagType::FLOAT ? bytesToFloat(yaw->payload) : 0.0f;
    }
}

void AppendCreateFrogportPsi(const string& blockName, int x, int y, int z,
    const NbtTagPtr& nbt, ModelData& model) {
    (void)x; (void)y; (void)z;
    string base = blockName.substr(0, blockName.find('['));
    if (base == "create:package_frogport") {
        // Rest pose: no animation / anticipation -> headPitch 0, tongue length 0 (not drawn).
        const float yaw = FrogportYaw(nbt);
        auto goggles = Child(nbt, "Goggles");
        const bool hasGoggles = goggles && goggles->type == TagType::BYTE && bytesToByte(goggles->payload) != 0;
        ModelData body = ProcessModelJson("create", "block/package_frogport/body", 0, 0, false, 300);
        ModelData head = ProcessModelJson("create",
            hasGoggles ? "block/package_frogport/head_goggles" : "block/package_frogport/head", 0, 0, false, 301);
        Rotate(body, 0.0f, yaw);
        Rotate(head, 0.0f, yaw);
        AppendPart(model, std::move(body));
        AppendPart(model, std::move(head));
        return;
    }
    if (base == "create:portable_storage_interface" || base == "create:portable_fluid_interface") {
        const string prefix = base == "create:portable_fluid_interface" ?
            "block/portable_fluid_interface/" : "block/portable_storage_interface/";
        const string facing = StateValue(blockName, "facing");
        // AngleHelper.horizontalAngle: south 0, west -90, north 180, east -270; vertical 0.
        float yaw = facing == "west" ? -90.0f : facing == "north" ? 180.0f : facing == "east" ? -270.0f : 0.0f;
        float pitch = facing == "up" || facing.empty() ? 0.0f : facing == "down" ? 180.0f : 90.0f;
        // Static: not connected (unpowered middle), extension progress 0.
        ModelData middle = ProcessModelJson("create", prefix + "block_middle", 0, 0, false, 302);
        ModelData top = ProcessModelJson("create", prefix + "block_top", 0, 0, false, 303);
        Translate(middle, 0.0f, 0.375f, 0.0f);
        Rotate(middle, pitch, yaw);
        Rotate(top, pitch, yaw);
        AppendPart(model, std::move(middle));
        AppendPart(model, std::move(top));
    }
}
