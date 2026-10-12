#pragma once
#include <string>
#include <cstdint>

struct ModelData;

// 解析后的 tint 类别。名字与 Crafter 的 biomeTex 输出名对齐（waterFog 大写 F）。
enum class TintKind : int8_t {
    None = -1,
    Grass = 0,
    Foliage,
    DryFoliage,
    Water,
    WaterFog,
    Fog,
    Sky,
    Fixed
};

struct TintResult {
    TintKind kind = TintKind::None;
    uint32_t color = 0xFFFFFF; // sRGB 0xRRGGBB，仅 Fixed 使用

    bool on() const { return kind != TintKind::None; }
    bool operator==(const TintResult& o) const {
        if (kind != o.kind) return false;
        if (kind == TintKind::Fixed) return color == o.color;
        return true;
    }
    bool operator!=(const TintResult& o) const { return !(*this == o); }
};

// biomeTex 输出名 / tint.json kind 名
const char* TintKindName(TintKind kind);

// 材质名后缀：@grass / @c619961（无 tint 返回空串）
std::string TintSuffix(const TintResult& tint);

// blockStateName 形如 "minecraft:redstone_wire[power=7]"
// tintIndex 为模型面里的 tintindex（-1 表示无）。tintIndex==2 为导出器内部的水约定。
TintResult ResolveTint(const std::string& blockStateName, int tintIndex);

// 对单个方块模型做面级 tint 解析；同一材质出现多种 tint 结果时复制材质并加后缀。
void ApplyTintToBlockModel(ModelData& model, const std::string& blockStateName);
