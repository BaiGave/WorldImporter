#pragma once
#include "block.h"
#include "biome.h"
#include "config.h"

class WorldAPI {
public:
    static int GetBlockId(int blockX, int blockY, int blockZ);
    static int GetHeightMapY(int blockX, int blockZ, const std::string& heightMapType);
    static int GetBiomeId(int blockX, int blockY, int blockZ);
    static int GetBiomeColor(int biomeId, BiomeColorType colorType);
    static Block GetBlockById(int blockId);
    
    static Config GetConfig();
    static void LoadConfig(const std::string& configFile);
    static void UpdateConfig(const Config& newConfig);
};