#ifndef ENTITY_MODEL_H
#define ENTITY_MODEL_H

// 原版实体模型(ModelPart 树)的运行时实例: 几何数据来自 EntityModelData.inc
// (tools/entity_models 从原版客户端 LayerDefinitions 自动导出), 姿态按原版 setupAnim 手动设置。

#include "model.h"
#include <functional>
#include <string>
#include <vector>

// 3x4 仿射矩阵 (PoseStack 的 Matrix4f 等价物, 双精度)
struct EntityAffine {
    double m[3][4] = { {1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0} };
    EntityAffine operator*(const EntityAffine& o) const;
    void Apply(const double p[3], double out[3]) const;
    double Det() const;
    static EntityAffine T(double x, double y, double z);
    static EntityAffine S(double x, double y, double z);
    static EntityAffine S(double s) { return S(s, s, s); }
    static EntityAffine RX(double rad);
    static EntityAffine RY(double rad);
    static EntityAffine RZ(double rad);
    static EntityAffine RYDeg(double deg);
    static EntityAffine RXDeg(double deg);
    static EntityAffine RZDeg(double deg);
};

// 顶层部件名 -> root 之前插入的变换 (旧版 AgeableListModel 幼体)
using TopPre = std::function<EntityAffine(const std::string&)>;

class EntityModel {
public:
    // family: "legacy" (<=1.21.x) / "modern" (26.x); id: ModelLayerLocation, e.g. "minecraft:villager#main"
    EntityModel(const std::string& family, const std::string& id);
    static bool Exists(const std::string& family, const std::string& id);
    bool Valid() const { return layer_ >= 0; }

    int Find(const std::string& name) const;       // -1 = 不存在
    int Parent(int part) const;
    int TopLevel(int part) const;                  // root 的直接子节点(root 自身返回 -1)
    bool InSubtree(int part, int ancestor) const;
    const std::string& Name(int part) const { return names_[part]; }
    int PartCount() const { return static_cast<int>(names_.size()); }

    // 姿态 (x, y, z, xRot, yRot, zRot, xScale, yScale, zScale), 初值 = 原版 bake 后的 initialPose
    float* Pose(int part) { return poses_[part].data(); }
    const float* Pose(int part) const { return poses_[part].data(); }
    void SetPose(const std::string& part, float x, float y, float z);   // 位置 (模型像素)
    void SetRot(const std::string& part, float x, float y, float z);
    void AddRot(const std::string& part, float x, float y, float z);
    void SetVisible(int part, bool v) { visible_[part] = v; }
    void SetVisible(const std::string& part, bool v) { int i = Find(part); if (i >= 0) visible_[i] = v; }
    void HideAllBut(const std::vector<std::string>& keep);             // 只保留这些部件(及其子部件)

    // ModelPart.copyFrom: 按部件名复制姿态 (withPosition=false 只复制旋转)
    void CopyPoseFrom(const EntityModel& src, const std::vector<std::string>& parts, bool withPosition);

    // 部件的完整变换 (base * [pre] * root * ... * part), 即 ModelPart.translateAndRotate 链
    EntityAffine PartTransform(int part, const EntityAffine& base,
                               const TopPre& pre = nullptr) const;

    // 把可见部件写入 out。material(part) 返回材质索引(<0 跳过该部件);
    // pre(topLevel) 可选, 在 root 之前插入的变换 (旧版 AgeableListModel 幼体头/身缩放)。
    void Emit(ModelData& out, const EntityAffine& base,
              const std::function<int(int)>& material,
              const TopPre& pre = nullptr) const;

private:
    int layer_ = -1;
    int partStart_ = 0;
    std::vector<std::string> names_;
    std::vector<int> parents_;
    std::vector<std::array<float, 9>> poses_;
    std::vector<bool> visible_;
};

#endif
