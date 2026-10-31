// EntityCube.h: 原版实体模型(ModelPart.Cube + PartPose)移植工具, 供实体渲染方块使用
#ifndef ENTITY_CUBE_H
#define ENTITY_CUBE_H

#include "model.h"
#include "texture.h"
#include <cmath>
#include <string>

namespace entitycube {

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
inline Aff T(float x, float y, float z) { Aff a; a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z; return a; }
inline Aff S(float x, float y, float z) { Aff a; a.m[0][0] = x; a.m[1][1] = y; a.m[2][2] = z; return a; }
inline Aff RX(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c; return a; }
inline Aff RY(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c; return a; }
inline Aff RZ(float r) { Aff a; float c = cosf(r), s = sinf(r); a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c; return a; }
// ModelPart.translateAndRotate: translate(xyz/16) * rotationZYX(z, y, x)
inline Aff Pose(float x, float y, float z, float rx = 0, float ry = 0, float rz = 0) {
    return T(x / 16.0f, y / 16.0f, z / 16.0f) * RZ(rz) * RY(ry) * RX(rx);
}
constexpr float kPi = 3.14159265358979f;
inline float Deg(float d) { return d * kPi / 180.0f; }

// 原版 ModelPart.Cube(含 mirror)。pose 为部件到方块空间的变换(输入为像素坐标/16)
inline void AddCube(ModelData& model, int mat, const Aff& pose, int texU, int texV,
                    float x, float y, float z, float w, float h, float d, float grow,
                    float texW, float texH, bool mirror = false) {
    float x0 = x - grow, y0 = y - grow, z0 = z - grow, x1 = x + w + grow, y1 = y + h + grow, z1 = z + d + grow;
    if (mirror) { float t = x0; x0 = x1; x1 = t; }
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
        if (fabsf(cr[0]) + fabsf(cr[1]) + fabsf(cr[2]) < 1e-9f) {  // 零厚度平面的侧面
            for (int k = 0; k < 3; ++k) e2[k] = W[3][k] - W[0][k];
            cr[0] = e1[1] * e2[2] - e1[2] * e2[1]; cr[1] = e1[2] * e2[0] - e1[0] * e2[2]; cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
            if (fabsf(cr[0]) + fabsf(cr[1]) + fabsf(cr[2]) < 1e-9f) continue;
        }
        // 绕序与变换后的外法线一致(镜像 cube 的 x 法线取反, 变换本身也可能含镜像)
        float qn[3] = { mirror ? -q.n[0] : q.n[0], q.n[1], q.n[2] };
        float n[3]; pose.Apply(qn, n, true);
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
        face.materialIndex = mat;
        face.faceDirection = DO_NOT_CULL;
        model.faces.push_back(face);
    }
}

// 注册实体贴图为材质, 返回材质下标; 贴图不存在返回 -1
inline int AddMaterial(ModelData& model, const std::string& ns, const std::string& name, const std::string& texPath) {
    std::string dir = "textures";
    if (!SaveTextureToFile(ns, texPath, dir)) return -1;
    std::string saved = "textures/" + ns + "/" + texPath + ".png";
    RegisterTexture(ns, texPath, saved);
    Material m;
    m.name = name;
    m.texturePath = saved;
    m.tintIndex = -1;
    model.materials.push_back(m);
    return (int)model.materials.size() - 1;
}

}  // namespace entitycube

#endif  // ENTITY_CUBE_H
