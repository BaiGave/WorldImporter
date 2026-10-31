#include "plantoffset.h"

#include "blockposseed.h"
#include "config.h"
#include "model.h"

#include <string>
#include <unordered_map>

namespace {

enum class Kind { XZ, XYZ };

struct Spec {
    Kind kind;
    float maxH;
    float maxV;
};

// ((double)((float)nibble / 15.0f) - 0.5) * 0.5，再按 Mth.clamp：
// value < min ? min : Math.min(value, max)。
double HorizontalOffset(long long seed, int shift, float maxHorizontal) {
    const long long nibble = (seed >> shift) & 0xFLL;
    const double value =
        (static_cast<double>(static_cast<float>(nibble) / 15.0f) - 0.5) * 0.5;
    const double limit = static_cast<double>(maxHorizontal);
    const double lower = -limit;
    if (value < lower) return lower;
    return value <= limit ? value : limit;
}

// XYZ 的 Y：((double)((float)((seed >> 4) & 15) / 15.0f) - 1.0) * (double)maxV。不钳制。
double VerticalOffset(long long seed, float maxVertical) {
    const long long nibble = (seed >> 4) & 0xFLL;
    return (static_cast<double>(static_cast<float>(nibble) / 15.0f) - 1.0) *
           static_cast<double>(maxVertical);
}

const std::unordered_map<std::string, Spec>& OffsetTable() {
    // 1.18.2 / 1.20.1 / 1.21.1 / 26.3 共有的 id 数值一致，一张表即可。
    // grass 是 1.20.3 之前的 short_grass。26.3 多出的 id 在旧世界里不会出现。
    static const std::unordered_map<std::string, Spec> table = [] {
        std::unordered_map<std::string, Spec> m;
        auto add = [&](Kind kind, float maxH, float maxV, std::initializer_list<const char*> ids) {
            for (const char* id : ids) m.emplace(id, Spec{kind, maxH, maxV});
        };
        constexpr float kH = 0.25f;
        constexpr float kV = 0.2f;
        add(Kind::XZ, kH, kV, {
            "mangrove_propagule", "tall_seagrass",
            "dandelion", "poppy", "blue_orchid", "allium", "azure_bluet",
            "red_tulip", "orange_tulip", "white_tulip", "pink_tulip",
            "oxeye_daisy", "cornflower", "wither_rose", "lily_of_the_valley",
            "torchflower", "golden_dandelion", "open_eyeblossom", "closed_eyeblossom",
            "sunflower", "lilac", "rose_bush", "peony",
            "tall_grass", "large_fern", "pitcher_plant",
            "bamboo_sapling", "bamboo",
            "warped_roots", "nether_sprouts", "crimson_roots", "hanging_roots",
        });
        // 钟乳石 / 硫磺刺：Speleothem 的水平上限是 0.125f，不是默认 0.25f。
        add(Kind::XZ, 0.125f, kV, {"pointed_dripstone", "sulfur_spike"});
        add(Kind::XYZ, kH, kV, {
            "grass", "short_grass", "fern", "short_dry_grass", "tall_dry_grass",
        });
        // 小型滴水叶的垂直上限是 0.1f。
        add(Kind::XYZ, kH, 0.1f, {"small_dripleaf"});
        return m;
    }();
    return table;
}

std::string_view IdOnly(std::string_view blockId) {
    const size_t bracket = blockId.find('[');
    if (bracket != std::string_view::npos) blockId = blockId.substr(0, bracket);
    return blockId;
}

}  // namespace

bool VanillaPlantOffset(std::string_view blockId, int x, int z,
                        double& ox, double& oy, double& oz) {
    const auto& table = OffsetTable();
    const auto it = table.find(std::string(IdOnly(blockId)));
    if (it == table.end()) return false;
    const Spec& spec = it->second;
    const long long seed = BlockPosSeed(x, 0, z);
    ox = HorizontalOffset(seed, 0, spec.maxH);
    oz = HorizontalOffset(seed, 8, spec.maxH);
    oy = spec.kind == Kind::XYZ ? VerticalOffset(seed, spec.maxV) : 0.0;
    return true;
}

void ApplyVanillaPlantOffset(ModelData& model, std::string_view ns,
                             std::string_view blockId, int x, int z) {
    if (!config.plantOffset) return; // 开关关掉时植物都待在格子中心
    if (ns != "minecraft" || model.vertices.empty()) return;
    double ox = 0.0;
    double oy = 0.0;
    double oz = 0.0;
    if (!VanillaPlantOffset(blockId, x, z, ox, oy, oz)) return;
    const float fx = static_cast<float>(ox);
    const float fy = static_cast<float>(oy);
    const float fz = static_cast<float>(oz);
    for (size_t i = 0; i + 2 < model.vertices.size(); i += 3) {
        model.vertices[i] += fx;
        model.vertices[i + 1] += fy;
        model.vertices[i + 2] += fz;
    }
}
