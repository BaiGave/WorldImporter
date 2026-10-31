#include "CreateBellCrafter.h"
#include "block.h"

#include <cmath>
#include <string>

using namespace std;

namespace {

constexpr int kIdxHauntedBell = 330;
constexpr int kIdxPeculiarBell = 331;
constexpr int kIdxCog = 332;
constexpr int kIdxLid = 333;
// AllPartialModels: BELT_FRAME = mechanical_crafter/belt,
// BELT = mechanical_crafter/belt_animated. There is no belt_frame model.
constexpr int kIdxBeltFrame = 334;
constexpr int kIdxBelt = 335;
constexpr int kIdxArrow = 336;

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

string BaseId(const string& blockName) {
    size_t bracket = blockName.find('[');
    return bracket == string::npos ? blockName : blockName.substr(0, bracket);
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

// Ry: x' = x c + z s, z' = -x s + z c. Matches VecHelper axis Y and rotateCentered(UP).
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

// Rz: x' = x c - y s, y' = x s + y c.
void RotateZ(ModelData& model, float deg) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        float x = v[i];
        float y = v[i + 1];
        v[i] = x * c - y * s;
        v[i + 1] = x * s + y * c;
    }
}

// Rx: y' = y c - z s, z' = y s + z c. EAST is +X.
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

// M = T(pivot) * R * T(-pivot). Leftmost matrix is applied last.
void RotateAbout(ModelData& model, float px, float py, float pz, char axis, float deg) {
    Translate(model, -px, -py, -pz);
    if (axis == 'x') RotateX(model, deg);
    else if (axis == 'z') RotateZ(model, deg);
    else RotateY(model, deg);
    Translate(model, px, py, pz);
}

void RotateCentered(ModelData& model, char axis, float deg) {
    RotateAbout(model, 0.5f, 0.5f, 0.5f, axis, deg);
}

// AngleHelper.horizontalAngle: toYRot, then negate when the axis is X.
// south 0, west -90, north 180, east -270.
float HorizontalAngle(const string& facing) {
    if (facing == "west") return -90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return -270.0f;
    return 0.0f;
}

// Pointing.xRotation: up 0, left 270, down 180, right 90.
int PointingXRot(const string& pointing) {
    if (pointing == "left") return 270;
    if (pointing == "down") return 180;
    if (pointing == "right") return 90;
    return 0;
}

// VecHelper.rotate(vec, deg, axis).
void RotateVec(float& x, float& y, float& z, float deg, char axis) {
    float s, c;
    SinCosDeg(deg, s, c);
    if (s == 0.0f && c == 1.0f) return;
    if (axis == 'x') {
        float ny = y * c - z * s;
        float nz = z * c + y * s;
        y = ny;
        z = nz;
    } else if (axis == 'y') {
        float nx = x * c + z * s;
        float nz = z * c - x * s;
        x = nx;
        z = nz;
    } else {
        float nx = x * c - y * s;
        float ny = y * c + x * s;
        x = nx;
        y = ny;
    }
}

// Direction.getNearest, enum order DOWN, UP, NORTH, SOUTH, WEST, EAST.
string NearestDirection(float x, float y, float z) {
    struct Cand { const char* name; float x, y, z; };
    static const Cand cands[] = {
        {"down", 0, -1, 0}, {"up", 0, 1, 0},
        {"north", 0, 0, -1}, {"south", 0, 0, 1},
        {"west", -1, 0, 0}, {"east", 1, 0, 0},
    };
    const char* best = "up";
    float bestDot = -1.0e30f;
    for (const auto& cand : cands) {
        float dot = x * cand.x + y * cand.y + z * cand.z;
        if (dot > bestDot) {
            bestDot = dot;
            best = cand.name;
        }
    }
    return best;
}

void DirectionStep(const string& dir, int& dx, int& dy, int& dz) {
    dx = dy = dz = 0;
    if (dir == "down") dy = -1;
    else if (dir == "up") dy = 1;
    else if (dir == "north") dz = -1;
    else if (dir == "south") dz = 1;
    else if (dir == "west") dx = -1;
    else if (dir == "east") dx = 1;
}

// MechanicalCrafterBlock.getTargetDirection.
string TargetDirection(const string& facing, int xRot) {
    float x = 0.0f, y = 1.0f, z = 0.0f;
    RotateVec(x, y, z, -static_cast<float>(xRot), 'z');
    RotateVec(x, y, z, HorizontalAngle(facing), 'y');
    return NearestDirection(x, y, z);
}

bool IsMechanicalCrafter(const string& name) {
    return BaseId(name) == "create:mechanical_crafter";
}

// Same facing, and pointing xRotations are not 180 apart.
bool IsValidTarget(int x, int y, int z, const string& facing, int xRot) {
    Block neighbour = GetBlockById(GetBlockId(x, y, z));
    if (!IsMechanicalCrafter(neighbour.name)) return false;
    if (StateValue(neighbour.name, "facing") != facing) return false;
    int other = PointingXRot(StateValue(neighbour.name, "pointing"));
    return std::abs(xRot - other) != 180;
}

// SuperByteBuffer calls accumulate like a PoseStack: the last rotateCentered
// hits the vertex first. renderAndTransform is UP then EAST, so EAST, then UP.
void RenderAndTransform(ModelData& part, float yRot, float xRot) {
    RotateCentered(part, 'x', xRot);
    RotateCentered(part, 'y', yRot + 90.0f);
}

// Partial models are block/haunted_bell and block/peculiar_bell (the body).
// Frames live in the blockstate. Not ringing, so no swing.
void AppendBell(ModelData& model, const string& id, const string& blockName) {
    string facing = StateValue(blockName, "facing");
    if (facing.empty()) facing = "south";
    string attachment = StateValue(blockName, "attachment");
    float rY = HorizontalAngle(facing);
    if (attachment == "single_wall" || attachment == "double_wall") rY += 90.0f;

    const char* path = id == "haunted_bell" ? "block/haunted_bell" : "block/peculiar_bell";
    int index = id == "haunted_bell" ? kIdxHauntedBell : kIdxPeculiarBell;
    ModelData bell = LoadPartial(path, index);
    RotateCentered(bell, 'y', rY);
    Append(model, std::move(bell));
}

// speed 0: KineticBlockEntityVisual.rotationOffset only. Crafter is a small cog.
// Even sum of the two axes perpendicular to the kinetic axis -> 22.5 degrees.
float CogPhase(char axis, int x, int y, int z) {
    int parity = axis == 'x' ? y + z : x + y;
    return parity % 2 == 0 ? 22.5f : 0.0f;
}

void AppendCrafter(ModelData& model, const string& blockName, int x, int y, int z,
    const NbtTagPtr& nbt) {
    string facing = StateValue(blockName, "facing");
    if (facing.empty()) facing = "south";
    string pointing = StateValue(blockName, "pointing");
    if (pointing.empty()) pointing = "up";
    int xRot = PointingXRot(pointing);
    float yRot = HorizontalAngle(facing);
    // getRotationAxis = horizontal facing axis. south/north -> Z, east/west -> X.
    char axis = (facing == "east" || facing == "west") ? 'x' : 'z';

    // Authored on Y. Vertex order: R_east(90), then R_up(0 or 90), then spin on the
    // facing axis. Facing south leaves the cog axis on Z.
    ModelData cog = LoadPartial("block/cogwheel_shaftless", kIdxCog);
    RotateCentered(cog, 'x', 90.0f);
    if (axis == 'x') RotateCentered(cog, 'y', 90.0f);
    RotateCentered(cog, axis, CogPhase(axis, x, y, z));
    Append(model, std::move(cog));

    // Cover byte 1 is the static covered IDLE lid.
    bool covered = false;
    if (nbt) {
        auto tag = getChildByName(nbt, "Cover");
        covered = tag && tag->type == TagType::BYTE && bytesToByte(tag->payload) == 1;
    }
    if (covered) {
        ModelData lid = LoadPartial("block/mechanical_crafter/lid", kIdxLid);
        RenderAndTransform(lid, yRot, static_cast<float>(xRot));
        Append(model, std::move(lid));
    }

    string target = TargetDirection(facing, xRot);
    int dx, dy, dz;
    DirectionStep(target, dx, dy, dz);
    if (IsValidTarget(x + dx, y + dy, z + dz, facing, xRot)) {
        ModelData belt = LoadPartial("block/mechanical_crafter/belt_animated", kIdxBelt);
        RenderAndTransform(belt, yRot, static_cast<float>(xRot));
        Append(model, std::move(belt));
        ModelData frame = LoadPartial("block/mechanical_crafter/belt", kIdxBeltFrame);
        RenderAndTransform(frame, yRot, static_cast<float>(xRot));
        Append(model, std::move(frame));
    } else {
        ModelData arrow = LoadPartial("block/mechanical_crafter/arrow", kIdxArrow);
        RenderAndTransform(arrow, yRot, static_cast<float>(xRot));
        Append(model, std::move(arrow));
    }
}

} // namespace

void AppendCreateBellCrafter(const string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model) {
    string id = LocalId(blockName);
    if (id == "haunted_bell" || id == "peculiar_bell") {
        AppendBell(model, id, blockName);
        return;
    }
    if (id == "mechanical_crafter") AppendCrafter(model, blockName, x, y, z, blockEntityNbt);
}
