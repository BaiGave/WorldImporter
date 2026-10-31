#include "EntityModel.h"
#include "EntityModelData.inc"
#include <algorithm>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kLayerCount = static_cast<int>(sizeof(kEmLayers) / sizeof(kEmLayers[0]));
int FindLayer(const std::string& family, const std::string& id) {
    for (int i = 0; i < kLayerCount; ++i)
        if (family == kEmLayers[i].family && id == kEmLayers[i].id) return i;
    return -1;
}
}

EntityAffine EntityAffine::operator*(const EntityAffine& o) const {
    EntityAffine r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = (j == 3) ? m[i][3] : 0.0;
            for (int k = 0; k < 3; ++k) s += m[i][k] * o.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}
void EntityAffine::Apply(const double p[3], double out[3]) const {
    for (int i = 0; i < 3; ++i) out[i] = m[i][0] * p[0] + m[i][1] * p[1] + m[i][2] * p[2] + m[i][3];
}
double EntityAffine::Det() const {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}
EntityAffine EntityAffine::T(double x, double y, double z) { EntityAffine a; a.m[0][3] = x; a.m[1][3] = y; a.m[2][3] = z; return a; }
EntityAffine EntityAffine::S(double x, double y, double z) { EntityAffine a; a.m[0][0] = x; a.m[1][1] = y; a.m[2][2] = z; return a; }
EntityAffine EntityAffine::RX(double r) { EntityAffine a; double c = std::cos(r), s = std::sin(r); a.m[1][1] = c; a.m[1][2] = -s; a.m[2][1] = s; a.m[2][2] = c; return a; }
EntityAffine EntityAffine::RY(double r) { EntityAffine a; double c = std::cos(r), s = std::sin(r); a.m[0][0] = c; a.m[0][2] = s; a.m[2][0] = -s; a.m[2][2] = c; return a; }
EntityAffine EntityAffine::RZ(double r) { EntityAffine a; double c = std::cos(r), s = std::sin(r); a.m[0][0] = c; a.m[0][1] = -s; a.m[1][0] = s; a.m[1][1] = c; return a; }
EntityAffine EntityAffine::RYDeg(double d) { return RY(d * kPi / 180.0); }
EntityAffine EntityAffine::RXDeg(double d) { return RX(d * kPi / 180.0); }
EntityAffine EntityAffine::RZDeg(double d) { return RZ(d * kPi / 180.0); }

bool EntityModel::Exists(const std::string& family, const std::string& id) { return FindLayer(family, id) >= 0; }

EntityModel::EntityModel(const std::string& family, const std::string& id) {
    layer_ = FindLayer(family, id);
    if (layer_ < 0) return;
    const EmLayer& L = kEmLayers[layer_];
    partStart_ = L.partStart;
    for (int i = 0; i < L.partCount; ++i) {
        const EmPart& p = kEmParts[L.partStart + i];
        names_.push_back(p.name);
        parents_.push_back(p.parent);
        std::array<float, 9> pose;
        for (int k = 0; k < 9; ++k) pose[k] = p.pose[k];
        poses_.push_back(pose);
        visible_.push_back(p.visible);
    }
}

int EntityModel::Find(const std::string& name) const {
    for (size_t i = 0; i < names_.size(); ++i) if (names_[i] == name) return static_cast<int>(i);
    return -1;
}
int EntityModel::Parent(int part) const { return parents_[part]; }
int EntityModel::TopLevel(int part) const {
    if (part <= 0) return -1;
    while (parents_[part] > 0) part = parents_[part];
    return part;
}
bool EntityModel::InSubtree(int part, int ancestor) const {
    for (int p = part; p >= 0; p = parents_[p]) if (p == ancestor) return true;
    return false;
}
void EntityModel::SetRot(const std::string& part, float x, float y, float z) {
    int i = Find(part); if (i < 0) return;
    poses_[i][3] = x; poses_[i][4] = y; poses_[i][5] = z;
}
void EntityModel::AddRot(const std::string& part, float x, float y, float z) {
    int i = Find(part); if (i < 0) return;
    poses_[i][3] += x; poses_[i][4] += y; poses_[i][5] += z;
}

void EntityModel::SetPose(const std::string& part, float x, float y, float z) {
    int i = Find(part); if (i < 0) return;
    poses_[i][0] = x; poses_[i][1] = y; poses_[i][2] = z;
}
void EntityModel::HideAllBut(const std::vector<std::string>& keep) {
    for (int i = 1; i < PartCount(); ++i) {
        if (parents_[i] != 0) continue;  // 只处理 root 的直接子部件
        bool k = false;
        for (const auto& n : keep) if (names_[i] == n) k = true;
        visible_[i] = k;
    }
}
void EntityModel::CopyPoseFrom(const EntityModel& src, const std::vector<std::string>& parts, bool withPosition) {
    for (const auto& n : parts) {
        int a = Find(n), b = src.Find(n);
        if (a < 0 || b < 0) continue;
        const float* p = src.Pose(b);
        for (int k = withPosition ? 0 : 3; k < 6; ++k) poses_[a][k] = p[k];
    }
}
namespace {
EntityAffine LocalPose(const std::array<float, 9>& p) {
    // ModelPart.translateAndRotate: translate(xyz/16) * rotationZYX(z, y, x) * scale
    EntityAffine m = EntityAffine::T(p[0] / 16.0, p[1] / 16.0, p[2] / 16.0) *
                     EntityAffine::RZ(p[5]) * EntityAffine::RY(p[4]) * EntityAffine::RX(p[3]);
    if (p[6] != 1.0f || p[7] != 1.0f || p[8] != 1.0f) m = m * EntityAffine::S(p[6], p[7], p[8]);
    return m;
}
}
EntityAffine EntityModel::PartTransform(int part, const EntityAffine& base,
                                        const TopPre& pre) const {
    if (part < 0 || part >= PartCount()) return base;
    std::vector<int> chain;
    for (int p = part; p >= 0; p = parents_[p]) chain.push_back(p);
    std::reverse(chain.begin(), chain.end());   // root ... part
    EntityAffine m = base;
    if (chain.size() > 1 && pre) m = m * pre(names_[chain[1]]);
    for (int p : chain) m = m * LocalPose(poses_[p]);
    return m;
}

void EntityModel::Emit(ModelData& out, const EntityAffine& base,
                       const std::function<int(int)>& material,
                       const TopPre& pre) const {
    if (layer_ < 0) return;
    const int n = PartCount();
    std::vector<EntityAffine> local(n), world(n);
    std::vector<bool> shown(n);
    for (int i = 0; i < n; ++i) {
        local[i] = LocalPose(poses_[i]);
    }
    // parts are stored in pre-order: parent index < child index
    for (int i = 0; i < n; ++i) {
        int par = parents_[i];
        if (par < 0) { world[i] = base * local[i]; shown[i] = visible_[i]; continue; }
        shown[i] = shown[par] && visible_[i];
        if (par == 0 && pre) world[i] = base * pre(names_[i]) * local[0] * local[i];
        else world[i] = world[par] * local[i];
    }
    for (int i = 0; i < n; ++i) {
        const EmPart& P = kEmParts[partStart_ + i];
        if (!shown[i] || P.polyCount == 0) continue;
        int mat = material(i);
        if (mat < 0) continue;
        const EntityAffine& W = world[i];
        const bool flip = W.Det() < 0.0;
        for (int q = 0; q < P.polyCount; ++q) {
            const float* poly = kEmPolys[P.polyStart + q];
            int vs = static_cast<int>(out.vertices.size() / 3);
            int us = static_cast<int>(out.uvCoordinates.size() / 2);
            for (int j = 0; j < 4; ++j) {
                int k = flip ? 3 - j : j;  // 原版顶点逆时针(朝外); 镜像矩阵时翻转绕序
                const float* v = poly + k * 5;
                double p[3] = { v[0] / 16.0, v[1] / 16.0, v[2] / 16.0 }, w[3];
                W.Apply(p, w);
                out.vertices.push_back(static_cast<float>(w[0]));
                out.vertices.push_back(static_cast<float>(w[1]));
                out.vertices.push_back(static_cast<float>(w[2]));
                out.uvCoordinates.push_back(v[3]);
                out.uvCoordinates.push_back(1.0f - v[4]);
            }
            Face f;
            f.vertexIndices = { vs, vs + 1, vs + 2, vs + 3 };
            f.uvIndices = { us, us + 1, us + 2, us + 3 };
            f.materialIndex = mat;
            f.faceDirection = DO_NOT_CULL;
            out.faces.push_back(f);
        }
    }
}
