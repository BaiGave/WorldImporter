#include "SolidsJsonGenerator.h"
#include "GlobalCache.h"
#include <mutex>
#include <fstream>

void SolidsJsonGenerator::Generate(const std::string& outputPath, const std::vector<std::string>& targetParentPaths) {
    std::unordered_set<std::string> solidBlocks;

    std::unordered_map<std::string, nlohmann::json> allBlockstates;
    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        allBlockstates = GlobalCache::blockstates;
    }

    for (const auto& [blockFullName, blockstateJson] : allBlockstates) {
        auto allParentPaths = GetAllParentPaths(blockFullName);

        bool allSolid = true;
        for (const auto& parentPaths : allParentPaths) {
            bool currentSolid = false;
            for (const auto& parentPath : parentPaths) {
                std::string normalizedParent = ExtractModelName(parentPath);
                for (const auto& target : targetParentPaths) {
                    if (normalizedParent.find(target) != std::string::npos) {
                        currentSolid = true;
                        break;
                    }
                }
                if (currentSolid) break;
            }
            if (!currentSolid) {
                allSolid = false;
                break;
            }
        }

        if (allSolid) {
            size_t statePos = blockFullName.find('[');
            std::string fullId = (statePos != std::string::npos) ? blockFullName.substr(0, statePos) : blockFullName;
            solidBlocks.insert(fullId);
        }
    }

    nlohmann::json j;
    j["solid_blocks"] = solidBlocks;
    std::ofstream file(outputPath);
    if (file.is_open()) {
        file << j.dump(4);
    }
}

std::string SolidsJsonGenerator::ExtractModelName(const std::string& modelId) {
    size_t colonPos = modelId.find(':');
    return (colonPos != std::string::npos) ? modelId.substr(colonPos + 1) : modelId;
}


std::vector<std::string> SolidsJsonGenerator::GetParentPaths(const std::string& modelNamespace, const std::string& modelBlockId) {
    std::vector<std::string> parentPaths;
    nlohmann::json currentModelJson = GetModelJson(modelNamespace, modelBlockId);
    while (true) {
        if (currentModelJson.contains("parent")) {
            const std::string parentModelId = currentModelJson["parent"].get<std::string>();
            parentPaths.push_back(parentModelId);

            size_t parentColonPos = parentModelId.find(':');
            std::string parentNamespace = (parentColonPos != std::string::npos) ? parentModelId.substr(0, parentColonPos) : "minecraft";
            std::string parentBlockId = (parentColonPos != std::string::npos) ? parentModelId.substr(parentColonPos + 1) : parentModelId;

            currentModelJson = GetModelJson(parentNamespace, parentBlockId);
        }
        else {
            break;
        }
    }
    return parentPaths;
}


std::vector<std::vector<std::string>> SolidsJsonGenerator::GetAllParentPaths(const std::string& blockFullName) {
    size_t colonPos = blockFullName.find(':');
    std::string namespaceName = (colonPos != std::string::npos) ? blockFullName.substr(0, colonPos) : "minecraft";
    std::string blockId = (colonPos != std::string::npos) ? blockFullName.substr(colonPos + 1) : blockFullName;

    size_t statePos = blockId.find('[');
    std::string baseBlockId = (statePos != std::string::npos) ? blockId.substr(0, statePos) : blockId;

    nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);
    std::vector<std::vector<std::string>> allParentPaths;

    if (blockstateJson.contains("variants")) {
        std::vector<std::string> modelIds;
        for (const auto& variant : blockstateJson["variants"].items()) {
            if (variant.value().is_array()) {
                for (const auto& item : variant.value()) {
                    if (item.contains("model")) {
                        modelIds.push_back(item["model"].get<std::string>());
                    }
                }
            }
            else {
                if (variant.value().contains("model")) {
                    modelIds.push_back(variant.value()["model"].get<std::string>());
                }
            }
        }

        for (const auto& modelId : modelIds) {
            size_t modelColonPos = modelId.find(':');
            std::string modelNamespace = (modelColonPos != std::string::npos) ? modelId.substr(0, modelColonPos) : "minecraft";
            std::string modelBlockId = (modelColonPos != std::string::npos) ? modelId.substr(modelColonPos + 1) : modelId;

            std::vector<std::string> parentPaths = GetParentPaths(modelNamespace, modelBlockId);
            parentPaths.push_back(modelId);
            allParentPaths.push_back(parentPaths);
        }
    }
    else if (blockstateJson.contains("multipart")) {
        allParentPaths.push_back({ blockFullName });
    }
    else {
        allParentPaths.push_back({ blockFullName });
    }

    return allParentPaths;
}