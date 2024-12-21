#include "config.h"
#include <fstream>
#include <sstream>
#include <iostream>

void WriteConfig(const Config& config, const std::string& configFile) {
    std::ofstream file(configFile);

    if (!file.is_open()) {
        std::cerr << "Could not open config file for writing: " << configFile << std::endl;
        return;
    }

    // 写入配置文件，按 key=value 格式
    file << "directoryPath = " << config.directoryPath << std::endl;
    file << "biomeMappingFile = " << config.biomeMappingFile << std::endl;
    file << "minX = " << config.minX << std::endl;
    file << "minY = " << config.minY << std::endl;
    file << "minZ = " << config.minZ << std::endl;
    file << "maxX = " << config.maxX << std::endl;
    file << "maxY = " << config.maxY << std::endl;
    file << "maxZ = " << config.maxZ << std::endl;
    file << "status = " << config.status << std::endl;

    std::cout << "Config written to " << configFile << std::endl;
}

Config LoadConfig(const std::string& configFile) {
    Config config;
    std::ifstream file(configFile);

    if (!file.is_open()) {
        std::cerr << "Could not open config file: " << configFile << std::endl;
        return config;  // 返回默认配置
    }

    std::string line;
    while (std::getline(file, line)) {
        std::stringstream ss(line);
        std::string key;
        std::string value;

        // 查找 '=' 符号，分割 key 和 value
        if (std::getline(ss, key, '=') && std::getline(ss, value)) {
            // 去除前后空格
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            // 根据 key 设置对应的 value
            if (key == "directoryPath") {
                config.directoryPath = value;
            }
            else if (key == "biomeMappingFile") {
                config.biomeMappingFile = value;
            }
            else if (key == "minX") {
                config.minX = std::stoi(value);
            }
            else if (key == "minY") {
                config.minY = std::stoi(value);
            }
            else if (key == "minZ") {
                config.minZ = std::stoi(value);
            }
            else if (key == "maxX") {
                config.maxX = std::stoi(value);
            }
            else if (key == "maxY") {
                config.maxY = std::stoi(value);
            }
            else if (key == "maxZ") {
                config.maxZ = std::stoi(value);
            }
            else if (key == "status") {
                config.status = std::stoi(value);
            }
        }
    }

    return config;
}
