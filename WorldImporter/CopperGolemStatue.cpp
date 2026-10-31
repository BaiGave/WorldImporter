#include "CopperGolemStatue.h"
#include "texture.h"
#include <cmath>
#include <unordered_map>

using namespace std;

// 几何按原版 CopperGolemModel 的四个 LayerDefinition(站立/奔跑/坐下/星形)移植,
// 变换按 CopperGolemStatueBlockRenderer: translation(0.5,0,0.5) * rotY(-facing.opposite.toYRot),
// CopperGolemStatueModel.setupAnim: root.y = 0, root.zRot = PI。单位: 方块(0..1)。
namespace {
    struct Aff {  // 3x4 仿射矩阵
        float m[3][4] = { {1,0,0,0},{0,1,0,0},{0,0,1,0} };
        Aff operator*(const Aff& o) const {
            Aff r;
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 4; ++j) {
                float s = (j == 3) ? m[i][3] : 0.0f;
                for (int k = 0; k < 3; ++k) s += m[i][k] * o.m[k][j];
                r.m[i][j] = s;
            }
            return r;
        }
        void Apply(const float p[3], float out[3], bool dir = false) const {
            for (int i = 0; i < 3; ++i)
                out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + (dir ? 0.0f : m[i][3]);
        }
    };
    Aff AffT(float x, float y, float z) { Aff a; a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z; return a; }
    Aff AffRX(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c; return a; }
    Aff AffRY(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c; return a; }
    Aff AffRZ(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c; return a; }
    // ModelPart.translateAndRotate: translate(xyz/16) * rotationZYX(z, y, x)
    Aff Pose(float x, float y, float z, float rx = 0, float ry = 0, float rz = 0) {
        return AffT(x / 16.0f, y / 16.0f, z / 16.0f) * AffRZ(rz) * AffRY(ry) * AffRX(rx);
    }

    // 原版 ModelPart.Cube(顶点 / UV / 面), 贴图 64x64
    void AddCube(ModelData& model, const Aff& pose, int texU, int texV,
                 float x, float y, float z, float w, float h, float d, float grow = 0.0f) {
        const float texW = 64.0f, texH = 64.0f;
        float x0 = x - grow, y0 = y - grow, z0 = z - grow, x1 = x + w + grow, y1 = y + h + grow, z1 = z + d + grow;
        const float P[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},
                                {x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
        float u0 = (float)texU, u1 = texU + d, u2 = texU + d + w, u22 = texU + d + w + w, u3 = texU + d + w + d, u4 = texU + d + w + d + w;
        float v0 = (float)texV, v1 = texV + d, v2 = texV + d + h;
        struct Q { int a, b, c, e; float ua, va, ub, vb; float n[3]; };
        const Q qs[6] = {
            {5, 4, 0, 1, u1, v0, u2, v1,  {0,-1,0}},
            {2, 3, 7, 6, u2, v1, u22, v0, {0,1,0}},
            {0, 4, 7, 3, u0, v1, u1, v2,  {-1,0,0}},
            {1, 0, 3, 2, u1, v1, u2, v2,  {0,0,-1}},
            {5, 1, 2, 6, u2, v1, u3, v2,  {1,0,0}},
            {4, 5, 6, 7, u3, v1, u4, v2,  {0,0,1}},
        };
        for (const Q& q : qs) {
            int idx[4] = { q.a, q.b, q.c, q.e };
            float uv[4][2] = { {q.ub, q.va}, {q.ua, q.va}, {q.ua, q.vb}, {q.ub, q.vb} };
            float W[4][3];
            for (int i = 0; i < 4; ++i) {
                float p[3] = { P[idx[i]][0] / 16.0f, P[idx[i]][1] / 16.0f, P[idx[i]][2] / 16.0f };
                pose.Apply(p, W[i]);
            }
            float e1[3], e2[3], cr[3];
            for (int k = 0; k < 3; ++k) { e1[k] = W[1][k] - W[0][k]; e2[k] = W[2][k] - W[0][k]; }
            cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
            // 绕序与变换后的面法线一致(root 的 zRot=PI 等)
            float n[3]; pose.Apply(q.n, n, true);
            bool flip = cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2] < 0.0f;
            int vs = (int)model.vertices.size() / 3, us = (int)model.uvCoordinates.size() / 2;
            for (int j = 0; j < 4; ++j) {
                int i = flip ? 3 - j : j;
                for (int k = 0; k < 3; ++k) model.vertices.push_back(W[i][k]);
                model.uvCoordinates.push_back(uv[i][0] / texW);
                model.uvCoordinates.push_back(1.0f - uv[i][1] / texH);
            }
            Face face;
            face.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            face.uvIndices = { us, us + 1, us + 2, us + 3 };
            face.materialIndex = 0;
            face.faceDirection = DO_NOT_CULL;
            model.faces.push_back(face);
        }
    }

    unordered_map<string, string> States(const string& blockName) {
        unordered_map<string, string> out;
        size_t lb = blockName.find('['), rb = blockName.rfind(']');
        if (lb == string::npos || rb == string::npos || rb <= lb) return out;
        string s = blockName.substr(lb + 1, rb - lb - 1);
        size_t pos = 0;
        while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            string kv = s.substr(pos, comma == string::npos ? string::npos : comma - pos);
            size_t eq = kv.find('=');
            if (eq != string::npos) out[kv.substr(0, eq)] = kv.substr(eq + 1);
            if (comma == string::npos) break;
            pos = comma + 1;
        }
        return out;
    }

    const float kPi = 3.14159265358979f;

    // CopperGolemModel.createBodyLayer
    void Standing(ModelData& m, const Aff& root) {
        Aff body = root * Pose(0, -5, 0);
        AddCube(m, body, 0, 15, -4, -6, -3, 8, 6, 6);
        Aff head = body * Pose(0, -6, 0);
        AddCube(m, head, 0, 0, -4, -5, -5, 8, 5, 10, 0.015f);
        AddCube(m, head, 56, 0, -1, -2, -6, 2, 3, 2);
        AddCube(m, head, 37, 8, -1, -9, -1, 2, 4, 2, -0.015f);
        AddCube(m, head, 37, 0, -2, -13, -2, 4, 4, 4, -0.015f);
        AddCube(m, body * Pose(-4, -6, 0), 36, 16, -3, -1, -2, 3, 10, 4);
        AddCube(m, body * Pose(4, -6, 0), 50, 16, 0, -1, -2, 3, 10, 4);
        AddCube(m, root * Pose(0, -5, 0), 0, 27, -4, 0, -2, 4, 5, 4);
        AddCube(m, root * Pose(0, -5, 0), 16, 27, 0, 0, -2, 4, 5, 4);
    }
    // CopperGolemModel.createRunningPoseBodyLayer
    void Running(ModelData& m, const Aff& root) {
        Aff body = root * Pose(-1.064f, -5, 0);
        AddCube(m, body * Pose(1.1f, 0.1f, 0.7f, 0.1204f, -0.0064f, -0.0779f), 0, 15, -4.02f, -6.116f, -3.5f, 8, 6, 6);
        Aff head = body * Pose(0.7f, -5.6f, -1.8f);
        AddCube(m, head, 0, 0, -4, -5.1f, -5, 8, 5, 10);
        AddCube(m, head, 56, 0, -1.02f, -2.1f, -6, 2, 3, 2);
        AddCube(m, head, 37, 8, -1.02f, -9.1f, -1, 2, 4, 2, -0.015f);
        AddCube(m, head, 37, 0, -2, -13.1f, -2, 4, 4, 4, -0.015f);
        AddCube(m, body * Pose(-4, -6, 0) * Pose(0.7f, -0.248f, -1.62f, 1.0036f, 0, 0), 36, 16, -3.052f, -1.11f, -2.036f, 3, 10, 4);
        AddCube(m, body * Pose(4, -6, 0) * Pose(0.732f, 0, 0, -0.8715f, -0.0535f, -0.0449f), 50, 16, 0.032f, -1.1f, -2, 3, 10, 4);
        AddCube(m, root * Pose(-3.064f, -5, 0) * Pose(1.048f, 0, -0.9f, -0.8727f, 0, 0), 0, 27, -1.856f, -0.1f, -1.09f, 4, 5, 4);
        AddCube(m, root * Pose(0.936f, -5, 0) * Pose(1, 0, 0, 0.7854f, 0, 0), 16, 27, -2.088f, -0.1f, -2, 4, 5, 4);
    }
    // CopperGolemModel.createSittingPoseBodyLayer
    void Sitting(ModelData& m, const Aff& root) {
        Aff body = root * Pose(0, -3, 2.325f);
        AddCube(m, body, 3, 19, -3, -4, -4.525f, 6, 1, 6);
        AddCube(m, body, 0, 15, -4, -3, -3.525f, 8, 6, 6);
        AddCube(m, body * Pose(0, -1, -4.325f, 0, 0, -3.1416f), 3, 18, -4, -3, -2.2f, 8, 6, 3);
        Aff head = body * Pose(0, -6, -0.2f);
        AddCube(m, head, 37, 8, -1, -7, -3.3f, 2, 4, 2, -0.015f);
        AddCube(m, head, 37, 0, -2, -11, -4.3f, 4, 4, 4, -0.015f);
        AddCube(m, head, 0, 0, -4, -3, -7.325f, 8, 5, 10);
        AddCube(m, head, 56, 0, -1, 0, -8.325f, 2, 3, 2);
        AddCube(m, body * Pose(-4, -5.6f, -1.8f, 0.4363f, 0, 0) * Pose(0, 0.0893f, 0.1198f, -1.0472f, 0, 0), 36, 16, -3.075f, -0.9733f, -1.9966f, 3, 10, 4);
        AddCube(m, body * Pose(4, -5.6f, -1.7f, 0.4363f, 0, 0) * Pose(0, -0.0015f, -0.0808f, -1.0472f, 0, 0), 50, 16, 0.075f, -1.0443f, -1.8997f, 3, 10, 4);
        AddCube(m, root * Pose(-2.1f, -2.1f, -2.075f) * Pose(0.05f, -1.9f, 1.075f, -1.5708f, 0, 0), 0, 27, -2, 0.975f, 0, 4, 5, 4);
        AddCube(m, root * Pose(2, -2, -2.075f) * Pose(0.05f, -2, 1.075f, -1.5708f, 0, 0), 16, 27, -2, 0.975f, 0, 4, 5, 4);
    }
    // CopperGolemModel.createStarPoseBodyLayer
    void Star(ModelData& m, const Aff& root) {
        Aff body = root * Pose(0, -5, 0);
        AddCube(m, body, 0, 15, -4, -6, -3, 8, 6, 6);
        Aff head = body * Pose(0, -6, 0);
        AddCube(m, head, 0, 0, -4, -5, -5, 8, 5, 10);
        AddCube(m, head, 56, 0, -1, -2, -6, 2, 3, 2);
        AddCube(m, head, 37, 8, -1, -9, -1, 2, 4, 2, -0.015f);
        AddCube(m, head, 37, 0, -2, -13, -2, 4, 4, 4, -0.015f);
        AddCube(m, body * Pose(-4, -6, 0) * Pose(1, 1, 0, 0, 0, 1.9199f), 36, 16, -1.5f, -5, -2, 3, 10, 4);
        AddCube(m, body * Pose(4, -6, 0) * Pose(-1, 1, 0, 0, 0, -1.9199f), 50, 16, -1.5f, -5, -2, 3, 10, 4);
        AddCube(m, root * Pose(-3, -5, 0) * Pose(0.35f, 2, 0.01f, 0, 0, 0.2618f), 0, 27, -2, -2.5f, -2, 4, 5, 4);
        AddCube(m, root * Pose(1, -5, 0) * Pose(1.65f, 2, 0, 0, 0, -0.2618f), 16, 27, -2, -2.5f, -2, 4, 5, 4);
    }
}

ModelData GenerateCopperGolemStatueModel(const string& blockName) {
    string ns = "minecraft", name = blockName;
    size_t colon = name.find(':');
    if (colon != string::npos) { ns = name.substr(0, colon); name = name.substr(colon + 1); }
    string blockID = name.substr(0, name.find('['));
    const string suffix = "copper_golem_statue";
    if (ns != "minecraft" || blockID.size() < suffix.size() ||
        blockID.compare(blockID.size() - suffix.size(), suffix.size(), suffix) != 0) return ModelData();
    string prefix = blockID.substr(0, blockID.size() - suffix.size());
    if (prefix.rfind("waxed_", 0) == 0) prefix = prefix.substr(6);
    string tex;  // CopperGolemOxidationLevels
    if (prefix.empty()) tex = "entity/copper_golem/copper_golem";
    else if (prefix == "exposed_" || prefix == "weathered_" || prefix == "oxidized_")
        tex = "entity/copper_golem/copper_golem_" + prefix.substr(0, prefix.size() - 1);
    else return ModelData();

    string dir = "textures";
    if (!SaveTextureToFile(ns, tex, dir)) return ModelData();
    string saved = "textures/" + ns + "/" + tex + ".png";
    RegisterTexture(ns, tex, saved);

    auto st = States(blockName);
    string facing = st.count("facing") ? st["facing"] : "north";
    string pose = st.count("copper_golem_pose") ? st["copper_golem_pose"] : "standing";
    // -facing.getOpposite().toYRot()
    float oppYRot = facing == "north" ? 0.0f : facing == "east" ? 90.0f : facing == "south" ? 180.0f : facing == "west" ? 270.0f : 0.0f;
    Aff root = AffT(0.5f, 0.0f, 0.5f) * AffRY(-oppYRot * kPi / 180.0f) * Pose(0, 0, 0, 0, 0, kPi);

    ModelData model;
    Material mat;
    mat.name = ns + ":" + blockID;
    mat.texturePath = saved;
    mat.tintIndex = -1;
    model.materials.push_back(mat);
    if (pose == "running") Running(model, root);
    else if (pose == "sitting") Sitting(model, root);
    else if (pose == "star") Star(model, root);
    else Standing(model, root);
    return model;
}
