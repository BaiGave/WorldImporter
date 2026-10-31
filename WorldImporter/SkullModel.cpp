#include "SkullModel.h"
#include "EntityCube.h"
#include <sstream>

using namespace std;
using namespace entitycube;

namespace {
    struct SkullType { const char* prefix; const char* tex[2]; int texW, texH; int kind; };  // kind: 0 普通 1 带帽子层 2 龙 3 猪灵
    const SkullType kSkulls[] = {
        {"skeleton",         {"entity/skeleton/skeleton", nullptr},               64, 32, 0},
        {"wither_skeleton",  {"entity/skeleton/wither_skeleton", nullptr},        64, 32, 0},
        {"creeper",          {"entity/creeper/creeper", nullptr},                 64, 32, 0},
        {"zombie",           {"entity/zombie/zombie", nullptr},                   64, 64, 1},
        {"player",           {"entity/player/wide/steve", "entity/steve"},        64, 64, 1},  // 1.19.3 前为 entity/steve
        {"dragon",           {"entity/enderdragon/dragon", nullptr},             256, 256, 2},
        {"piglin",           {"entity/piglin/piglin", nullptr},                   64, 64, 3},
    };
    float DirToYRot(const string& f) {  // Direction.toYRot
        if (f == "west") return 90.0f;
        if (f == "north") return 180.0f;
        if (f == "east") return 270.0f;
        return 0.0f;
    }
}

ModelData GenerateSkullModel(const string& blockName) {
    string ns = "minecraft", processed = blockName;
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) { ns = processed.substr(0, colonPos); processed = processed.substr(colonPos + 1); }
    if (ns != "minecraft") return ModelData();
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // <type>_skull / <type>_wall_skull / <type>_head / <type>_wall_head
    const SkullType* type = nullptr;
    bool wall = false;
    for (const SkullType& t : kSkulls) {
        string p = t.prefix;
        if (blockID == p + "_skull" || blockID == p + "_head") { type = &t; break; }
        if (blockID == p + "_wall_skull" || blockID == p + "_wall_head") { type = &t; wall = true; break; }
    }
    if (!type) return ModelData();

    int rotation = 0; string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1);
        if (!statePart.empty() && statePart.back() == ']') statePart.pop_back();
        stringstream ss(statePart); string pair;
        while (getline(ss, pair, ',')) {
            size_t eq = pair.find('=');
            if (eq == string::npos) continue;
            string k = pair.substr(0, eq), v = pair.substr(eq + 1);
            if (k == "rotation") { try { rotation = stoi(v); } catch (...) {} }
            else if (k == "facing") facing = v;
        }
    }

    ModelData model;
    int mat = -1;
    for (const char* tex : type->tex) {
        if (!tex) continue;
        mat = AddMaterial(model, ns, ns + ":" + blockID, tex);
        if (mat >= 0) break;
    }
    if (mat < 0) return ModelData();

    // SkullBlockRenderer: 地面 T(.5,0,.5) RY(-rot*22.5) S(-1,-1,1);
    // 墙上 T(.5-stepX/4, .25, .5-stepZ/4) RY(-opposite.toYRot) S(-1,-1,1)
    Aff root;
    if (wall) {
        float sx = 0, sz = 0;
        if (facing == "north") sz = -1; else if (facing == "south") sz = 1;
        else if (facing == "west") sx = -1; else if (facing == "east") sx = 1;
        float opp = DirToYRot(facing) + 180.0f;
        root = T(0.5f - sx * 0.25f, 0.25f, 0.5f - sz * 0.25f) * RY(Deg(-opp)) * S(-1, -1, 1);
    } else {
        root = T(0.5f, 0.0f, 0.5f) * RY(Deg(-rotation * 22.5f)) * S(-1, -1, 1);
    }
    const float W = (float)type->texW, H = (float)type->texH;

    switch (type->kind) {
    case 0:  // SkullModel.createMobHeadLayer
        AddCube(model, mat, root, 0, 0, -4, -8, -4, 8, 8, 8, 0, W, H);
        break;
    case 1:  // SkullModel.createHumanoidHeadLayer: 头 + 帽子层(外扩 0.25)
        AddCube(model, mat, root, 0, 0, -4, -8, -4, 8, 8, 8, 0, W, H);
        AddCube(model, mat, root, 32, 0, -4, -8, -4, 8, 8, 8, 0.25f, W, H);
        break;
    case 2: {  // DragonHeadModel: head offset(0,-7.986666,0) scaled(0.75), jaw offset(0,4,-8) xRot=0.2
        Aff head = root * T(0, -7.986666f * 0.75f / 16.0f, 0) * S(0.75f, 0.75f, 0.75f);
        AddCube(model, mat, head, 176, 44, -6, -1, -24, 12, 5, 16, 0, W, H);
        AddCube(model, mat, head, 112, 30, -8, -8, -10, 16, 16, 16, 0, W, H);
        AddCube(model, mat, head, 0, 0, -5, -12, -4, 2, 4, 6, 0, W, H, true);
        AddCube(model, mat, head, 112, 0, -5, -3, -22, 2, 2, 4, 0, W, H, true);
        AddCube(model, mat, head, 0, 0, 3, -12, -4, 2, 4, 6, 0, W, H);
        AddCube(model, mat, head, 112, 0, 3, -3, -22, 2, 2, 4, 0, W, H);
        AddCube(model, mat, head * Pose(0, 4, -8, 0.2f), 176, 65, -6, 0, -16, 12, 4, 16, 0, W, H);
        break;
    }
    case 3:  // PiglinHeadModel(PiglinModel.addHead), 耳朵 zRot 取 setupAnim(0) 的 -/+0.7
        AddCube(model, mat, root, 0, 0, -5, -8, -4, 10, 8, 8, 0, W, H);
        AddCube(model, mat, root, 31, 1, -2, -4, -5, 4, 4, 1, 0, W, H);
        AddCube(model, mat, root, 2, 4, 2, -2, -5, 1, 2, 1, 0, W, H);
        AddCube(model, mat, root, 2, 0, -3, -2, -5, 1, 2, 1, 0, W, H);
        AddCube(model, mat, root * Pose(4.5f, -6, 0, 0, 0, -0.7f), 51, 6, 0, 0, -2, 1, 5, 4, 0, W, H);
        AddCube(model, mat, root * Pose(-4.5f, -6, 0, 0, 0, 0.7f), 39, 6, -1, 0, -2, 1, 5, 4, 0, W, H);
        break;
    }
    return model;
}
