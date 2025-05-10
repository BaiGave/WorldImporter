#include "RegionCache.h"
#include "fileutils.h"
#include "config.h"

std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache(1024);

const std::vector<char>& GetRegionFromCache(int regionX, int regionZ) {
    auto regionKey = std::make_pair(regionX, regionZ);
    auto it = regionCache.find(regionKey);
    if (it == regionCache.end()) {
        std::vector<char> fileData = ReadFileToMemory(config.worldPath, regionX, regionZ);
        auto result = regionCache.emplace(regionKey, std::move(fileData));
        it = result.first;
    }
    return it->second;
}