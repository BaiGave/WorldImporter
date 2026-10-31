#include "biome.h"
#include "config.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

// Golden vectors from the real 1.21.1 classes (package vref3, see tests/vref3).
// ctest does not need Java; regenerate with VRef.java and Noise.java if the
// game jars change.

Config config;

namespace {

uint64_t DoubleBits(double value) {
    uint64_t bits = 0;
    static_assert(sizeof(double) == sizeof(uint64_t), "double must be 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void ExpectEq(int64_t actual, int64_t expected, const std::string& label) {
    if (actual == expected) return;
    std::ostringstream out;
    out << label << ": expected " << expected << ", got " << actual;
    Fail(out.str());
}

void ExpectBits(double actual, uint64_t expected, const std::string& label) {
    const uint64_t bits = DoubleBits(actual);
    if (bits == expected) return;
    std::ostringstream out;
    out << label << ": expected bits " << expected << ", got " << bits;
    Fail(out.str());
}

void TestObfuscateSeed() {
    const int64_t minSeed = std::numeric_limits<int64_t>::min();
    const struct { int64_t seed, hashed; } cases[] = {
        {0, 8794265229978523055LL},
        {1, -6467378160175308932LL},
        {-1, 6759447113877070610LL},
        {42, -4111196313959201555LL},
        {123456789LL, -5458710421630621949LL},
        {-4530634556500121041LL, 4792833769898742894LL},
        {2345, -770544256835470535LL},
        {minSeed, 6374347445474471398LL},
        {987654321012345678LL, -8281241630885055684LL},
    };
    for (const auto& entry : cases) {
        ExpectEq(BiomeObfuscateSeed(entry.seed), entry.hashed,
                 "obfuscateSeed(" + std::to_string(entry.seed) + ")");
    }
}

void TestLcg() {
    const int64_t minSeed = std::numeric_limits<int64_t>::min();
    const struct { int64_t seed, salt, next; } cases[] = {
        {0, 0, 0},
        {1, 1, 7806831264735756413LL},
        {-1, 5, 4921441182957829603LL},
        {12345, -99, -4650592213636512687LL},
        {6364136223846793005LL, 1442695040888963407LL, 2669886397722105383LL},
        {minSeed, 7, minSeed + 7},
        {81985529216486895LL, -1, 8669372371246408077LL},
    };
    for (const auto& entry : cases) {
        ExpectEq(BiomeLcgNext(entry.seed, entry.salt), entry.next, "lcg");
    }
}

void TestFiddledDistance() {
    const int64_t zoom = BiomeObfuscateSeed(123456789LL);
    ExpectEq(zoom, -5458710421630621949LL, "zoom seed");
    const struct { int qx, qy, qz; double fx; uint64_t bits; } cases[] = {
        {0, 0, 0, 0.00, 4600929866399036212ULL},
        {0, 0, 0, 0.25, 4598960421171363840ULL},
        {0, 0, 0, 0.50, 4599242775757376717ULL},
        {0, 0, 0, 0.75, 4601776930157074841ULL},
        {1, 2, 3, 0.00, 4604618725419143661ULL},
        {1, 2, 3, 0.25, 4603970453363406931ULL},
        {1, 2, 3, 0.50, 4604448081214512825ULL},
        {1, 2, 3, 0.75, 4606051608972461344ULL},
        {-1, -2, -3, 0.00, 4594149535360021955ULL},
        {-1, -2, -3, 0.25, 4596812992327146537ULL},
        {-1, -2, -3, 0.50, 4601077634233459016ULL},
        {-1, -2, -3, 0.75, 4604795890758519357ULL},
        {15, -4, 8, 0.00, 4606141560018729697ULL},
        {15, -4, 8, 0.25, 4607849901772022743ULL},
        {15, -4, 8, 0.50, 4609600764088093246ULL},
        {15, -4, 8, 0.75, 4611800297392486482ULL},
        {4, 16, -5, 0.00, 4606690044779996119ULL},
        {4, 16, -5, 0.25, 4605440119961540362ULL},
        {4, 16, -5, 0.50, 4605316095049927230ULL},
        {4, 16, -5, 0.75, 4606317970045156721ULL},
    };
    for (const auto& entry : cases) {
        const double dist = BiomeFiddledDistance(zoom, entry.qx, entry.qy, entry.qz, entry.fx, 0.25, 0.5);
        ExpectBits(dist, entry.bits, "fiddled distance");
    }
}

void ExpectQuart(int64_t zoom, int x, int y, int z, int qx, int qy, int qz) {
    int ax = 0, ay = 0, az = 0;
    BiomeChosenQuart(zoom, x, y, z, ax, ay, az);
    if (ax == qx && ay == qy && az == qz) return;
    std::ostringstream out;
    out << "quart " << x << "," << y << "," << z << " expected " << qx << "," << qy << "," << qz
        << " got " << ax << "," << ay << "," << az;
    Fail(out.str());
}

void TestChosenQuart() {
    const int64_t zoom = BiomeObfuscateSeed(123456789LL);
    const struct { int x, y, z, qx, qy, qz; } cases[] = {
        {0, 64, 0, -1, 16, -1},
        {1, 64, 0, 0, 16, -1},
        {2, 63, -1, 0, 15, -1},
        {3, 64, 3, 0, 16, 0},
        {4, 64, 4, 1, 16, 1},
        {15, 70, 15, 3, 17, 3},
        {16, 64, 16, 4, 15, 3},
        {-1, 64, -1, -1, 16, -1},
        {-5, 30, 7, -2, 7, 1},
        {8, 8, 8, 2, 1, 2},
        {100, -64, 100, 24, -17, 25},
        {-16, 319, -16, -5, 80, -4},
        {7, 63, 8, 1, 15, 2},
        {-8, -33, 9, -3, -9, 2},
    };
    for (const auto& entry : cases) ExpectQuart(zoom, entry.x, entry.y, entry.z, entry.qx, entry.qy, entry.qz);

    const int64_t worldZoom = BiomeObfuscateSeed(-4530634556500121041LL);
    ExpectEq(worldZoom, 4792833769898742894LL, "world zoom");
    const struct { int x, y, z, qx, qy, qz; } world[] = {
        {0, 80, 0, 0, 20, -1},
        {17, 63, -20, 4, 15, -6},
        {-2, 100, 5, -1, 25, 1},
        {31, 64, 31, 8, 15, 7},
    };
    for (const auto& entry : world) ExpectQuart(worldZoom, entry.x, entry.y, entry.z, entry.qx, entry.qy, entry.qz);
}

void TestSwamp() {
    const struct { int x, z, color; uint64_t bits; } cases[] = {
        {0, 0, 6975545, 0ULL},
        {1, 1, 6975545, 4591700309687113436ULL},
        {8, -2, 6975545, 4603466275018636357ULL},
        {-3, 9, 5011004, 13820301741379738394ULL},
        {15, 40, 6975545, 4591809184279306006ULL},
        {100, -7, 5011004, 13826596135591347945ULL},
        {-40, 3, 6975545, 4602247698123376138ULL},
        {7, 80, 5011004, 13823056610080102604ULL},
        {64, 15, 6975545, 4599214077674703854ULL},
        {-16, -16, 6975545, 4599556989228577391ULL},
    };
    for (const auto& entry : cases) {
        ExpectBits(BiomeSwampNoise(entry.x, entry.z), entry.bits, "swamp noise");
        ExpectEq(BiomeSwampGrassColor(entry.x, entry.z), entry.color, "swamp color");
    }
}

void TestBlendAverage() {
    const int four[] = {0xFF0100, 0x00FF00, 0x0000FF, 0x010203};
    ExpectEq(BiomeBlendTint(four, 4), 0x404040, "blend 4");
    const int trunc[] = {0x020000, 0x020000, 0x010000};
    ExpectEq(BiomeBlendTint(trunc, 3), 0x010000, "blend trunc");
    const int single[] = {0x4C763C};
    ExpectEq(BiomeBlendTint(single, 1), 0x4C763C, "blend one");
}

void TestRadiusConfig() {
    Config defaults;
    ExpectEq(defaults.biomeBlendRadius, 2, "default radius");
    const auto dir = std::filesystem::temp_directory_path() / "wi_biome_blend_cfg";
    std::filesystem::create_directories(dir);
    const auto path = dir / "config.json";
    {
        std::ofstream out(path);
        out << "{\"biomeBlendRadius\": 100}";
    }
    ExpectEq(LoadConfig(path.string()).biomeBlendRadius, 32, "clamp high");
    {
        std::ofstream out(path);
        out << "{\"biomeBlendRadius\": -4}";
    }
    ExpectEq(LoadConfig(path.string()).biomeBlendRadius, 0, "clamp low");
    {
        std::ofstream out(path);
        out << "{\"biomeBlendRadius\": 0}";
    }
    ExpectEq(LoadConfig(path.string()).biomeBlendRadius, 0, "radius off");
}

}  // namespace

int main() {
    try {
        TestObfuscateSeed();
        TestLcg();
        TestFiddledDistance();
        TestChosenQuart();
        TestSwamp();
        TestBlendAverage();
        TestRadiusConfig();
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
    std::cout << "biome blend vectors ok" << std::endl;
    return 0;
}
