#include "GlobalCache.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>

Config config;

namespace {
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Yuushya 2.2.0 ships wire/*.json with an extra closing brace; Minecraft (Gson)
// stops after the first value, so the model still loads in game.
void run(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    const auto jar = root / "mod.jar";
    int error = 0;
    zip_t* archive = zip_open(jar.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    require(archive != nullptr, "create fixture");
    const std::vector<std::pair<std::string, std::string>> entries = {
        {"assets/test/models/wire/15.json",
         "{\r\n    \"parent\": \"test:wire/template\",\r\n      \"textures\": { \r\n      \"0\": \"test:wire/15\"}\r\n    }\r\n}\r\n"},
        {"assets/test/blockstates/wire.json", "{\"variants\":{\"\":{\"model\":\"test:wire/15\"}}}}"},
        {"assets/test/models/broken.json", "{\"parent\": }"},
    };
    for (const auto& [name, content] : entries) {
        auto* source = zip_source_buffer(archive, content.data(), content.size(), 0);
        require(source != nullptr && zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8) >= 0, "add entry");
    }
    require(zip_close(archive) == 0, "close fixture");

    JarReader reader(jar.wstring());
    require(reader.open(), "open fixture");
    std::unordered_map<std::string, std::vector<unsigned char>> textures, colormaps, ctmTextures;
    std::unordered_map<std::string, Json> blockstates, models, mcmeta, biomes, atlases, paintings;
    std::unordered_map<std::string, std::string> modelAssets, ctmProperties;
    reader.cacheAllResources(textures, blockstates, models, modelAssets, mcmeta, biomes,
        colormaps, ctmProperties, ctmTextures, atlases, paintings);
    require(models.count("test:wire/15") == 1, "model with trailing brace must load");
    require(models.at("test:wire/15").at("parent") == "test:wire/template", "model parent kept");
    require(models.at("test:wire/15").at("textures").at("0") == "test:wire/15", "model textures kept");
    require(blockstates.count("test:wire") == 1, "blockstate with trailing brace must load");
    require(models.count("test:broken") == 0, "invalid JSON still rejected");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("wi-resource-json-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        run(root);
        std::filesystem::remove_all(root);
        std::cout << "PASS: resource JSON ignores trailing data like Minecraft\n";
        return 0;
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
