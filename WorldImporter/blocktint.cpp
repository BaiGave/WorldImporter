#include "blocktint.h"
#include "model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <mutex>
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
        // 内部方块名的属性写作 key:value(见 block.cpp), 也兼容 key=value
        size_t eq = pair.find_first_of("=:");
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
        {"water_cauldron", TintKind::Water},

        // 状态相关（下面 ResolveTint 里单独算颜色）
        {"redstone_wire", TintKind::Fixed},
        {"melon_stem", TintKind::Fixed},
        {"pumpkin_stem", TintKind::Fixed},
        {"attached_melon_stem", TintKind::Fixed},
        {"attached_pumpkin_stem", TintKind::Fixed},
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
    } else if (base == "attached_melon_stem" || base == "attached_pumpkin_stem") {
        result.color = 0xE0C71C;
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
    // 与游戏一致：模型里一个 tintindex 都没有就不染色。
    // 游戏侧 tintindex 是硬门槛——面没有 tintindex 时客户端根本不会去问方块的
    // color provider；资源包删掉这个字段就是在表达"这些面不上色"（例如 Yuushya
    // 把白桦/云杉的贴图直接画成彩色）。这里只记录诊断，绝不替资源包猜色型。
    if (!hasTintedFace) {
        NoteUntintedKnownBlock(blockStateName);
        return;
    }

    // tintindex -> 结果 的小缓存，避免同方块重复解析
    std::vector<std::pair<int, TintResult>> resolveCache;
    auto resolve = [&](int index) -> const TintResult& {
        for (const auto& entry : resolveCache) {
            if (entry.first == index) return entry.second;
        }
        resolveCache.emplace_back(index, ResolveTint(blockStateName, index));
        // 面确实要染色(tintindex >= 0)却解析不出色型：记一条诊断，供导出结束时汇总。
        if (index >= 0 && !resolveCache.back().second.on()) {
            NoteUnresolvedTint(blockStateName);
        }
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

// ------------------------- tint 诊断 -------------------------
// 两件事在导出器里没有"正确"做法，只能提示：
//  1) 面写了 tintindex，但按方块名查不到色型（例如新版本方块 / 模组方块）→ 这些面
//     在游戏里会由方块自己的 provider 上色，Crafter 里则是贴图原色；
//  2) 方块属于已知原版色型，但模型整块没有 tintindex → 游戏里同样不染色，不是 bug。
// 两者都在导出结束时按方块名汇总，替代"没 tintindex 就猜一个色型"的做法。
namespace {

struct TintDiagnostics {
    std::mutex mutex;
    std::unordered_map<std::string, size_t> unresolved;    // 有 tintindex 但查不到色型
    std::unordered_map<std::string, size_t> untintedKnown; // 已知色型但模型无 tintindex
};

TintDiagnostics& Diagnostics() {
    static TintDiagnostics diagnostics;
    return diagnostics;
}

// 方块名条目上限：极端场景（大量模组方块）下避免统计表无限膨胀。
constexpr size_t kMaxDiagEntries = 4096;

void CountEntry(std::unordered_map<std::string, size_t>& entries, const std::string& name) {
    if (name.empty()) return;
    auto it = entries.find(name);
    if (it != entries.end()) {
        ++it->second;
        return;
    }
    if (entries.size() >= kMaxDiagEntries) return;
    entries.emplace(name, 1);
}

void DumpDiagnostics(const char* title, std::unordered_map<std::string, size_t>& entries,
                     const char* hint) {
    if (entries.empty()) return;
    std::vector<std::pair<std::string, size_t>> rows(entries.begin(), entries.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    size_t total = 0;
    for (const auto& row : rows) total += row.second;
    std::cout << "⚠ tint 诊断 | " << title << ": " << rows.size() << " 种, " << total << " 个方块\n";
    const size_t shown = std::min<size_t>(rows.size(), 15);
    for (size_t i = 0; i < shown; ++i) {
        std::cout << "    " << rows[i].first << "  x" << rows[i].second << "\n";
    }
    if (rows.size() > shown) {
        std::cout << "    ... 另有 " << (rows.size() - shown) << " 种未列出\n";
    }
    if (hint) std::cout << "    (" << hint << ")\n";
}

} // namespace

void NoteUnresolvedTint(const std::string& blockStateName) {
    auto& diagnostics = Diagnostics();
    std::lock_guard<std::mutex> lock(diagnostics.mutex);
    CountEntry(diagnostics.unresolved, blockStateName);
}

void NoteUntintedKnownBlock(const std::string& blockStateName) {
    // 只有"表里认得这个方块、但模型没 tintindex"才值得提示；表外方块本来就该不染。
    if (ExtractNamespace(blockStateName) != "minecraft") return;
    if (LookupVanillaKind(ExtractBaseName(blockStateName)) == TintKind::None) return;
    auto& diagnostics = Diagnostics();
    std::lock_guard<std::mutex> lock(diagnostics.mutex);
    CountEntry(diagnostics.untintedKnown, blockStateName);
}

void PrintTintDiagnostics() {
    auto& diagnostics = Diagnostics();
    std::unordered_map<std::string, size_t> unresolved;
    std::unordered_map<std::string, size_t> untintedKnown;
    {
        std::lock_guard<std::mutex> lock(diagnostics.mutex);
        unresolved.swap(diagnostics.unresolved);
        untintedKnown.swap(diagnostics.untintedKnown);
    }
    DumpDiagnostics("有 tintindex 但解析不到色型", unresolved,
                    "这些面不会染色；原版方块出现说明色型表缺项，模组方块属于已知限制");
    DumpDiagnostics("原版色型方块但模型无 tintindex", untintedKnown,
                    "游戏里同样不染色（资源包有意为之），仅提示，未做任何修改");
    std::cout.flush();
}
