// Golden values come from WorldImporter/tests/vref2/OffsetRef.java, which calls
// the real 1.21.1 client: ayo.b(int,int,int) (Mth.getSeed) and ayo.a(DDD)D (Mth.clamp).
#include "plantoffset.h"
#include "blockposseed.h"
#include "model.h"

#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

Config config;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Row {
    int x;
    int z;
    long long seed;
    double x25;
    double z25;
    double y02;
    double y01;
    double x125;
    double z125;
};

// maxH 0.25 and 0.125, maxV 0.2 and 0.1. Y of the block is not an input.
constexpr Row kRows[] = {
    {0, 0, 0LL,
        -0.25000000000000000, -0.25000000000000000,
        -0.20000000298023224, -0.10000000149011612,
        -0.12500000000000000, -0.12500000000000000},
    {1, 2, -98563738688972LL,
        -0.11666665971279144, -0.18333332985639572,
        -0.16000000178813933, -0.080000000894069670,
        -0.11666665971279144, -0.12500000000000000},
    {-1, 3, -33674130395733LL,
        0.11666667461395264, 0.18333333730697632,
        -0.066666663686434370, -0.033333331843217184,
        0.11666667461395264, 0.12500000000000000},
    {-8, -8, -90067312761588LL,
        0.15000000596046448, -0.083333328366279600,
        -0.20000000298023224, -0.10000000149011612,
        0.12500000000000000, -0.083333328366279600},
    {15, -1, -98377442327771LL,
        -0.083333328366279600, -0.14999999850988388,
        -0.17333333452542620, -0.086666667262713100,
        -0.083333328366279600, -0.12500000000000000},
    {123456, -98765, 70753772163237LL,
        -0.083333328366279600, -0.25000000000000000,
        -0.066666663686434370, -0.033333331843217184,
        -0.083333328366279600, -0.12500000000000000},
    {-30000001, 42, 46559573724323LL,
        -0.14999999850988388, 0.15000000596046448,
        -0.066666663686434370, -0.033333331843217184,
        -0.12500000000000000, 0.12500000000000000},
    {8, 8, -90067312761588LL,
        0.15000000596046448, -0.083333328366279600,
        -0.20000000298023224, -0.10000000149011612,
        0.12500000000000000, -0.083333328366279600},
    {49, 14, -27674899506588LL,
        -0.11666665971279144, 0.083333343267440800,
        -0.12000000059604643, -0.060000000298023215,
        -0.11666665971279144, 0.083333343267440800},
    {59, 26, -57368854717921LL,
        0.25000000000000000, 0.083333343267440800,
        -0.18666666875282922, -0.093333334376414610,
        0.12500000000000000, 0.083333343267440800},
    {37, 26, -99089864099179LL,
        -0.083333328366279600, 0.083333343267440800,
        -0.079999996423721240, -0.039999998211860620,
        -0.083333328366279600, 0.083333343267440800},
    {25, 18, -116561250191433LL,
        -0.016666665673255920, -0.016666665673255920,
        -0.053333330949147495, -0.026666665474573747,
        -0.016666665673255920, -0.016666665673255920},
    {19, 18, -125987537726513LL,
        0.25000000000000000, -0.14999999850988388,
        -0.039999998211860620, -0.019999999105930310,
        0.12500000000000000, -0.12500000000000000},
    {1, 26, -66230483057415LL,
        0.050000011920928955, -0.11666665971279144,
        0.0000000000000000, 0.0000000000000000,
        0.050000011920928955, -0.11666665971279144},
};

void expect(const char* id, int x, int z, double ex, double ey, double ez) {
    double ox = 1.0;
    double oy = 1.0;
    double oz = 1.0;
    if (!VanillaPlantOffset(id, x, z, ox, oy, oz) || ox != ex || oy != ey || oz != ez) {
        std::ostringstream message;
        message.setf(std::ios::fmtflags(0), std::ios::floatfield);
        message.precision(17);
        message << id << " @ " << x << "," << z << " got " << ox << " " << oy << " " << oz
                << " expected " << ex << " " << ey << " " << ez;
        throw std::runtime_error(message.str());
    }
}

void expectNone(const char* id, int x, int z) {
    double ox = 7.0;
    double oy = 8.0;
    double oz = 9.0;
    require(!VanillaPlantOffset(id, x, z, ox, oy, oz), std::string(id) + " should not offset");
    require(ox == 7.0 && oy == 8.0 && oz == 9.0, std::string(id) + " wrote an offset");
}

void checkSeedsAndOffsets() {
    for (const Row& row : kRows) {
        require(BlockPosSeed(row.x, 0, row.z) == row.seed, "BlockPosSeed != Mth.getSeed(x, 0, z)");
        expect("poppy", row.x, row.z, row.x25, 0.0, row.z25);
        expect("torchflower", row.x, row.z, row.x25, 0.0, row.z25);
        expect("bamboo", row.x, row.z, row.x25, 0.0, row.z25);
        expect("bamboo[age=1,leaves=large]", row.x, row.z, row.x25, 0.0, row.z25);
        expect("short_grass", row.x, row.z, row.x25, row.y02, row.z25);
        expect("grass", row.x, row.z, row.x25, row.y02, row.z25);
        expect("fern", row.x, row.z, row.x25, row.y02, row.z25);
        expect("pointed_dripstone", row.x, row.z, row.x125, 0.0, row.z125);
        expect("pointed_dripstone[thickness=tip,vertical_direction=down,waterlogged=true]",
               row.x, row.z, row.x125, 0.0, row.z125);
        expect("sulfur_spike", row.x, row.z, row.x125, 0.0, row.z125);
        expect("small_dripleaf", row.x, row.z, row.x25, row.y01, row.z25);
        expect("small_dripleaf[facing=north,half=lower,waterlogged=false]",
               row.x, row.z, row.x25, row.y01, row.z25);
    }
    // Block Y changes the model-variant seed and must not change the plant offset.
    require(BlockPosSeed(25, 151, 18) != BlockPosSeed(25, 0, 18),
            "variant seed unexpectedly ignored block y");
    require(BlockPosSeed(25, 154, 18) != BlockPosSeed(25, 151, 18),
            "two dripstone block y values hashed the same");
    expect("pointed_dripstone", 25, 18, -0.016666665673255920, 0.0, -0.016666665673255920);
    expect("tall_seagrass", 2, 52, 0.25000000000000000, 0.0, 0.25000000000000000);
    expect("hanging_roots", 1, 26, 0.050000011920928955, 0.0, -0.11666665971279144);
    expect("short_dry_grass", 59, 26, 0.25000000000000000, -0.18666666875282922, 0.083333343267440800);
    expect("golden_dandelion", 49, 14, -0.11666665971279144, 0.0, 0.083333343267440800);
}

void checkAbsent() {
    const char* ids[] = {
        "stone", "seagrass", "seagrass[half=lower]", "dead_bush", "oak_sapling",
        "pink_petals", "sugar_cane", "spore_blossom", "air", "",
    };
    for (const char* id : ids) expectNone(id, 2, 52);
    expectNone("seagrass", -1, 3);
    expectNone("dead_bush", 41, 26);
}

void checkVertexAdd() {
    const Row& grass = kRows[9];  // 59, 26
    ModelData model;
    model.vertices = {0.5f, 0.0f, 0.8f, 0.1f, 1.0f, 0.2f};
    ApplyVanillaPlantOffset(model, "minecraft", "short_grass[snowy=false]", grass.x, grass.z);
    const float fx = static_cast<float>(grass.x25);
    const float fy = static_cast<float>(grass.y02);
    const float fz = static_cast<float>(grass.z25);
    require(model.vertices[0] == 0.5f + fx, "float x add");
    require(model.vertices[1] == 0.0f + fy, "float y add");
    require(model.vertices[2] == 0.8f + fz, "float z add");
    require(model.vertices[3] == 0.1f + fx, "second vertex x");
    require(model.vertices[4] == 1.0f + fy, "second vertex y");
    require(model.vertices[5] == 0.2f + fz, "second vertex z");

    ModelData untouched = model;
    ApplyVanillaPlantOffset(untouched, "create", "short_grass", grass.x, grass.z);
    ApplyVanillaPlantOffset(untouched, "minecraft", "seagrass", grass.x, grass.z);
    ApplyVanillaPlantOffset(untouched, "minecraft", "dead_bush", grass.x, grass.z);
    require(untouched.vertices == model.vertices, "non-plant or other namespace moved");

    ModelData empty;
    ApplyVanillaPlantOffset(empty, "minecraft", "poppy", 0, 0);
    require(empty.vertices.empty(), "empty model grew vertices");

    ModelData flower;
    flower.vertices = {0.0f, 0.25f, 1.0f};
    ApplyVanillaPlantOffset(flower, "minecraft", "poppy", 0, 0);
    require(flower.vertices[0] == 0.0f + static_cast<float>(-0.25), "poppy x at origin");
    require(flower.vertices[1] == 0.25f, "XZ offset must leave y alone");
    require(flower.vertices[2] == 1.0f + static_cast<float>(-0.25), "poppy z at origin");
}

}  // namespace

int main() {
    try {
        checkSeedsAndOffsets();
        checkAbsent();
        checkVertexAdd();
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << std::endl;
        return 1;
    }
    std::cout << "plant offset ok" << std::endl;
    return 0;
}
