#pragma once

#include <unordered_map>
#include <vector>
#include <utility>
#include "hashutils.h"
#include "config.h"

extern std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache;
const std::vector<char>& GetRegionFromCache(int regionX, int regionZ);