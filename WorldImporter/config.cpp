#include "config.h" 
#include <codecvt> 
#include <locale>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

// 写入配置到文件
void WriteConfig(const Config& config, const std::string& configFile) {
    std::ofstream file(configFile);

    if (!file.is_open()) {
        std::cerr << "Could not open config file for writing: " << configFile << std::endl;
        return;
    }

    // 写入全局配置
    file << "worldPath = " << config.worldPath << std::endl;
    file << "packagePath = " << config.packagePath << std::endl;
    file << "biomeMappingFile = " << config.biomeMappingFile << std::endl;
    file << "minX = " << config.minX << std::endl;
    file << "minY = " << config.minY << std::endl;
    file << "minZ = " << config.minZ << std::endl;
    file << "maxX = " << config.maxX << std::endl;
    file << "maxY = " << config.maxY << std::endl;
    file << "maxZ = " << config.maxZ << std::endl;
    file << "status = " << config.status << std::endl;
    file << "importByChunk = " << (config.importByChunk ? "1" : "0") << std::endl;
    file << "importByBlockType = " << (config.importByBlockType ? "1" : "0") << std::endl;
    file << "pointCloudType = " << config.pointCloudType << std::endl;
    file << "lodLevel = " << config.lodLevel << std::endl;
    file << "importFilePath = " << config.importFilePath << std::endl;
    file << "selectedGameVersion = " << config.selectedGameVersion << std::endl;

    // 写入每个版本的配置
    for (const auto& versionConfig : config.versionConfigs) {
        file << "[" << versionConfig.first << "]" << std::endl;
        const VersionConfig& version = versionConfig.second;
        file << "gameFolderPath = " << version.gameFolderPath << std::endl;
        file << "minecraftVersion = " << version.minecraftVersion << std::endl;
        file << "modLoaderType = " << version.modLoaderType << std::endl;

        // 写入模组列表
        file << "modList = ";
        for (size_t i = 0; i < version.modList.size(); ++i) {
            file << version.modList[i];
            if (i < version.modList.size() - 1)
                file << ";";
        }
        file << std::endl;

        // 写入资源包列表
        file << "resourcePackList = ";
        for (size_t i = 0; i < version.resourcePackList.size(); ++i) {
            file << version.resourcePackList[i];
            if (i < version.resourcePackList.size() - 1)
                file << ";";
        }
        file << std::endl;

        // 写入存档列表
        file << "saveGameList = ";
        for (size_t i = 0; i < version.saveGameList.size(); ++i) {
            file << version.saveGameList[i];
            if (i < version.saveGameList.size() - 1)
                file << ";";
        }
        file << std::endl;
    }

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
    std::string currentVersion;
    VersionConfig currentVersionConfig;

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

            if (key == "worldPath") {
                config.worldPath = value;
            }
            else if (key == "packagePath") {
                config.packagePath = value;
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
            else if (key == "importByChunk") {
                config.importByChunk = (value == "1");
            }
            else if (key == "importByBlockType") {
                config.importByBlockType = (value == "1");
            }
            else if (key == "pointCloudType") {
                config.pointCloudType = std::stoi(value);
            }
            else if (key == "lodLevel") {
                config.lodLevel = std::stoi(value);
            }
            else if (key == "importFilePath") {
                config.importFilePath = value;
            }
            else if (key == "selectedGameVersion") {
                config.selectedGameVersion = value;
            }
        }

        if (line.find('[') == 0 && line.find(']') != std::string::npos) {
            // 如果已有版本，保存当前版本配置
            if (!currentVersion.empty()) {
                // 将当前版本配置存入 config.versionConfigs
                if (!currentVersionConfig.gameFolderPath.empty()) {
                    config.versionConfigs[currentVersion] = currentVersionConfig;
                }
            }

            // 获取版本名并开始解析新的版本块
            size_t startPos = line.find('[') + 1;
            size_t endPos = line.find(']');
            if (startPos != std::string::npos && endPos != std::string::npos) {
                currentVersion = line.substr(startPos, endPos - startPos);
                currentVersionConfig = VersionConfig();  // 重置版本配置
            }
        }


        // 解析版本块中的配置项
        if (!currentVersion.empty()) {
            if (line.find("gameFolderPath") != std::string::npos) {
                currentVersionConfig.gameFolderPath = value;
            }
            else if (line.find("minecraftVersion") != std::string::npos) {
                currentVersionConfig.minecraftVersion = value;
            }
            else if (line.find("modLoaderType") != std::string::npos) {
                currentVersionConfig.modLoaderType = value;
            }
            else if (line.find("modList") != std::string::npos) {
                std::stringstream modListStream(value);
                std::string mod;
                while (std::getline(modListStream, mod, ';')) {
                    currentVersionConfig.modList.push_back(mod);
                }
            }
            else if (line.find("resourcePackList") != std::string::npos) {
                std::stringstream resourcePackListStream(value);
                std::string pack;
                while (std::getline(resourcePackListStream, pack, ';')) {
                    currentVersionConfig.resourcePackList.push_back(pack);
                }
            }
            else if (line.find("saveGameList") != std::string::npos) {
                std::stringstream saveGameListStream(value);
                std::string save;
                while (std::getline(saveGameListStream, save, ';')) {
                    currentVersionConfig.saveGameList.push_back(save);
                }
            }
        }
    }

    // 在文件结束时保存最后一个版本配置
    if (!currentVersion.empty()) {
        if (!currentVersionConfig.gameFolderPath.empty()) {
            config.versionConfigs[currentVersion] = currentVersionConfig;
        }
    }

    return config;
}
