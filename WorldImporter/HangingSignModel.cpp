#include "HangingSignModel.h"
#include "EntityCube.h"
#include <sstream>

using namespace std;
using namespace entitycube;

namespace {
    float DirToYRot(const string& f) {  // Direction.toYRot
        if (f == "west") return 90.0f;
        if (f == "north") return 180.0f;
        if (f == "east") return 270.0f;
        return 0.0f;  // south
    }

    // 父级 normalChains 为 ZERO, 四个链片是它的子部件
    void AddNormalChains(ModelData& model, int mat, const Aff& root) {
        const float q = kPi * 0.25f;  // ±pi/4
        const float W = 64.0f, H = 32.0f;
        AddCube(model, mat, root * Pose(-5, -6, 0, 0, -q, 0), 0, 6, -1.5f, 0, 0, 3, 6, 0, 0, W, H);  // chainL1
        AddCube(model, mat, root * Pose(-5, -6, 0, 0,  q, 0), 6, 6, -1.5f, 0, 0, 3, 6, 0, 0, W, H);  // chainL2
        AddCube(model, mat, root * Pose( 5, -6, 0, 0, -q, 0), 0, 6, -1.5f, 0, 0, 3, 6, 0, 0, W, H);  // chainR1
        AddCube(model, mat, root * Pose( 5, -6, 0, 0,  q, 0), 6, 6, -1.5f, 0, 0, 3, 6, 0, 0, W, H);  // chainR2
    }
}

ModelData GenerateHangingSignModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // <wood>_wall_hanging_sign 必须先于 <wood>_hanging_sign(前者也以后者为后缀)
    const string wallSuffix = "_wall_hanging_sign";
    const string ceilSuffix = "_hanging_sign";
    bool wall = false;
    string wood;
    if (blockID.size() > wallSuffix.size() && blockID.ends_with(wallSuffix)) {
        wall = true;
        wood = blockID.substr(0, blockID.size() - wallSuffix.size());
    } else if (blockID.size() > ceilSuffix.size() && blockID.ends_with(ceilSuffix)) {
        wood = blockID.substr(0, blockID.size() - ceilSuffix.size());
    } else {
        return ModelData();
    }

    int rotation = 0;
    string facing = "north";
    bool attached = false;
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1);
        if (!statePart.empty() && statePart.back() == ']') statePart.pop_back();
        stringstream ss(statePart);
        string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "rotation") { try { rotation = stoi(v); } catch (...) {} }
            else if (k == "facing") facing = v;
            else if (k == "attached") attached = (v == "true");
        }
    }

    ModelData model;
    // 贴图 64x32: textures/entity/signs/hanging/<wood>.png
    int mat = AddMaterial(model, ns, ns + ":" + blockID, "entity/signs/hanging/" + wood);
    if (mat < 0) return ModelData();

    // HangingSignRenderer: T(.5,.9375,.5) * RY(-angle) * T(0,-.3125,0) * S(1,-1,-1)
    // 天花板 angle = rotation*22.5; 墙上 angle = facing.toYRot
    float angle = wall ? DirToYRot(facing) : rotation * 22.5f;
    Aff root = T(0.5f, 0.9375f, 0.5f) * RY(Deg(-angle)) * T(0.0f, -0.3125f, 0.0f) * S(1.0f, -1.0f, -1.0f);
    const float W = 64.0f, H = 32.0f;

    // board 始终可见
    AddCube(model, mat, root, 0, 12, -7, 0, -1, 14, 10, 2, 0, W, H);
    if (wall) {
        // 墙上: plank + normalChains
        AddCube(model, mat, root, 0, 0, -8, -6, -2, 16, 2, 4, 0, W, H);
        AddNormalChains(model, mat, root);
    } else if (attached) {
        // 天花板且 attached: 一根竖链
        AddCube(model, mat, root, 14, 6, -6, -6, 0, 12, 6, 0, 0, W, H);
    } else {
        // 天花板未 attached: 左右两对链
        AddNormalChains(model, mat, root);
    }
    return model;
}
