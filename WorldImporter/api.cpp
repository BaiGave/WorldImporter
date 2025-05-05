#include "api.h"
#include "block.h"
#include "biome.h"

int WorldAPI::GetBlockId(int blockX, int blockY, int blockZ) {
    return ::GetBlockId(blockX, blockY, blockZ);
}

int WorldAPI::GetHeightMapY(int blockX, int blockZ, const std::string& heightMapType) {
    return ::GetHeightMapY(blockX, blockZ, heightMapType);
}

int WorldAPI::GetBiomeId(int blockX, int blockY, int blockZ) {
    return ::GetBiomeId(blockX, blockY, blockZ);
}

int WorldAPI::GetBiomeColor(int biomeId, BiomeColorType colorType) {
    return Biome::GetColor(biomeId, colorType);
}

Block WorldAPI::GetBlockById(int blockId) {
    return ::GetBlockById(blockId);
}

Config WorldAPI::GetConfig() {
    return config;
}

void WorldAPI::LoadConfig(const std::string& configFile) {
    config = ::LoadConfig(configFile);
}

void WorldAPI::UpdateConfig(const Config& newConfig) {
    config = newConfig;
}