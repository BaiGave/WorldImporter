#include "blockstate.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

// The application normally supplies this in main.cpp.
Config config;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Java 向量（/tmp 里的 LegacyPick，Mth.getSeed + java.util.Random）。
// 权重：4 = {1,1,1,1}，3 = {1,2}，7 = {1,2,4}。
// i* 是 abs((int)nextLong()) % w；s* 是 getWeightedItem 槽位。
// m* / ms* 是 multipart：new Random(new Random(seed).nextLong()) 的索引和槽位。
// n* 是 1.21.5+ nextInt 槽位。p4/p3/p4b 是 multipart 顺序 nextInt（跳过非加权组）。
struct Vec {
    int x, y, z;
    long long seed, next, part;
    int i4, i3, i7;
    int s4, s3, s7;
    int m4, m3, m7;
    int ms4, ms3, ms7;
    int n4, n3, n7;
    int p4, p3, p4b;
};

constexpr Vec kVecs[] = {
    {0, 0, 0, 0, -4962768465676381896LL, 4804307197456638271LL,
        0, 2, 0, 0, 1, 0, 3, 0, 3, 3, 0, 2, 2, 0, 2, 2, 1, 0},
    {1, 2, 3, -33674130277896LL, -3774776766155813106LL, -9150041119820290220LL,
        2, 1, 0, 2, 1, 0, 0, 1, 1, 0, 1, 1, 3, 1, 2, 3, 0, 2},
    {-1, 0, 0, 133076631896896LL, 3934986497149574704LL, 8060158486708560834LL,
        0, 0, 6, 0, 0, 2, 2, 2, 4, 2, 1, 2, 0, 0, 0, 0, 1, 2},
    {0, -1, 0, 645, -5605584720434349240LL, -1668744637787913649LL,
        0, 0, 6, 0, 0, 2, 1, 0, 5, 1, 0, 2, 2, 0, 2, 2, 0, 0},
    {0, 0, -1, -20769809685848LL, -3079779970147568391LL, 4257671866014695938LL,
        1, 1, 2, 1, 1, 1, 2, 1, 1, 2, 1, 1, 3, 1, 2, 3, 0, 1},
    {-5, 64, -7, 98641477852946LL, -1535830954054350877LL, 4661230172429430075LL,
        3, 2, 3, 3, 1, 2, 3, 1, 0, 3, 1, 0, 3, 1, 2, 3, 1, 1},
    {16, 70, 16, 85408436249061LL, 1642836547225657544LL, -2873554988641717225LL,
        0, 1, 4, 0, 1, 2, 3, 0, 2, 3, 0, 1, 0, 1, 2, 0, 0, 0},
    {-16, 100, -16, 5652833026595LL, 5617587780655262925LL, -2150457318512232593LL,
        3, 0, 6, 3, 0, 2, 3, 1, 5, 3, 1, 2, 1, 1, 2, 1, 0, 1},
    {100, -64, 200, 6979507695959LL, -8721999621003030256LL, 5308874669245652960LL,
        0, 2, 6, 0, 1, 2, 0, 2, 1, 0, 1, 1, 2, 1, 2, 2, 1, 1},
    {-3129871, 150, 8, 43813554671449LL, -780990848248215926LL, -6119395799456031841LL,
        2, 2, 5, 2, 1, 2, 3, 2, 0, 3, 1, 0, 3, 0, 2, 3, 1, 3},
    {3129871, -20, -100000, -78825962843268LL, 4078504197887379706LL, -686771311484591345LL,
        2, 2, 6, 2, 1, 2, 1, 0, 5, 1, 0, 2, 0, 0, 2, 0, 1, 2},
    {8, 150, -3, -3535083877493LL, -6715928692134385789LL, 5054921787666735774LL,
        1, 2, 2, 1, 1, 1, 2, 2, 0, 2, 1, 0, 2, 1, 1, 2, 1, 1},
    {-100, -64, -200, 45272202291123LL, 8213166214719027645LL, -1202162662694501492LL,
        1, 0, 4, 1, 0, 2, 0, 2, 6, 0, 1, 2, 1, 0, 1, 1, 1, 3},
    {123456, 320, -654321, 83414464962617LL, 6999085345101299539LL, -6228074032274713839LL,
        1, 0, 5, 1, 0, 2, 1, 0, 5, 1, 0, 2, 1, 1, 2, 1, 0, 2},
};

constexpr int kW4[] = {1, 1, 1, 1};
constexpr int kW3[] = {1, 2};
constexpr int kW7[] = {1, 2, 4};

int slotOf(std::int32_t index, const int* weights, int count) {
    return SelectWeightedSlot(index, count, [&](int i) { return weights[i]; });
}

ModelData marker(int id) {
    ModelData model;
    model.vertices = {static_cast<float>(id), 0.f, 0.f};
    return model;
}

std::vector<WeightedModelData> weighted(const int* weights, int count, int base) {
    std::vector<WeightedModelData> models;
    for (int i = 0; i < count; ++i) {
        WeightedModelData entry;
        entry.weight = weights[i];
        entry.model = marker(base + i);
        models.push_back(std::move(entry));
    }
    return models;
}

int readMarker(const ModelData& model, const std::string& label) {
    require(model.vertices.size() >= 3, label + " missing marker");
    return static_cast<int>(std::lround(model.vertices[0]));
}

std::vector<int> readMarkers(const ModelData& model, const std::string& label) {
    require(model.vertices.size() % 3 == 0 && !model.vertices.empty(), label + " bad vertices");
    std::vector<int> ids;
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        ids.push_back(static_cast<int>(std::lround(model.vertices[i])));
    }
    return ids;
}

void checkRng() {
    for (const Vec& v : kVecs) {
        const std::string at = std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
        require(BlockPosSeed(v.x, v.y, v.z) == v.seed, "seed " + at);
        LegacyRandom once(v.seed);
        require(once.nextLong() == v.next, "nextLong " + at);
        LegacyRandom outer(v.seed);
        const long long partSeed = outer.nextLong();
        LegacyRandom part(partSeed);
        require(part.nextLong() == v.part, "multipart nextLong " + at);

        LegacyRandom i4(v.seed), i3(v.seed), i7(v.seed);
        require(LegacyWeightedIndex(i4, 4) == v.i4, "index4 " + at);
        require(LegacyWeightedIndex(i3, 3) == v.i3, "index3 " + at);
        require(LegacyWeightedIndex(i7, 7) == v.i7, "index7 " + at);
        require(slotOf(v.i4, kW4, 4) == v.s4, "slot4 " + at);
        require(slotOf(v.i3, kW3, 2) == v.s3, "slot3 " + at);
        require(slotOf(v.i7, kW7, 3) == v.s7, "slot7 " + at);
        require(slotOf(v.m4, kW4, 4) == v.ms4, "mp slot4 " + at);
        require(slotOf(v.m3, kW3, 2) == v.ms3, "mp slot3 " + at);
        require(slotOf(v.m7, kW7, 3) == v.ms7, "mp slot7 " + at);

        LegacyRandom modern(v.seed);
        require(slotOf(modern.nextInt(4), kW4, 4) == v.n4, "modern4 " + at);
        LegacyRandom modern3(v.seed);
        require(slotOf(modern3.nextInt(3), kW3, 2) == v.n3, "modern3 " + at);
        LegacyRandom modern7(v.seed);
        require(slotOf(modern7.nextInt(7), kW7, 3) == v.n7, "modern7 " + at);

        // 非加权组不消耗：w4、跳过、w3、w4。
        LegacyRandom stream(v.seed);
        require(slotOf(stream.nextInt(4), kW4, 4) == v.p4, "stream p4 " + at);
        require(slotOf(stream.nextInt(3), kW3, 2) == v.p3, "stream p3 " + at);
        require(slotOf(stream.nextInt(4), kW4, 4) == v.p4b, "stream p4b " + at);
    }
}

void checkMinValue() {
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    require(JavaAbs32(kMin) == kMin, "abs(MIN_VALUE) must stay negative");
    require(JavaAbs32(kMin) % 4 == 0, "MIN % 4");
    require(JavaAbs32(kMin) % 3 == -2, "MIN % 3");
    require(JavaAbs32(kMin) % 7 == -2, "MIN % 7");
    // 游戏先取余，负余数选中第一项。未取余的 MIN_VALUE 减法回绕，得到空。
    require(slotOf(JavaAbs32(kMin) % 4, kW4, 4) == 0, "modded MIN picks first of 4");
    require(slotOf(JavaAbs32(kMin) % 3, kW3, 2) == 0, "modded MIN picks first of 3");
    require(slotOf(JavaAbs32(kMin) % 7, kW7, 3) == 0, "modded MIN picks first of 7");
    require(slotOf(kMin, kW4, 4) == -1, "unreduced MIN_VALUE is empty");

    const auto models = weighted(kW4, 4, 0);
    const ModelData empty = TakeWeightedModel(models, kMin);
    require(empty.vertices.empty() && empty.faces.empty(), "empty model for unreduced MIN_VALUE");
    require(readMarker(TakeWeightedModel(models, JavaAbs32(kMin) % 4), "modded") == 0,
            "modded MIN model is the first weight");
}

void checkCache() {
    VariantModelCache["vp"]["w4"] = weighted(kW4, 4, 0);
    VariantModelCache["vp"]["w3"] = weighted(kW3, 2, 10);
    VariantModelCache["vp"]["w7"] = weighted(kW7, 3, 30);
    MultipartModelCache["vp"]["multi"] = {
        {weighted(kW4, 4, 0), true},
        {weighted(kW4, 1, 100), false},
        {weighted(kW3, 2, 10), true},
        {weighted(kW4, 4, 20), true},
    };
    blockstateCachesFrozen.store(true, std::memory_order_release);
    config.variantSeedMode = VariantSeedMode::Game;

    int differed = 0;
    for (const Vec& v : kVecs) {
        const long long seed = BlockPosSeed(v.x, v.y, v.z);
        const std::string at = std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
        if (v.s4 != v.n4 || v.s3 != v.n3 || v.s7 != v.n7) ++differed;

        g_activeChunkDataVersion = 3955; // 1.21.1
        require(readMarker(GetRandomModelFromCache("vp", "w4", seed), at) == v.s4, "legacy w4 " + at);
        require(readMarker(GetRandomModelFromCache("vp", "w3", seed), at) == 10 + v.s3, "legacy w3 " + at);
        require(readMarker(GetRandomModelFromCache("vp", "w7", seed), at) == 30 + v.s7, "legacy w7 " + at);
        {
            const auto ids = readMarkers(GetRandomModelFromCache("vp", "multi", seed), "legacy multi " + at);
            require(ids.size() == 4, "legacy multi count " + at);
            require(ids[0] == v.ms4 && ids[1] == 100 && ids[2] == 10 + v.ms3 && ids[3] == 20 + v.ms4,
                    "legacy multi slots " + at);
        }

        g_activeChunkDataVersion = std::nullopt;
        require(readMarker(GetRandomModelFromCache("vp", "w4", seed), at) == v.n4, "modern w4 " + at);
        require(readMarker(GetRandomModelFromCache("vp", "w3", seed), at) == 10 + v.n3, "modern w3 " + at);
        require(readMarker(GetRandomModelFromCache("vp", "w7", seed), at) == 30 + v.n7, "modern w7 " + at);
        {
            const auto ids = readMarkers(GetRandomModelFromCache("vp", "multi", seed), "modern multi " + at);
            require(ids.size() == 4, "modern multi count " + at);
            require(ids[0] == v.p4 && ids[1] == 100 && ids[2] == 10 + v.p3 && ids[3] == 20 + v.p4b,
                    "modern multi slots " + at);
        }
    }
    require(differed > 0, "legacy and modern picks never diverged");

    const long long seed = BlockPosSeed(0, 0, 0);
    g_activeChunkDataVersion = 4324;
    require(readMarker(GetRandomModelFromCache("vp", "w4", seed), "4324") == kVecs[0].s4, "4324 is legacy");
    g_activeChunkDataVersion = 4325;
    require(readMarker(GetRandomModelFromCache("vp", "w4", seed), "4325") == kVecs[0].n4, "4325 is modern");
    g_activeChunkDataVersion = 5023; // 26.3
    require(readMarker(GetRandomModelFromCache("vp", "w4", seed), "5023") == kVecs[0].n4, "26.3 is modern");
    g_activeChunkDataVersion = std::nullopt;
    require(readMarker(GetRandomModelFromCache("vp", "w4", seed), "missing") == kVecs[0].n4, "missing stays modern");

    config.variantSeedMode = VariantSeedMode::First;
    g_activeChunkDataVersion = 3955;
    require(readMarker(GetRandomModelFromCache("vp", "w4", seed), "first") == 0, "First ignores version");
    config.variantSeedMode = VariantSeedMode::Game;
    g_activeChunkDataVersion = std::nullopt;
}
} // namespace

int main() {
    try {
        checkRng();
        checkMinValue();
        checkCache();
        std::cout << "PASS: legacy and modern variant picks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
