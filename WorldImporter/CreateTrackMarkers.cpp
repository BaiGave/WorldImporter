#include "CreateTrackMarkers.h"

#include "block.h"

#include <cmath>
#include <cstring>
#include <string>

using namespace std;

namespace {

// Partial-model cache indices. 370-379 is reserved for these markers.
constexpr int kIdxFlagOff = 370;
constexpr int kIdxFlagAssemble = 371;
constexpr int kIdxSignalOff = 372;
constexpr int kIdxSignalOn = 373;
constexpr int kIdxOverlayStation = 374;
constexpr int kIdxOverlaySignal = 375;
constexpr int kIdxOverlayDual = 376;
constexpr int kIdxOverlayObserver = 377;

constexpr double kPi = 3.14159265358979323846;

struct TrackGeom {
    float ax, ay, az;
    float lx, ly, lz;
    bool ok;
};

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

string ReadString(const NbtTagPtr& parent, const char* name) {
    if (!parent) return "";
    auto tag = getChildByName(parent, name);
    if (!tag || tag->type != TagType::STRING) return "";
    return bytesToString(tag->payload);
}

bool ReadInt(const NbtTagPtr& parent, const char* name, int& out) {
    if (!parent) return false;
    auto tag = getChildByName(parent, name);
    if (!tag || tag->type != TagType::INT || tag->payload.size() < 4) return false;
    out = bytesToInt(tag->payload);
    return true;
}

// CompoundTag.getBoolean: missing or zero byte is false. Positive is AxisDirection.POSITIVE.
bool ReadBool(const NbtTagPtr& parent, const char* name) {
    if (!parent) return false;
    auto tag = getChildByName(parent, name);
    if (!tag || tag->type != TagType::BYTE || tag->payload.empty()) return false;
    return bytesToByte(tag->payload) != 0;
}

bool HasCompound(const NbtTagPtr& parent, const char* name) {
    if (!parent) return false;
    auto tag = getChildByName(parent, name);
    return tag && tag->type == TagType::COMPOUND;
}

// Right-handed degrees. Exact at multiples of 90 so ends stay on the block grid.
void SinCosDeg(float deg, float& s, float& c) {
    float wrapped = fmod(deg, 360.0f);
    if (wrapped < 0.0f) wrapped += 360.0f;
    if (wrapped == 0.0f) { s = 0.0f; c = 1.0f; return; }
    if (wrapped == 90.0f) { s = 1.0f; c = 0.0f; return; }
    if (wrapped == 180.0f) { s = 0.0f; c = -1.0f; return; }
    if (wrapped == 270.0f) { s = -1.0f; c = 0.0f; return; }
    float rad = deg * static_cast<float>(kPi / 180.0);
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

void Scale(ModelData& model, float s) {
    if (s == 1.0f) return;
    auto& v = model.vertices;
    for (size_t i = 0; i + 2 < v.size(); i += 3) {
        v[i] *= s;
        v[i + 1] *= s;
        v[i + 2] *= s;
    }
}

// Ry: x' = x c + z s, z' = -x s + z c. Matches CreateFlaps / PoseStack Y.
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

float RadToDeg(double rad) {
    double deg = rad * (180.0 / kPi);
    double nearest90 = round(deg / 90.0) * 90.0;
    if (fabs(deg - nearest90) < 1e-3) return static_cast<float>(nearest90);
    return static_cast<float>(deg);
}

// TrackShape axes. axes[0] is the overlay axis; the last axis is the flag axis.
// Ascending normals are unused by the applied yaw/pitch (roll is not rendered).
TrackGeom ShapeOf(const string& shape) {
    auto one = [](float x, float y, float z) {
        return TrackGeom{x, y, z, x, y, z, true};
    };
    auto two = [](float x, float y, float z, float x2, float y2, float z2) {
        return TrackGeom{x, y, z, x2, y2, z2, true};
    };
    if (shape == "zo") return one(0, 0, 1);
    if (shape == "xo") return one(1, 0, 0);
    if (shape == "pd") return one(1, 0, 1);
    if (shape == "nd") return one(-1, 0, 1);
    if (shape == "an") return one(0, 1, -1);
    if (shape == "as") return one(0, 1, 1);
    if (shape == "ae") return one(1, 1, 0);
    if (shape == "aw") return one(-1, 1, 0);
    if (shape == "tn") return one(0, 0, -1);
    if (shape == "ts") return one(0, 0, 1);
    if (shape == "te") return one(1, 0, 0);
    if (shape == "tw") return one(-1, 0, 0);
    if (shape == "cr_o") return two(0, 0, 1, 1, 0, 0);
    if (shape == "cr_d") return two(1, 0, 1, -1, 0, 1);
    if (shape == "cr_pdx") return two(1, 0, 0, 1, 0, 1);
    if (shape == "cr_pdz") return two(0, 0, 1, 1, 0, 1);
    if (shape == "cr_ndx") return two(1, 0, 0, -1, 0, 1);
    if (shape == "cr_ndz") return two(0, 0, 1, -1, 0, 1);
    return {};
}

// Vec3.normalize: length below 1e-4 stays zero.
bool Normalize(float& x, float& y, float& z) {
    double len = sqrt(static_cast<double>(x) * x + static_cast<double>(y) * y + static_cast<double>(z) * z);
    if (len < 1.0e-4) {
        x = y = z = 0.0f;
        return false;
    }
    x = static_cast<float>(x / len);
    y = static_cast<float>(y / len);
    z = static_cast<float>(z / len);
    return true;
}

// TrackRenderer.getModelAngles pitch (x) and yaw (y). Roll uses the shape normal
// and is not applied by prepareTrackOverlay.
void ModelYawPitch(float dx, float dy, float dz, float& yawDeg, float& pitchDeg) {
    float len = sqrt(dx * dx + dz * dz);
    double yaw = atan2(static_cast<double>(dx), static_cast<double>(dz));
    double pitch = atan2(static_cast<double>(len), static_cast<double>(dy)) - (kPi / 2.0);
    yawDeg = RadToDeg(yaw);
    pitchDeg = RadToDeg(pitch);
}

// Direction.getNearest(x, 0, z). values() order is down, up, north, south, west, east.
// The best dot starts at Float.MIN_VALUE so a zero vector stays north.
const char* NearestHorizontal(float x, float z) {
    struct Dir { const char* name; float sx, sz; };
    static const Dir dirs[] = {
        {"down", 0, 0}, {"up", 0, 0},
        {"north", 0, -1}, {"south", 0, 1},
        {"west", -1, 0}, {"east", 1, 0},
    };
    const char* best = "north";
    float bestDot = 1.1754944e-38f;
    for (const auto& dir : dirs) {
        float dot = x * dir.sx + z * dir.sz;
        if (dot > bestDot) {
            bestDot = dot;
            best = dir.name;
        }
    }
    return best;
}

// toYRot: south 0, west 90, north 180, east 270.
int ToYRot(const char* dir) {
    if (strcmp(dir, "west") == 0) return 90;
    if (strcmp(dir, "north") == 0) return 180;
    if (strcmp(dir, "east") == 0) return 270;
    return 0;
}

// getClockWise normal, horizontal: north->east, east->south, south->west, west->north.
void ClockWiseStep(const char* dir, float& x, float& z) {
    if (strcmp(dir, "north") == 0) { x = 1.0f; z = 0.0f; }
    else if (strcmp(dir, "east") == 0) { x = 0.0f; z = 1.0f; }
    else if (strcmp(dir, "south") == 0) { x = -1.0f; z = 0.0f; }
    else { x = 0.0f; z = -1.0f; }
}

bool TargetTrack(const NbtTagPtr& nbt, int x, int y, int z, int& tx, int& ty, int& tz, bool& positive) {
    auto track = nbt ? getChildByName(nbt, "TargetTrack") : NbtTagPtr();
    int dx = 0, dy = 0, dz = 0;
    if (!ReadInt(track, "X", dx) || !ReadInt(track, "Y", dy) || !ReadInt(track, "Z", dz)) return false;
    tx = x + dx;
    ty = y + dy;
    tz = z + dz;
    positive = ReadBool(nbt, "TargetDirection");
    return true;
}

// create:track only. Bezier on the marker, or Smoothing on the track BE, skips the overlay.
bool PlainTrack(const NbtTagPtr& nbt, int tx, int ty, int tz, string& shape) {
    if (HasCompound(nbt, "Bezier")) return false;
    Block block = GetBlockById(GetBlockId(tx, ty, tz));
    if (block.GetNameAndNameSpaceWithoutState() != "create:track") return false;
    auto trackNbt = GetBlockEntityNbt(tx, ty, tz);
    if (trackNbt && getChildByName(trackNbt, "Smoothing")) return false;
    shape = StateValue(block.name, "shape");
    if (shape.empty()) shape = "zo";
    return ShapeOf(shape).ok;
}

// PoseStack and SuperByteBuffer post-multiply: the last call hits the vertex first.
// renderFlag after transformFlag: translate(1/32), rotateY(flipped?0:180), translate(-1/32),
// then transformFlag's rotateX, uncenter, translate, rotateY, center.
// Flag value 0 -> progress 0 -> rotateX of ±270.
void PoseFlag(ModelData& part, float yRot, bool flipped) {
    const float nudge = 0.001953125f;
    const float z = flipped ? (0.875f - nudge) : (0.125f + nudge);
    const float xDeg = (flipped ? 1.0f : -1.0f) * 270.0f;
    Translate(part, -0.03125f, 0.0f, 0.0f);
    RotateY(part, flipped ? 0.0f : 180.0f);
    Translate(part, 0.03125f, 0.0f, 0.0f);
    RotateX(part, xDeg);
    Translate(part, -0.5f, -0.5f, -0.5f);
    Translate(part, nudge, 0.59375f, z);
    RotateY(part, yRot);
    Translate(part, 0.5f, 0.5f, 0.5f);
}

// prepareTrackOverlay then the partial translate/scale/translate. Scale is 1.
// Caller translate by the target offset is the outermost pose op.
void PoseOverlay(ModelData& part, float yawDeg, float pitchDeg, float liftY, float liftZ,
    float offX, float offY, float offZ) {
    Translate(part, -0.5f, 0.0f, -0.5f);
    Scale(part, 1.0f);
    Translate(part, 0.5f, 0.0f, 0.5f);
    Translate(part, 0.0f, liftY, liftZ);
    Translate(part, -0.5f, -0.5f, -0.5f);
    RotateX(part, pitchDeg);
    RotateY(part, yawDeg);
    Translate(part, 0.5f, 0.5f, 0.5f);
    Translate(part, offX, offY, offZ);
}

bool ResolveFlag(const TrackGeom& geom, bool positive, int dx, int dz, float& yRot, bool& flipped) {
    float ax = geom.lx * (positive ? 1.0f : -1.0f);
    float az = geom.lz * (positive ? 1.0f : -1.0f);
    if (ax == 0.0f && geom.ly == 0.0f && az == 0.0f) return false;
    const char* nearest = NearestHorizontal(ax, az);
    yRot = static_cast<float>(-ToYRot(nearest) - 90);
    flipped = false;
    if (dx * dx + dz * dz == 0) return true;
    float cx, cz;
    ClockWiseStep(nearest, cx, cz);
    flipped = static_cast<float>(dx) * cx + static_cast<float>(dz) * cz > 0.0f;
    return true;
}

void AppendOverlay(ModelData& model, const string& shape, bool positive, int offX, int offY, int offZ,
    const char* path, int index) {
    TrackGeom geom = ShapeOf(shape);
    if (!geom.ok) return;
    float dx = geom.ax * (positive ? 1.0f : -1.0f);
    float dy = geom.ay * (positive ? 1.0f : -1.0f);
    float dz = geom.az * (positive ? 1.0f : -1.0f);
    if (!Normalize(dx, dy, dz)) return;
    float yaw = 0.0f, pitch = 0.0f;
    ModelYawPitch(dx, dy, dz, yaw, pitch);
    float liftY = 0.0f, liftZ = 0.0f;
    if (geom.ay != 0.0f) {
        liftY = 0.4375f;
        liftZ = (positive ? 1.0f : -1.0f) * (2.5f / 16.0f);
    }
    ModelData part = LoadPartial(path, index);
    PoseOverlay(part, yaw, pitch, liftY, liftZ,
        static_cast<float>(offX), static_cast<float>(offY), static_cast<float>(offZ));
    Append(model, std::move(part));
}

void AppendStation(ModelData& model, const string& blockName, int x, int y, int z, const NbtTagPtr& nbt) {
    int tx, ty, tz;
    bool positive = false;
    if (!TargetTrack(nbt, x, y, z, tx, ty, tz, positive)) return;
    Block track = GetBlockById(GetBlockId(tx, ty, tz));
    if (track.GetNameAndNameSpaceWithoutState() != "create:track") return;
    string shape = StateValue(track.name, "shape");
    if (shape.empty()) shape = "zo";
    TrackGeom geom = ShapeOf(shape);
    if (!geom.ok) return;

    float yRot = 0.0f;
    bool flipped = false;
    if (!ResolveFlag(geom, positive, tx - x, tz - z, yRot, flipped)) return;
    const bool assembling = StateValue(blockName, "assembling") == "true";
    const char* flagPath = assembling ? "block/track_station/flag_assemble" : "block/track_station/flag_off";
    int flagIndex = assembling ? kIdxFlagAssemble : kIdxFlagOff;
    ModelData flag = LoadPartial(flagPath, flagIndex);
    PoseFlag(flag, yRot, flipped);
    Append(model, std::move(flag));

    if (assembling) return;
    string plain;
    if (!PlainTrack(nbt, tx, ty, tz, plain)) return;
    AppendOverlay(model, plain, positive, tx - x, ty - y, tz - z,
        "block/track_overlay/station", kIdxOverlayStation);
}

void AppendSignal(ModelData& model, int x, int y, int z, const NbtTagPtr& nbt) {
    const bool red = ReadString(nbt, "State") == "RED";
    ModelData lamp = LoadPartial(red ? "block/track_signal/indicator_on" : "block/track_signal/indicator_off",
        red ? kIdxSignalOn : kIdxSignalOff);
    Append(model, std::move(lamp));

    string overlay = ReadString(nbt, "Overlay");
    if (overlay != "RENDER" && overlay != "DUAL") return;
    int tx, ty, tz;
    bool positive = false;
    if (!TargetTrack(nbt, x, y, z, tx, ty, tz, positive)) return;
    string shape;
    if (!PlainTrack(nbt, tx, ty, tz, shape)) return;
    if (overlay == "DUAL") {
        AppendOverlay(model, shape, positive, tx - x, ty - y, tz - z,
            "block/track_overlay/signal_dual", kIdxOverlayDual);
    } else {
        AppendOverlay(model, shape, positive, tx - x, ty - y, tz - z,
            "block/track_overlay/signal", kIdxOverlaySignal);
    }
}

void AppendObserver(ModelData& model, int x, int y, int z, const NbtTagPtr& nbt) {
    int tx, ty, tz;
    bool positive = false;
    if (!TargetTrack(nbt, x, y, z, tx, ty, tz, positive)) return;
    string shape;
    if (!PlainTrack(nbt, tx, ty, tz, shape)) return;
    AppendOverlay(model, shape, positive, tx - x, ty - y, tz - z,
        "block/track_overlay/observer", kIdxOverlayObserver);
}

} // namespace

void AppendCreateTrackMarkers(const string& blockName, int x, int y, int z,
    const NbtTagPtr& blockEntityNbt, ModelData& model) {
    string id = LocalId(blockName);
    if (id == "track_station") AppendStation(model, blockName, x, y, z, blockEntityNbt);
    else if (id == "track_signal") AppendSignal(model, x, y, z, blockEntityNbt);
    else if (id == "track_observer") AppendObserver(model, x, y, z, blockEntityNbt);
}
