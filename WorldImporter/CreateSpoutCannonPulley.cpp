#include "CreateSpoutCannonPulley.h"
#include "blockstate.h"

#include <cmath>
#include <string>

using namespace std;

namespace {

constexpr int kIdxSpoutTop = 310;
constexpr int kIdxSpoutMiddle = 311;
constexpr int kIdxSpoutBottom = 312;
constexpr int kIdxConnector = 313;
constexpr int kIdxPipe = 314;
constexpr int kIdxShaft = 315;
constexpr int kIdxRopeCoil = 316;
constexpr int kIdxHoseCoil = 317;
constexpr int kIdxElevatorCoil = 318;
constexpr int kIdxHoseMagnet = 319;
constexpr int kIdxElevatorMagnet = 320;

void MarkNoCull(ModelData& model) {
    for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
}

void Append(ModelData& target, ModelData part) {
    if (part.vertices.empty()) return;
    MarkNoCull(part);
    MergeModelsDirectly(target, part);
}

void AppendAtY(ModelData& target, const ModelData& src, float y) {
    if (src.vertices.empty()) return;
    if (y == 0.0f) {
        MergeModelsDirectly(target, src);
        return;
    }
    ModelData part = src;
    ApplyDoublePositionOffset(part, 0.0, static_cast<double>(y), 0.0);
    MergeModelsDirectly(target, part);
}

ModelData LoadPartial(const string& path, int index) {
    ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
    MarkNoCull(model);
    return model;
}

ModelData LoadBlockstate(const string& id, const string& fallback, int index) {
    ModelData model = GetRandomModelFromCache("create", id);
    if (model.vertices.empty()) model = ProcessModelJson("create", fallback, 0, 0, false, index);
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

// Right-handed degrees. Exact at multiples of 90 so shaft ends stay on the block grid.
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

// Ry: x' = x c + z s, z' = -x s + z c. About the origin.
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

// M = T(pivot) * R * T(-pivot). Leftmost matrix is applied last.
void RotateAbout(ModelData& model, float px, float py, float pz, char axis, float deg) {
    Translate(model, -px, -py, -pz);
    if (axis == 'x') RotateX(model, deg);
    else if (axis == 'z') RotateZ(model, deg);
    else RotateY(model, deg);
    Translate(model, px, py, pz);
}

// CachedBuffers.partialFacing = center * Ry(horizontalAngle) * uncenter.
void PartialFacing(ModelData& model, float horizontalAngle) {
    RotateAbout(model, 0.5f, 0.5f, 0.5f, 'y', horizontalAngle);
}

// AngleHelper.horizontalAngle: south 0, west -90, north 180, east -270.
float HorizontalAngle(const string& facing) {
    if (facing == "west") return -90.0f;
    if (facing == "north") return 180.0f;
    if (facing == "east") return -270.0f;
    return 0.0f;
}

float NbtFloat(const NbtTagPtr& tag) {
    if (!tag || tag->payload.size() < 4) return 0.0f;
    if (tag->type == TagType::FLOAT) return bytesToFloat(tag->payload);
    if (tag->type == TagType::DOUBLE && tag->payload.size() >= 8)
        return static_cast<float>(bytesToDouble(tag->payload));
    return 0.0f;
}

// Rope/elevator store a float. Hose stores LerpedFloat {Value, Target, ...}.
float ReadOffset(const NbtTagPtr& nbt) {
    if (!nbt) return 0.0f;
    auto tag = getChildByName(nbt, "Offset");
    if (!tag) return 0.0f;
    if (tag->type == TagType::COMPOUND) {
        auto value = getChildByName(tag, "Value");
        if (value) return NbtFloat(value);
        return 0.0f;
    }
    return NbtFloat(tag);
}

bool NbtFlag(const NbtTagPtr& nbt, const string& name) {
    if (!nbt) return false;
    auto tag = getChildByName(nbt, name);
    return tag && tag->type == TagType::BYTE && bytesToByte(tag->payload) != 0;
}

bool HasTag(const NbtTagPtr& nbt, const string& name) {
    return nbt && getChildByName(nbt, name) != nullptr;
}

void RotateShaft(ModelData& shaft, const string& axis) {
    // Authored along Y. Axis x: Rz(90) about centre. Axis z: Rx(90) about centre.
    if (axis == "x") RotateAbout(shaft, 0.5f, 0.5f, 0.5f, 'z', 90.0f);
    else if (axis == "z") RotateAbout(shaft, 0.5f, 0.5f, 0.5f, 'x', 90.0f);
}

// facing.getClockWise().getAxis(): north/south -> x, east/west -> z.
string ClockwiseAxis(const string& facing) {
    if (facing == "north" || facing == "south") return "x";
    if (facing == "east" || facing == "west") return "z";
    return "y";
}

void AppendSpout(ModelData& model) {
    // Squeeze is 0 at rest: three nozzle bits, no transform. They hang below y=0.
    Append(model, LoadPartial("block/spout/top", kIdxSpoutTop));
    Append(model, LoadPartial("block/spout/middle", kIdxSpoutMiddle));
    Append(model, LoadPartial("block/spout/bottom", kIdxSpoutBottom));
}

void AppendCannon(ModelData& model, const NbtTagPtr& nbt) {
    float yaw = 0.0f;
    if (nbt) {
        auto tag = getChildByName(nbt, "DefaultYaw");
        if (tag) yaw = NbtFloat(tag);
    }
    constexpr float pitch = 40.0f;

    // T(.5,0,.5) * Ry(yaw+90) * T(-.5,0,-.5)
    ModelData connector = LoadPartial("block/schematicannon/connector", kIdxConnector);
    Translate(connector, -0.5f, 0.0f, -0.5f);
    RotateY(connector, yaw + 90.0f);
    Translate(connector, 0.5f, 0.0f, 0.5f);
    Append(model, std::move(connector));

    // T(.5,.9375,.5) * Ry(yaw+90) * Rz(pitch) * T(-.5,-.9375,-.5)
    ModelData pipe = LoadPartial("block/schematicannon/pipe", kIdxPipe);
    Translate(pipe, -0.5f, -0.9375f, -0.5f);
    RotateZ(pipe, pitch);
    RotateY(pipe, yaw + 90.0f);
    Translate(pipe, 0.5f, 0.9375f, 0.5f);
    Append(model, std::move(pipe));
}

enum class PulleyKind { Rope, Hose, Elevator };

void AppendPulley(ModelData& model, PulleyKind kind, const string& blockName, const NbtTagPtr& nbt) {
    string facing = StateValue(blockName, "facing");
    if (facing.empty()) facing = "south";
    string axis = kind == PulleyKind::Rope ? StateValue(blockName, "axis") : ClockwiseAxis(facing);
    if (axis.empty()) axis = "y";

    ModelData shaft = LoadPartial("block/shaft", kIdxShaft);
    RotateShaft(shaft, axis);
    Append(model, std::move(shaft));

    const char* coilPath = "block/rope_pulley/rope_coil";
    int coilIndex = kIdxRopeCoil;
    if (kind == PulleyKind::Hose) {
        coilPath = "block/hose_pulley/hose_coil";
        coilIndex = kIdxHoseCoil;
    } else if (kind == PulleyKind::Elevator) {
        coilPath = "block/elevator_pulley/rope_coil";
        coilIndex = kIdxElevatorCoil;
    }
    // Rope/hose: Direction(POSITIVE, shaftAxis). Elevator: block facing.
    float coilAngle = 0.0f;
    if (kind == PulleyKind::Elevator) coilAngle = HorizontalAngle(facing);
    else if (axis == "x") coilAngle = -270.0f; // east
    else if (axis == "z") coilAngle = 0.0f;    // south
    ModelData coil = LoadPartial(coilPath, coilIndex);
    PartialFacing(coil, coilAngle);
    Append(model, std::move(coil));

    float offset = ReadOffset(nbt);
    if (!std::isfinite(offset)) offset = 0.0f;

    bool running = false;
    if (kind == PulleyKind::Hose) running = true;
    else if (kind == PulleyKind::Elevator) {
        // PulleyRenderer.isPulleyRunning: running || mirrorParent != null (not virtual).
        running = NbtFlag(nbt, "Running") || HasTag(nbt, "MirrorParent");
    } else {
        running = NbtFlag(nbt, "Running");
    }

    auto orientElevator = [&](ModelData& part) {
        // center * Ry(180 + horizontalAngle(facing)) * uncenter
        PartialFacing(part, 180.0f + HorizontalAngle(facing));
    };

    if (running || offset == 0.0f) {
        ModelData magnet;
        if (kind == PulleyKind::Elevator) {
            // ElevatorPulleyRenderer always draws the full magnet, never a half.
            magnet = LoadPartial("block/elevator_pulley/pulley_magnet", kIdxElevatorMagnet);
            orientElevator(magnet);
        } else if (offset > 0.25f) {
            magnet = kind == PulleyKind::Rope
                ? LoadBlockstate("pulley_magnet", "block/rope_pulley/pulley_magnet", 318)
                : LoadPartial("block/hose_pulley/pulley_magnet", kIdxHoseMagnet);
        } else {
            magnet = kind == PulleyKind::Rope
                ? LoadPartial("block/rope_pulley/rope_half_magnet", 310)
                : LoadPartial("block/hose_pulley/rope_half_magnet", 311);
        }
        AppendAtY(model, magnet, -offset);
    }

    // ElevatorPulleyRenderer: no rope at offset 0; half rope whenever the
    // fraction is near an integer; full belts at -(offset - i), i < offset-0.25.
    if (kind == PulleyKind::Elevator) {
        if (offset == 0.0f) return;
        float f = fmod(offset, 1.0f);
        if (f < 0.25f || f > 0.75f) {
            ModelData half = LoadPartial("block/elevator_pulley/rope_half", 314);
            orientElevator(half);
            float at = f > 0.75f ? f - 1.0f : f;
            AppendAtY(model, half, -at);
        }
        if (!running) return;
        ModelData rope = LoadPartial("block/elevator_pulley/rope", 316);
        orientElevator(rope);
        for (int i = 0; i < 4096 && static_cast<float>(i) < offset - 0.25f; ++i)
            AppendAtY(model, rope, -(offset - static_cast<float>(i)));
        return;
    }

    float f = fmod(offset, 1.0f);
    if (offset > 0.75f && (f < 0.25f || f > 0.75f)) {
        ModelData half = kind == PulleyKind::Rope
            ? LoadPartial("block/rope_pulley/rope_half", 312)
            : LoadPartial("block/hose_pulley/rope_half", 313);
        float at = f > 0.75f ? f - 1.0f : f;
        AppendAtY(model, half, -at);
    }
    if (!running) return;
    ModelData rope = kind == PulleyKind::Rope
        ? LoadBlockstate("rope", "block/rope_pulley/rope", 317)
        : LoadPartial("block/hose_pulley/rope", 315);
    for (int i = 0; i < 4096 && static_cast<float>(i) < offset - 1.25f; ++i)
        AppendAtY(model, rope, -(offset - static_cast<float>(i) - 1.0f));
}

} // namespace

void AppendCreateSpoutCannonPulley(const string& blockName, const NbtTagPtr& blockEntityNbt,
    ModelData& model) {
    string id = LocalId(blockName);
    if (id == "spout") {
        AppendSpout(model);
        return;
    }
    if (id == "schematicannon") {
        AppendCannon(model, blockEntityNbt);
        return;
    }
    if (id == "rope_pulley") AppendPulley(model, PulleyKind::Rope, blockName, blockEntityNbt);
    else if (id == "hose_pulley") AppendPulley(model, PulleyKind::Hose, blockName, blockEntityNbt);
    else if (id == "elevator_pulley") AppendPulley(model, PulleyKind::Elevator, blockName, blockEntityNbt);
}
