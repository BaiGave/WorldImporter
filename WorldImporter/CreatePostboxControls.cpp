#include "CreatePostboxControls.h"

#include <cmath>
#include <string>

using namespace std;

namespace {

// Partial-model cache indices. 380-389 is reserved for these parts.
constexpr int kIdxPostboxFlag = 380;
constexpr int kIdxControlsButton = 381;
constexpr int kIdxControlsIndicator0 = 382;

// transformFlag pivot, and the button axis's Z component (Vec3(0, 1, -0.325)).
constexpr float kFlagPivotY = 0.625f;
constexpr float kFlagPivotZ = 0.125f;
constexpr float kButtonAxisZ = -0.325f;

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

// Ry: x' = x c + z s, z' = -x s + z c. Matches Axis.YP / PoseStack Y.
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

// M = T(pivot) * R * T(-pivot). Leftmost matrix is applied last to the vertex.
void RotateAbout(ModelData& model, float px, float py, float pz, char axis, float deg) {
    Translate(model, -px, -py, -pz);
    if (axis == 'x') RotateX(model, deg);
    else RotateY(model, deg);
    Translate(model, px, py, pz);
}

void RotateCentered(ModelData& model, char axis, float deg) {
    RotateAbout(model, 0.5f, 0.5f, 0.5f, axis, deg);
}

// VecHelper.rotate(vec, deg, axis). Y matches RotateY.
void RotateVecY(float& x, float& z, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    float nx = x * c + z * s;
    float nz = z * c - x * s;
    x = nx;
    z = nz;
}

// Direction.toYRot: south 0, west 90, north 180, east 270.
float ToYRot(const string& facing) {
    if (facing == "west") return 90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return 270.0f;
    return 0.0f;
}

// AngleHelper.horizontalAngle: toYRot, then negate on the X axis.
// south 0, west -90, north 180, east -270. Vertical axes are 0.
float HorizontalAngle(const string& facing) {
    if (facing == "west") return -90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return -270.0f;
    return 0.0f;
}

// AngleHelper.verticalAngle: up -90, down 90, horizontal 0.
float VerticalAngle(const string& facing) {
    if (facing == "up") return -90.0f;
    if (facing == "down") return 90.0f;
    return 0.0f;
}

string OppositeHorizontal(const string& facing) {
    if (facing == "north") return "south";
    if (facing == "south") return "north";
    if (facing == "east") return "west";
    if (facing == "west") return "east";
    return facing;
}

// CachedBuffers.rotateToFace: center, rotateY(horizontalAngle), rotateX(verticalAngle), uncenter.
// PoseStack post-multiplies, so uncenter hits the vertex first.
void PartialFacing(ModelData& model, const string& facing) {
    Translate(model, -0.5f, -0.5f, -0.5f);
    RotateX(model, VerticalAngle(facing));
    RotateY(model, HorizontalAngle(facing));
    Translate(model, 0.5f, 0.5f, 0.5f);
}

// SuperByteBuffer call order is rotateCentered, then transformFlag
// (translate, rotateXDegrees, translateBack). The last call hits the vertex first.
// Flag value 0 -> progress 0 -> rotateX 0, so the pivot translate cancels.
// Open/closed does not move the flag unless the BE flag is chased.
void TransformFlag(ModelData& model, const string& facing) {
    constexpr float progress = 0.0f;
    Translate(model, 0.0f, -kFlagPivotY, -kFlagPivotZ);
    RotateX(model, -progress * 90.0f);
    Translate(model, 0.0f, kFlagPivotY, kFlagPivotZ);
    RotateCentered(model, 'y', 180.0f - ToYRot(facing));
}

// buttonMovementAxis * (-0.07 + -1/24 * value) + buttonMovementAxis * 0.07.
// value 0 cancels. The translation is applied after partialFacing is baked.
void ButtonRestOffset(const string& facing, float& ox, float& oy, float& oz) {
    float x = 0.0f;
    float y = 1.0f;
    float z = kButtonAxisZ;
    RotateVecY(x, z, HorizontalAngle(facing));
    constexpr float value = 0.0f;
    const float scale = (-0.07f - (1.0f / 24.0f) * value) + 0.07f;
    ox = x * scale;
    oy = y * scale;
    oz = z * scale;
}

bool IsPostbox(const string& id) {
    return id.size() > 8 && id.ends_with("_postbox");
}

void AppendPostboxFlag(ModelData& model, const string& blockName) {
    string facing = StateValue(blockName, "facing");
    if (facing.empty()) facing = "north";
    ModelData flag = LoadPartial("block/package_postbox/flag", kIdxPostboxFlag);
    TransformFlag(flag, facing);
    Append(model, std::move(flag));
}

void AppendContraptionControls(ModelData& model, const string& blockName) {
    // Renderer uses state facing.getOpposite().
    string facing = OppositeHorizontal(StateValue(blockName, "facing"));
    if (facing.empty()) facing = "south";

    ModelData button = LoadPartial("block/contraption_controls/button", kIdxControlsButton);
    PartialFacing(button, facing);
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    ButtonRestOffset(facing, ox, oy, oz);
    Translate(button, ox, oy, oz);
    Append(model, std::move(button));

    // (int)indicator / 45 % 8 + 8, then % 8. Value 0 selects indicator_0. No translate.
    ModelData indicator = LoadPartial("block/contraption_controls/indicator_0", kIdxControlsIndicator0);
    PartialFacing(indicator, facing);
    Append(model, std::move(indicator));
}

} // namespace

void AppendCreatePostboxControls(const string& blockName, ModelData& model) {
    string id = LocalId(blockName);
    if (IsPostbox(id)) {
        AppendPostboxFlag(model, blockName);
        return;
    }
    if (id == "contraption_controls") AppendContraptionControls(model, blockName);
}
