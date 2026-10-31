#include "CreateFlaps.h"

#include <cmath>
#include <string>

using namespace std;

namespace {

// Partial-model cache indices. 360-369 is reserved for these flaps.
constexpr int kIdxFunnelFlap = 360;
constexpr int kIdxBeltFunnelFlap = 361;
constexpr int kIdxBeltTunnelFlap = 362;

// FlapStuffs constants. X offset and segment step are the Java float literals.
constexpr float kXOffset = 0.0046875f;
constexpr float kSegmentStep = -0.190625f;
constexpr int kFlapCount = 4;

// VecHelper.voxelSpace: (x, y, z) / 16.
constexpr float kTunnelPivotX = 0.0f;
constexpr float kTunnelPivotY = 10.0f / 16.0f;
constexpr float kTunnelPivotZ = 1.0f / 16.0f;
constexpr float kFunnelPivotX = 0.0f;
constexpr float kFunnelPivotY = 10.0f / 16.0f;
constexpr float kFunnelPivotZ = 9.5f / 16.0f;

void MarkNoCull(ModelData& model) {
    for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
}

void Append(ModelData& target, ModelData part) {
    if (part.vertices.empty()) return;
    MarkNoCull(part);
    MergeModelsDirectly(target, part);
}

ModelData LoadPartial(const string& path, int index) {
    ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
    MarkNoCull(model);
    return model;
}

string LocalId(const string& blockName) {
    size_t bracket = blockName.find('[');
    string base = bracket == string::npos ? blockName : blockName.substr(0, bracket);
    size_t colon = base.rfind(':');
    return colon == string::npos ? base : base.substr(colon + 1);
}

string StateValue(const string& blockName, const string& key) {
    size_t begin = blockName.find('[');
    size_t end = blockName.rfind(']');
    if (begin == string::npos || end == string::npos || end <= begin) return "";
    string body = blockName.substr(begin + 1, end - begin - 1);
    size_t pos = 0;
    while (pos < body.size()) {
        size_t comma = body.find(',', pos);
        string pair = body.substr(pos, comma == string::npos ? string::npos : comma - pos);
        size_t sep = pair.find('=');
        if (sep == string::npos) sep = pair.find(':');
        if (sep != string::npos && pair.substr(0, sep) == key) return pair.substr(sep + 1);
        if (comma == string::npos) break;
        pos = comma + 1;
    }
    return "";
}

// Right-handed degrees. Exact at multiples of 90 so ends stay on the block grid.
void SinCosDeg(float deg, float& s, float& c) {
    float wrapped = fmod(deg, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    if (wrapped == 0.0f) { s = 0.0f; c = 1.0f; return; }
    if (wrapped == 90.0f) { s = 1.0f; c = 0.0f; return; }
    if (wrapped == 180.0f) { s = 0.0f; c = -1.0f; return; }
    if (wrapped == 270.0f) { s = -1.0f; c = 0.0f; return; }
    float rad = deg * (3.14159265358979323846f / 180.0f);
    s = sin(rad);
    c = cos(rad);
}

void Translate(ModelData& model, float x, float y, float z) {
    if (x == 0.0f && y == 0.0f && z == 0.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        v[i] += x;
        v[i + 1] += y;
        v[i + 2] += z;
    }
}

// Ry: x' = x c + z s, z' = -x s + z c. Matches CreateBellCrafter / PoseStack Y.
void RotateY(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float x = v[i];
        float z = v[i + 2];
        v[i] = x * c + z * s;
        v[i + 2] = -x * s + z * c;
    }
}

// Rx: y' = y c - z s, z' = y s + z c.
void RotateX(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float y = v[i + 1];
        float z = v[i + 2];
        v[i + 1] = y * c - z * s;
        v[i + 2] = y * s + z * c;
    }
}

// AngleHelper.horizontalAngle(facing.getOpposite()):
// opposite south -> 0, west -> -90, north -> 180, east -> -270.
float HorizontalAngleOfOpposite(const string& facing) {
    if (facing == "north") return 0.0f;
    if (facing == "east") return -90.0f;
    if (facing == "south") return 180.0f;
    if (facing == "west") return -270.0f;
    return 0.0f;
}

bool IsHorizontalFacing(const string& facing) {
    return facing == "north" || facing == "south" || facing == "west" || facing == "east";
}

// FlapStuffs.flapAngle. flapness 0 is the closed rest pose: the product is 0.
float FlapAngle(float flapness, int segment) {
    float intensity = segment == 3 ? 1.5f : static_cast<float>(segment + 1);
    float absFlap = fabs(flapness);
    float angle = sin((1.0f - absFlap) * static_cast<float>(M_PI) * intensity) * 30.0f * flapness;
    if (flapness < 0.0f) angle *= 0.5f;
    return angle;
}

// PoseStack post-multiplies, so the last call hits the vertex first.
// renderFlaps: center, rotateY, uncenter, translate(xOff, 0, zOff),
// then per segment (cumulative x step outside the inner push):
// translate(pivot), rotateX(flapAngle), translateBack(pivot), draw.
void RenderFlaps(ModelData& target, const ModelData& flap, float px, float py, float pz,
    const string& facing, float zOffset) {
    if (flap.vertices.empty() || !IsHorizontalFacing(facing)) return;
    const float yaw = HorizontalAngleOfOpposite(facing);
    for (int segment = 0; segment < kFlapCount; ++segment) {
        ModelData part = flap;
        const float ang = FlapAngle(0.0f, segment);
        Translate(part, -px, -py, -pz);
        RotateX(part, ang);
        Translate(part, px, py, pz);
        Translate(part, kXOffset + static_cast<float>(segment) * kSegmentStep, 0.0f, zOffset);
        Translate(part, -0.5f, -0.5f, -0.5f);
        RotateY(part, yaw);
        Translate(part, 0.5f, 0.5f, 0.5f);
        Append(target, std::move(part));
    }
}

// FunnelBlockEntity.getFlapOffset, then the renderer passes -getFlapOffset().
// Wall funnels: -(-1/16) = 1/16.
// BeltFunnelBlock.Shape order is RETRACTED, EXTENDED, PUSHING, PULLING.
// The synthetic switch maps EXTENDED -> 0.5, PUSHING and PULLING -> -0.125,
// RETRACTED -> 0. Negate that for zOffset.
float FunnelZOffset(const string& id, const string& blockName) {
    if (id == "andesite_funnel" || id == "brass_funnel") return 0.0625f;
    string shape = StateValue(blockName, "shape");
    if (shape == "extended") return -0.5f;
    if (shape == "pushing" || shape == "pulling") return 0.125f;
    return 0.0f;
}

// Direction 3D data: 0 down, 1 up, 2 north, 3 south, 4 west, 5 east.
const char* HorizontalDirectionName(int data3d) {
    switch (data3d) {
    case 2: return "north";
    case 3: return "south";
    case 4: return "west";
    case 5: return "east";
    default: return nullptr;
    }
}

void AppendFunnel(ModelData& model, const string& id, const string& blockName) {
    string facing = StateValue(blockName, "facing");
    // FunnelBlockEntity.hasFlap: getFunnelFacing axis is horizontal.
    // Wall funnels and belt funnels both store that direction as "facing".
    if (!IsHorizontalFacing(facing)) return;
    const bool belt = id == "andesite_belt_funnel" || id == "brass_belt_funnel";
    const char* path = belt ? "block/belt_funnel/flap" : "block/funnel/flap";
    int index = belt ? kIdxBeltFunnelFlap : kIdxFunnelFlap;
    ModelData flap = LoadPartial(path, index);
    RenderFlaps(model, flap, kFunnelPivotX, kFunnelPivotY, kFunnelPivotZ, facing,
        FunnelZOffset(id, blockName));
}

void AppendTunnel(ModelData& model, const NbtTagPtr& nbt) {
    if (!nbt) return;
    auto flaps = getChildByName(nbt, "Flaps");
    if (!flaps || flaps->type != TagType::LIST) return;
    ModelData flap = LoadPartial("block/belt_tunnel/flap", kIdxBeltTunnelFlap);
    for (const auto& child : flaps->children) {
        if (!child || child->type != TagType::INT || child->payload.size() < 4) continue;
        const char* dir = HorizontalDirectionName(bytesToInt(child->payload));
        if (!dir) continue;
        RenderFlaps(model, flap, kTunnelPivotX, kTunnelPivotY, kTunnelPivotZ, dir, 0.0f);
    }
}

} // namespace

void AppendCreateFlaps(const string& blockName, const NbtTagPtr& blockEntityNbt, ModelData& model) {
    string id = LocalId(blockName);
    if (id == "andesite_funnel" || id == "brass_funnel" ||
        id == "andesite_belt_funnel" || id == "brass_belt_funnel") {
        AppendFunnel(model, id, blockName);
        return;
    }
    if (id == "andesite_tunnel" || id == "brass_tunnel") AppendTunnel(model, blockEntityNbt);
}
