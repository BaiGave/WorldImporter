#pragma once
#include <string>
#include <vector>
#include <unordered_set>
#include "blockstate.h" 
#include "nlohmann/json.hpp"

class SolidsJsonGenerator {
public:
    static void Generate(const std::string& outputPath, const std::vector<std::string>& targetParentPaths);

private:
    static std::string ExtractModelName(const std::string& modelId);
    static std::vector<std::string> GetParentPaths(const std::string& modelNamespace, const std::string& modelBlockId);
    static std::vector<std::vector<std::string>> GetAllParentPaths(const std::string& blockFullName);
};