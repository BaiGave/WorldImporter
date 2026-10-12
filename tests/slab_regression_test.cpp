#include "ChunkGenerator.h"
#include "blockstate.h"
#include "CTM.h"
#include "Occlusion.h"
#include "locutil.h"
#include "biome.h"
#include "RegionCache.h"
#include <iostream>
Config config;
int main() {
    config.minX=0; config.maxX=15; config.minY=0; config.maxY=15;
    config.minZ=0; config.maxZ=15;
    config.useRandomBlockModels=false;
    InitializeGlobalBlockPalette();
    globalBlockPalette.emplace_back("minecraft:smooth_stone_slab[type:double,waterlogged:false]", true);
    globalBlockPalette.emplace_back("minecraft:smooth_stone_slab[type:top,waterlogged:false]", true);
    SectionCacheEntry section;
    section.blockData.assign(4096,0);
    section.blockData[toYZX(4,1,4)]=1;
    section.blockData[toYZX(4,1,3)]=2;
    section.blockData[toYZX(4,1,5)]=2;
    sectionCache[{0,0,AdjustSectionY(0)}]=std::move(section);
    // Cache an explicit full slab cube and neighboring upper slabs.
    ModelData cube;
    cube.vertices={0,0,0, 1,0,0, 1,1,0, 0,1,0, 0,0,1, 1,0,1, 1,1,1, 0,1,1};
    cube.uvCoordinates={0,0,1,0,1,1,0,1};
    cube.materials.emplace_back("minecraft:block/smooth_stone_slab_side","test.png",-1);
    cube.faces.push_back(Face{{0,3,2,1},{0,1,2,3},0,FaceType::NORTH});
    cube.faces.push_back(Face{{5,6,7,4},{0,1,2,3},0,FaceType::SOUTH});
    BlockModelCache["minecraft"][globalBlockPalette[1].GetModifiedName()]=cube;
    // An empty but present upper-slab model is sufficient for the neighbor test.
    BlockModelCache["minecraft"][globalBlockPalette[2].GetModifiedName()]=ModelData{};
    // Activate CTM without a matching rule: this is the old faulty condition.
    GlobalCache::ctmProperties["testjar:minecraft:optifine/ctm/test/test.properties"] =
        "matchBlocks=minecraft:glass\nmethod=fixed\ntiles=0\n";
    GlobalCache::ctmPropertiesIndex["ctmproperties:minecraft:optifine/ctm/test/test.properties"] =
        "testjar:minecraft:optifine/ctm/test/test.properties";
    InitializeCtmRules();
    if (!HasCtmRules()) return 2;
    globalPaletteFrozen.store(true); blockstateCachesFrozen.store(true);
    ChunkGenerator::PrepareBlockNames();
    auto result=ChunkGenerator::GenerateChunkModel(0,0,0);
    int sides=0;
    for(const auto& f:result.faces) if(f.faceDirection==FaceType::NORTH || f.faceDirection==FaceType::SOUTH) ++sides;
    std::cout << "double slab exposed sides: " << sides << '\n';
    // Frozen missing data must be treated as missing, never trigger region I/O.
    const auto sectionsBefore = sectionCache.size();
    const auto paletteBefore = globalBlockPalette.size();
    const auto regionsBefore = regionCache.size();
    config.worldPath = "missing_test_world";
    const int missingBiome = GetBiomeId(100000, -1, 100000);
    const int missingHeight = GetHeightMapY(100000, 100000, "MOTION_BLOCKING");
    const bool unchanged = sectionCache.size() == sectionsBefore &&
        globalBlockPalette.size() == paletteBefore && regionCache.size() == regionsBefore;
    std::cout << "frozen missing cache read: " << unchanged << '\n';
    return sides==2 && missingBiome==0 && missingHeight==-1 && unchanged ? 0:1;
}
