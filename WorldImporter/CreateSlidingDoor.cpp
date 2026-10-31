#include "CreateSlidingDoor.h"
#include "blockstate.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

using namespace std;

namespace {

// Column-vector 3x4 affine: v' = R * v + t. Product A*B applies B first.
struct Affine {
    float m[3][4] = {};

    static Affine Id() {
        Affine a;
        a.m[0][0] = a.m[1][1] = a.m[2][2] = 1.f;
        return a;
    }

    static Affine Translate(float x, float y, float z) {
        Affine a = Id();
        a.m[0][3] = x;
        a.m[1][3] = y;
        a.m[2][3] = z;
        return a;
    }

    // Right-handed about +Y through the origin:
    // x' = x cos + z sin, z' = -x sin + z cos.
    static Affine RotateY(float degrees) {
        const float r = degrees * (3.14159265358979323846f / 180.f);
        const float c = cos(r);
        const float s = sin(r);
        Affine a = Id();
        a.m[0][0] = c;
        a.m[0][2] = s;
        a.m[2][0] = -s;
        a.m[2][2] = c;
        return a;
    }

    Affine operator*(const Affine& b) const {
        Affine r;
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) {
                r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
            }
            r.m[i][3] = m[i][0] * b.m[0][3] + m[i][1] * b.m[1][3] + m[i][2] * b.m[2][3] + m[i][3];
        }
        return r;
    }

    void Apply(ModelData& model) const {
        for (size_t i = 0; i < model.vertices.size(); i += 3) {
            const float x = model.vertices[i];
            const float y = model.vertices[i + 1];
            const float z = model.vertices[i + 2];
            model.vertices[i] = m[0][0] * x + m[0][1] * y + m[0][2] * z + m[0][3];
            model.vertices[i + 1] = m[1][0] * x + m[1][1] * y + m[1][2] * z + m[1][3];
            model.vertices[i + 2] = m[2][0] * x + m[2][1] * y + m[2][2] * z + m[2][3];
        }
    }
};

unordered_map<string, string> ParseStates(const string& blockName) {
    unordered_map<string, string> states;
    size_t begin = blockName.find('[');
    size_t end = blockName.rfind(']');
    if (begin == string::npos || end == string::npos || begin >= end) return states;
    stringstream stream(blockName.substr(begin + 1, end - begin - 1));
    string pair;
    while (getline(stream, pair, ',')) {
        size_t separator = pair.find('=');
        if (separator == string::npos) separator = pair.find(':');
        if (separator != string::npos) {
            states[pair.substr(0, separator)] = pair.substr(separator + 1);
        }
    }
    return states;
}

string BlockId(const string& blockName) {
    string name = blockName;
    size_t bracket = name.find('[');
    if (bracket != string::npos) name = name.substr(0, bracket);
    size_t colon = name.find(':');
    if (colon != string::npos) name = name.substr(colon + 1);
    return name;
}

string State(const unordered_map<string, string>& states, const string& key, const string& fallback = "") {
    auto it = states.find(key);
    return it == states.end() ? fallback : it->second;
}

bool IsSlidingDoor(const string& id) {
    return id == "andesite_door" || id == "brass_door" || id == "copper_door" ||
        id == "train_door" || id == "framed_glass_door";
}

bool IsFoldingDoor(const string& id) {
    return id == "andesite_door" || id == "copper_door";
}

string Clockwise(const string& facing) {
    if (facing == "north") return "east";
    if (facing == "east") return "south";
    if (facing == "south") return "west";
    if (facing == "west") return "north";
    return facing;
}

string Opposite(const string& facing) {
    if (facing == "north") return "south";
    if (facing == "south") return "north";
    if (facing == "east") return "west";
    if (facing == "west") return "east";
    return facing;
}

void HorizontalVector(const string& facing, float& x, float& z) {
    x = 0.f;
    z = 0.f;
    if (facing == "east") x = 1.f;
    else if (facing == "west") x = -1.f;
    else if (facing == "south") z = 1.f;
    else if (facing == "north") z = -1.f;
}

// AngleHelper.horizontalAngle: south 0, west -90, north 180, east -270.
float HorizontalAngle(const string& facing) {
    if (facing == "south") return 0.f;
    if (facing == "west") return -90.f;
    if (facing == "north") return 180.f;
    if (facing == "east") return -270.f;
    return 0.f;
}

void DisableCulling(ModelData& model) {
    for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
}

void AppendModel(ModelData& dst, ModelData src) {
    if (src.faces.empty()) return;
    if (dst.faces.empty()) dst = std::move(src);
    else MergeModelsDirectly(dst, src);
}

ModelData LoadVariantModel(const string& ns, const string& id, const nlohmann::json& entry) {
    const nlohmann::json* obj = &entry;
    if (entry.is_array()) {
        if (entry.empty() || !entry[0].is_object()) return {};
        obj = &entry[0];
    }
    if (!obj->is_object() || !obj->contains("model")) return {};

    string modelPath = (*obj)["model"].get<string>();
    size_t colon = modelPath.find(':');
    if (colon != string::npos) modelPath = modelPath.substr(colon + 1);

    int rotationX = 0;
    int rotationY = 0;
    bool uvlock = false;
    if (obj->contains("x") && (*obj)["x"].is_number()) rotationX = (*obj)["x"].get<int>();
    if (obj->contains("y") && (*obj)["y"].is_number()) rotationY = (*obj)["y"].get<int>();
    if (obj->contains("uvlock") && (*obj)["uvlock"].is_boolean()) uvlock = (*obj)["uvlock"].get<bool>();
    rotationX = (rotationX % 360 + 360) % 360;
    rotationY = (rotationY % 360 + 360) % 360;

    ModelData model = ProcessModelJson(ns, modelPath, rotationX, rotationY, uvlock);
    RenameBlockMaterials(model, ns, id);
    return model;
}

// Closed, visible door half. Variant key properties are alphabetical.
ModelData LoadClosedHalf(const string& ns, const string& id, const string& facing,
    const string& hinge, const string& half) {
    nlohmann::json blockstate = GetBlockstateJson(ns, id);
    if (blockstate.is_null() || !blockstate.contains("variants")) return {};
    const string key = "facing=" + facing + ",half=" + half + ",hinge=" + hinge +
        ",open=false,visible=true";
    const auto& variants = blockstate["variants"];
    if (!variants.contains(key)) return {};
    return LoadVariantModel(ns, id, variants[key]);
}

ModelData LoadPartial(const string& ns, const string& id, const string& which) {
    ModelData model = ProcessModelJson(ns, "block/" + id + "/" + which, 0, 0, false);
    RenameBlockMaterials(model, ns, id);
    return model;
}

// PoseStack order: the first factor is applied last.
// T(0,-1/512,0) * T(F*value2/32) * centerRy(horizontalAngle(F.clockwise))
//   * (flip ? T(0,0,1) : I) * Ry(91*f*v*v)
//   * (!left ? T(0,0,f/2) * Ry(-181*f*v*v) : I) * (flip ? T(0,0,-0.5) : I)
Affine FoldTransform(const string& facing, bool flip, bool left, float v, float value2) {
    const float f = flip ? -1.f : 1.f;
    float faceX = 0.f, faceZ = 0.f;
    HorizontalVector(facing, faceX, faceZ);

    Affine transform = Affine::Translate(0.f, -1.f / 512.f, 0.f);
    transform = transform * Affine::Translate(faceX * value2 / 32.f, 0.f, faceZ * value2 / 32.f);
    transform = transform * Affine::Translate(0.5f, 0.5f, 0.5f);
    transform = transform * Affine::RotateY(HorizontalAngle(Clockwise(facing)));
    transform = transform * Affine::Translate(-0.5f, -0.5f, -0.5f);
    if (flip) transform = transform * Affine::Translate(0.f, 0.f, 1.f);
    transform = transform * Affine::RotateY(91.f * f * v * v);
    if (!left) {
        transform = transform * Affine::Translate(0.f, 0.f, f * 0.5f);
        transform = transform * Affine::RotateY(-181.f * f * v * v);
    }
    if (flip) transform = transform * Affine::Translate(0.f, 0.f, -0.5f);
    return transform;
}

ModelData BuildFoldingDoor(const string& ns, const string& id, const string& facing,
    const string& hinge, float v, float value2) {
    const bool flip = hinge == "right";
    ModelData result;
    for (bool left : { true, false }) {
        const string which = (left ^ flip) ? "fold_left" : "fold_right";
        ModelData partial = LoadPartial(ns, id, which);
        FoldTransform(facing, flip, left, v, value2).Apply(partial);
        AppendModel(result, std::move(partial));
    }
    return result;
}

ModelData BuildSlidingDoor(const string& ns, const string& id, const string& facing,
    const string& hinge, const string& movement, float v, float value2) {
    float moveX = 0.f, moveZ = 0.f, faceX = 0.f, faceZ = 0.f;
    HorizontalVector(movement, moveX, moveZ);
    HorizontalVector(facing, faceX, faceZ);
    const float slide = v * v * 13.f / 16.f;
    const float nudge = value2 / 32.f;
    const float offsetX = moveX * slide + faceX * nudge;
    const float offsetZ = moveZ * slide + faceZ * nudge;

    ModelData result;
    for (const string& half : { "lower", "upper" }) {
        ModelData part = LoadClosedHalf(ns, id, facing, hinge, half);
        // 1 - 1/512f, the renderer's upper-half lift (about 0.9980469).
        const float offsetY = half == "upper" ? (1.f - 1.f / 512.f) : 0.f;
        ApplyDoublePositionOffset(part, offsetX, offsetY, offsetZ);
        AppendModel(result, std::move(part));
    }
    return result;
}

} // namespace

bool ApplyCreateSlidingDoor(const string& ns, const string& blockName, ModelData& model) {
    if (ns != "create") return false;
    const string id = BlockId(blockName);
    if (!IsSlidingDoor(id)) return false;

    const auto states = ParseStates(blockName);
    if (State(states, "visible") != "false") return false;

    const string half = State(states, "half");
    if (half == "upper") {
        model = ModelData{};
        return true;
    }
    if (half != "lower") return false;

    const string facing = State(states, "facing", "north");
    const string hinge = State(states, "hinge", "left");
    const float animation = State(states, "open") == "true" ? 1.f : 0.f;
    const float value2 = clamp(animation * 10.f, 0.f, 1.f);

    string movement = Clockwise(facing);
    if (hinge == "left") movement = Opposite(movement);

    ModelData built = IsFoldingDoor(id)
        ? BuildFoldingDoor(ns, id, facing, hinge, animation, value2)
        : BuildSlidingDoor(ns, id, facing, hinge, movement, animation, value2);
    if (built.vertices.empty()) return false;

    DisableCulling(built);
    model = std::move(built);
    return true;
}
