#ifndef ITEM_RENDER_H
#define ITEM_RENDER_H

// 物品渲染 (原版 ItemRenderer 的几何部分), 可复用: 实体手持物品/盔甲架/物品展示框/掉落物,
// 以及其他需要"把某个物品画出来"的地方 (如 Create 置物台上的物品)。
//
// 用法:
//   ItemStackInfo item = ItemRender::ParseItemStack(nbt);           // {id, Count|count, tag|components}
//   ModelData mesh;
//   if (ItemRender::Build(item, ItemDisplay::Fixed, false, mesh))  // 已施加 display 变换和 translate(-0.5)
//       ItemRender::Transform(mesh, poseStackEquivalent);          // 再按调用方的 PoseStack 放到世界里
//
// 材质: mesh.materials[i].name / texturePath (相对导出目录, 已写出贴图) / tint (固定色, 物品染色)。
// 物品模型来源: 1.21.4+ assets/<ns>/items/<id>.json (select/condition/range_dispatch 取静态分支),
// 更早版本 models/item/<id>.json; builtin/generated 按原版 ItemModelGenerator 生成挤出几何。
// 箱子/旗帜/床/潜影盒/末地烛/饰纹陶罐/地图等特殊渲染器仍返回 false。
// 盾牌、三叉戟(手持 3D)、头颅已由实体模型生成。

#include "EntityModel.h"
#include "model.h"
#include "nbtutils.h"
#include <string>
#include <vector>

enum class ItemDisplay {
    None, ThirdPersonLeftHand, ThirdPersonRightHand, FirstPersonLeftHand, FirstPersonRightHand,
    Head, Gui, Ground, Fixed
};

// 盾牌旗帜图案的一层 (asset 是 entity/shield/<asset> 的文件名, dye 是 DyeColor 0..15)
struct BannerPatternLayer {
    std::string asset;
    int dye = 0;
};

struct ItemStackInfo {
    std::string id;            // "minecraft:iron_sword"; 空 = 无物品
    int count = 0;
    int dyeColor = -1;         // 染色 RGB (display.color / minecraft:dyed_color), -1 = 未染色
    std::string trimMaterial;  // 盔甲纹饰材料 ("minecraft:gold"), 也用于纹饰调色板
    std::string trimPattern;   // 盔甲纹饰图案 ("minecraft:sentry"), 空 = 无纹饰
    std::string equipAsset;    // minecraft:equippable.asset_id 覆盖值, 空 = 按物品默认
    int bannerBase = -1;       // 盾牌底色 DyeColor id, -1 = 组件/NBT 里没有
    std::vector<BannerPatternLayer> bannerPatterns;
    bool empty() const { return id.empty() || id == "minecraft:air" || count <= 0; }
};

namespace ItemRender {
ItemStackInfo ParseItemStack(const NbtTagPtr& tag);

// 物品几何 (方块单位), 已施加 ItemTransform(display, leftHand) 与 translate(-0.5,-0.5,-0.5)。
// 返回 false = 空/缺模型/特殊渲染器。groundScaleY 可选: 该模型 ground 变换的 y 缩放 (旧版掉落物 bob 用)。
bool Build(const ItemStackInfo& item, ItemDisplay display, bool leftHand, ModelData& out,
           double* groundScaleY = nullptr);

// 用仿射矩阵变换整个网格 (镜像矩阵时翻转绕序)
void Transform(ModelData& mesh, const EntityAffine& m);

// 头颅实体模型 (不含物品 display)。world 是部件之外的 PoseStack。
// modern=false 时附带 1.21 renderSkull 的 S(-1,-1,1)、head.yRot=180°，以及龙颅的额外平移/缩放。
// 玩家头使用默认 Steve 贴图，不读 profile。
bool EmitSkullModel(const std::string& itemId, const EntityAffine& world, bool modern, ModelData& out);

// assets/<ns>/<path> 下的 JSON 资源 (path 如 "items/iron_sword.json", "equipment/iron.json"); 不存在返回 null
nlohmann::json AssetJson(const std::string& ns, const std::string& path);
}  // namespace ItemRender

#endif
