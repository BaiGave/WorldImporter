#include "blocktint.h"
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// 从 "minecraft:redstone_wire[power=7]" 提取命名空间
std::string_view ExtractNamespace(std::string_view name) {
    size_t bracket = name.find('[');
    std::string_view head = (bracket == std::string_view::npos) ? name : name.substr(0, bracket);
    size_t colon = head.find(':');
    if (colon == std::string_view::npos) return "minecraft";
    return head.substr(0, colon);
}

// 提取不带命名空间、不带状态的基础方块名
std::string_view ExtractBaseName(std::string_view name) {
    size_t bracket = name.find('[');
    std::string_view head = (bracket == std::string_view::npos) ? name : name.substr(0, bracket);
    size_t colon = head.find(':');
    return (colon == std::string_view::npos) ? head : head.substr(colon + 1);
}

// 读取整型 blockstate 属性，缺省返回 defaultValue
int ReadStateInt(std::string_view name, std::string_view key, int defaultValue) {
    size_t bracket = name.find('[');
    if (bracket == std::string_view::npos) return defaultValue;
    size_t close = name.find(']', bracket + 1);
    if (close == std::string_view::npos) return defaultValue;
    std::string_view states = name.substr(bracket + 1, close - bracket - 1);
    size_t start = 0;
    while (start < states.size()) {
        size_t end = states.find(',', start);
        std::string_view pair = (end == std::string_view::npos) ? states.substr(start)
                                                                : states.substr(start, end - start);
        size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == key) {
            try {
                return std::stoi(std::string(pair.substr(eq + 1)));
            } catch (...) {
                return defaultValue;
            }
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return defaultValue;
}

// 原版 RedstoneWireBlock.getColorForPower 的同款公式
uint32_t RedstoneColor(int power) {
    power = std::clamp(power, 0, 15);
    float f = power / 15.0f;
    float r = (power == 0) ? 0.3f : f * 0.6f + 0.4f;
    float g = f * f * 0.7f - 0.5f;
    float b = f * f * 0.6f - 0.7f;
    if (g < 0.0f) g = 0.0f;
    if (b < 0.0f) b = 0.0f;
    auto channel = [](float v) -> uint32_t {
        int c = static_cast<int>(v * 255.0f);
        return static_cast<uint32_t>(std::clamp(c, 0, 255));
    };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);
}

// 原版茎按年龄着色的色表（age 0..7）
constexpr uint32_t kStemColors[8] = {
    0x00FF00, 0x20F704, 0x40EF08, 0x60E70C,
    0x80DF10, 0xA0D714, 0xC0CF18, 0xE0C71C
};

uint32_t StemColor(int age) {
    age = std::clamp(age, 0, 7);
    return kStemColors[age];
}

// 按方块基础名查询静态色型表（仅 minecraft 命名空间）
TintKind LookupVanillaKind(std::string_view baseName) {
    static const std::unordered_map<std::string_view, TintKind> kTable = {
        // 草类（BiomeColors.getAverageGrassColor）
        {"grass_block", TintKind::Grass},
        {"grass", TintKind::Grass},
        {"short_grass", TintKind::Grass},
        {"tall_grass", TintKind::Grass},
        {"fern", TintKind::Grass},
        {"large_fern", TintKind::Grass},
        {"potted_fern", TintKind::Grass},
        {"sugar_cane", TintKind::Grass},
        {"pink_petals", TintKind::Grass},
        {"wildflowers", TintKind::Grass},
        {"bush", TintKind::Grass},
        {"firefly_bush", TintKind::Grass},

        // 树叶类（BiomeColors.getAverageFoliageColor）
        {"oak_leaves", TintKind::Foliage},
        {"jungle_leaves", TintKind::Foliage},
        {"acacia_leaves", TintKind::Foliage},
        {"dark_oak_leaves", TintKind::Foliage},
        {"mangrove_leaves", TintKind::Foliage},
        {"vine", TintKind::Foliage},

        // 干叶类
        {"short_dry_grass", TintKind::DryFoliage},
        {"tall_dry_grass", TintKind::DryFoliage},
        {"leaf_litter", TintKind::DryFoliage},

        // 固定色
        {"spruce_leaves", TintKind::Fixed}, // 0x619961
        {"birch_leaves", TintKind::Fixed},  // 0x80A755
        {"lily_pad", TintKind::Fixed},      // 0x208030

        // 水
        {"water", TintKind::Water},

        // 状态相关（下面 ResolveTint 里单独算颜色）
        {"redstone_wire", TintKind::Fixed},
        {"melon_stem", TintKind::Fixed},
        {"pumpkin_stem", TintKind::Fixed},
    };
    auto it = kTable.find(baseName);
    return (it == kTable.end()) ? TintKind::None : it->second;
}

} // namespace

const char* TintKindName(TintKind kind) {
    switch (kind) {
    case TintKind::Grass: return "grass";
    case TintKind::Foliage: return "foliage";
    case TintKind::DryFoliage: return "dryfoliage";
    case TintKind::Water: return "water";
    case TintKind::WaterFog: return "waterFog";
    case TintKind::Fog: return "fog";
    case TintKind::Sky: return "sky";
    case TintKind::Fixed: return "fixed";
    default: return "none";
    }
}

std::string TintSuffix(const TintResult& tint) {
    if (!tint.on()) return {};
    if (tint.kind != TintKind::Fixed) return std::string("@") + TintKindName(tint.kind);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "@c%06X", tint.color & 0xFFFFFFu);
    return buf;
}

TintResult ResolveTint(const std::string& blockStateName, int tintIndex) {
    TintResult result;
    if (tintIndex < 0) return result;

    // 导出器内部约定：流体材质用 tintIndex == 2 标记水
    if (tintIndex == 2) {
        result.kind = TintKind::Water;
        return result;
    }

    // 目前只处理原版，模组方块先不猜色型
    if (ExtractNamespace(blockStateName) != "minecraft") return result;

    const std::string_view base = ExtractBaseName(blockStateName);
    if (base.empty()) return result;

    switch (LookupVanillaKind(base)) {
    case TintKind::None:
        return result;
    case TintKind::Grass:
    case TintKind::Foliage:
    case TintKind::DryFoliage:
    case TintKind::Water:
    case TintKind::WaterFog:
    case TintKind::Fog:
    case TintKind::Sky:
        result.kind = LookupVanillaKind(base);
        return result;
    case TintKind::Fixed:
        break;
    }

    // 固定色
    if (base == "spruce_leaves") {
        result.color = 0x619961;
    } else if (base == "birch_leaves") {
        result.color = 0x80A755;
    } else if (base == "lily_pad") {
        result.color = 0x208030;
    } else if (base == "redstone_wire") {
        result.color = RedstoneColor(ReadStateInt(blockStateName, "power", 0));
    } else if (base == "melon_stem" || base == "pumpkin_stem") {
        result.color = StemColor(ReadStateInt(blockStateName, "age", 0));
    } else {
        // 表里标了 Fixed 却没有具体颜色，视为不上色
        return TintResult{};
    }
    result.kind = TintKind::Fixed;
    return result;
}

void ApplyTintToBlockModel(ModelData& model, const std::string& blockStateName) {
    if (model.faces.empty() || model.materials.empty()) return;

    bool hasTintedFace = false;
    for (const auto& face : model.faces) {
        if (face.tintIndex >= 0) {
            hasTintedFace = true;
            break;
        }
    }
    if (!hasTintedFace) return;

    // tintindex -> 结果 的小缓存，避免同方块重复解析
    std::vector<std::pair<int, TintResult>> resolveCache;
    auto resolve = [&](int index) -> const TintResult& {
        for (const auto& entry : resolveCache) {
            if (entry.first == index) return entry.second;
        }
        resolveCache.emplace_back(index, ResolveTint(blockStateName, index));
        return resolveCache.back().second;
    };

    struct Variant {
        int original;
        TintResult tint;
        int newIndex;
    };
    std::vector<Variant> variants;
    std::vector<Material> newMaterials;
    newMaterials.reserve(model.materials.size());

    auto variantIndex = [&](int original, const TintResult& tint) -> int {
        // CTM 规则已把 tint 解析并锁定到材质(见 CTM.cpp applyRuleTint)时原样保留,
        // 不能再按当前方块的 tintindex 重解析(否则草方块上的砂砾 overlay 会被染成草绿)。
        if (model.materials[original].tintLocked) {
            for (const auto& v : variants) {
                if (v.original == original) return v.newIndex;
            }
            Material material = model.materials[original];
            int index = static_cast<int>(newMaterials.size());
            newMaterials.push_back(std::move(material));
            variants.push_back({original, TintResult{}, index});
            return index;
        }
        for (const auto& v : variants) {
            if (v.original == original && v.tint == tint) return v.newIndex;
        }
        Material material = model.materials[original];
        material.tint = tint;
        if (tint.on()) {
            material.name = material.name + TintSuffix(tint);
        } else {
            material.tintIndex = -1;
        }
        int index = static_cast<int>(newMaterials.size());
        newMaterials.push_back(std::move(material));
        variants.push_back({original, tint, index});
        return index;
    };

    const int materialCount = static_cast<int>(model.materials.size());
    for (auto& face : model.faces) {
        if (face.materialIndex < 0 || face.materialIndex >= materialCount) continue;
        face.materialIndex = variantIndex(face.materialIndex, resolve(face.tintIndex));
    }

    model.materials = std::move(newMaterials);
}
