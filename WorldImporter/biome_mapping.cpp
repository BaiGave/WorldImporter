#include "biome_mapping.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

// Ⱥϵӳ���
std::unordered_map<std::string, int> biomeMapping;

void loadBiomeMapping(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open biome mapping file: " + filepath);
    }

    json j;
    file >> j;

    if (j.contains("biome_ids")) {
        for (auto& item : j["biome_ids"].items()) {
            std::string key = item.key();
            int value = item.value();
            biomeMapping[key] = value;
        }
    }
    else {
        throw std::runtime_error("Biome mapping file does not contain 'biome_ids'.");
    }
}


int getBiomeId(const std::string& biome) {
    auto it = biomeMapping.find(biome);
    if (it != biomeMapping.end()) {
        return it->second;
    }
    return -1;  // ���û���ҵ���Ӧ��ȺϵID������-1
}
