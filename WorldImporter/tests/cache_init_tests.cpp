#include "GlobalCache.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

Config config;
namespace GlobalCache {
    extern std::unordered_map<std::string, nlohmann::json> atlases;
}

namespace {
using Bytes = std::vector<unsigned char>;
using Json = nlohmann::json;
using Text = std::string;
#define RESOURCE_MAPS(X) \
    X(textures, Bytes) X(blockstates, Json) X(models, Json) X(modelAssets, Text) \
    X(mcmetaCache, Json) X(biomes, Json) X(paintingVariants, Json) X(colormaps, Bytes) \
    X(ctmProperties, Text) X(ctmTextures, Bytes) X(atlases, Json)

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
struct Resources {
#define DECLARE(name, type) std::unordered_map<std::string, type> name;
    RESOURCE_MAPS(DECLARE)
#undef DECLARE
};
Resources scan(const std::filesystem::path& path, size_t range = 0, size_t count = 1) {
    JarReader reader(path.wstring());
    require(reader.open(), "open fixture");
    Resources result;
    reader.cacheAllResources(result.textures, result.blockstates, result.models,
        result.modelAssets, result.mcmetaCache, result.biomes, result.colormaps,
        result.ctmProperties, result.ctmTextures, result.atlases, result.paintingVariants,
        range, count);
    return result;
}
void merge(Resources& into, const Resources& from, const std::string& prefix = "") {
#define MERGE(name, type) for (const auto& [key, value] : from.name) into.name.try_emplace(prefix + key, value);
    RESOURCE_MAPS(MERGE)
#undef MERGE
}
void check(const Resources& actual, const Resources& expected) {
#define CHECK(name, type) require(actual.name == expected.name, #name " range scan differs");
    RESOURCE_MAPS(CHECK)
#undef CHECK
}

void makeArchive(const std::filesystem::path& path, int marker) {
    std::filesystem::create_directories(path.parent_path());
    int error = 0;
    zip_t* archive = zip_open(path.string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, &error);
    require(archive != nullptr, "create fixture");
    // Keep buffers alive until zip_close has consumed them.
    std::vector<std::pair<std::string, std::string>> entries;
    entries.emplace_back("version.json", R"({"id":"fixture"})");
    const std::string value = Json{{"marker", marker}}.dump();
    auto addResources = [&](const std::string& root, const std::string& text) {
        entries.emplace_back(root + "assets/test/textures/block/sample.png", text);
        entries.emplace_back(root + "assets/test/textures/item/sample.png", text);
        entries.emplace_back(root + "assets/test/textures/entity/sample.png", text);
        entries.emplace_back(root + "assets/test/textures/colormap/grass.png", text);
        entries.emplace_back(root + "assets/test/textures/block/sample.png.mcmeta", text);
        entries.emplace_back(root + "assets/test/textures/block/other.mcmeta", text);
        entries.emplace_back(root + "assets/test/blockstates/sample.json", text);
        entries.emplace_back(root + "assets/test/models/block/sample.json", text);
        entries.emplace_back(root + "assets/test/models/item/sample.json", text);
        entries.emplace_back(root + "assets/test/models/custom/sample.obj", text);
        entries.emplace_back(root + "assets/test/models/custom/sample.obj.mek", text);
        entries.emplace_back(root + "assets/test/models/custom/sample.mtl", text);
        entries.emplace_back(root + "assets/test/atlases/blocks.json", text);
        entries.emplace_back(root + "assets/test/optifine/ctm/sample/rule.properties", "# " + text);
        entries.emplace_back(root + "assets/test/optifine/ctm/sample/0.png", text);
        entries.emplace_back(root + "assets/test/optifine/ctm/sample/0.png.mcmeta", text);
        entries.emplace_back(root + "data/test/worldgen/biome/nested/sample.json", text);
        entries.emplace_back(root + "data/test/painting_variant/nested/sample.json", text);
    };
    addResources("", value);
    // Stored padding triggers the large-jar scheduler without asset assumptions.
    entries.emplace_back("ignored.bin", std::string(9 * 1024 * 1024, 'x'));
    // Duplicated logical keys in later ranges must keep the earlier values.
    addResources("wrapped/", R"({"marker":999})");
    entries.emplace_back("assets/test/textures/block/empty.png", "");
    entries.emplace_back("assets/test/models/custom/empty.obj", "");
    for (const auto& [name, content] : entries) {
        auto* source = zip_source_buffer(archive, content.data(), content.size(), 0);
        require(source != nullptr, "fixture source");
        const auto index = zip_file_add(archive, name.c_str(), source, ZIP_FL_ENC_UTF_8);
        require(index >= 0, "add fixture entry");
        require(zip_set_file_compression(archive, index, ZIP_CM_STORE, 0) == 0, "store fixture entry");
    }
    require(zip_close(archive) == 0, "close fixture");
}

template<class T>
void checkIndex(const std::unordered_map<std::string, std::string>& actual,
                const T& first, const char* kind, const char* jarId) {
    std::unordered_map<std::string, std::string> expected;
    for (const auto& [key, value] : first) {
        expected.emplace(std::string(kind) + ":" + key, std::string(jarId) + ":" + key);
    }
    require(actual == expected, std::string(kind) + " priority index differs");
}

void run(const std::filesystem::path& root) {
    const auto first = root / "a" / "pack.zip";
    const auto second = root / "b" / "pack.zip"; // Same jar id tests cache collisions.
    const auto vanilla = root / "vanilla.jar";
    makeArchive(first, 1);
    makeArchive(second, 2);
    makeArchive(vanilla, 3);
    const auto serial = scan(first);
    require(serial.textures.size() == 4 && serial.models.size() == 2 &&
        serial.ctmTextures.size() == 1 && serial.mcmetaCache.size() == 3 &&
        serial.modelAssets.size() == 4 && serial.biomes.size() == 1 &&
        serial.colormaps.size() == 1 && serial.blockstates.size() == 1 &&
        serial.ctmProperties.size() == 1 && serial.atlases.size() == 1 &&
        serial.paintingVariants.size() == 1, "fixture resource coverage");
    require(serial.models.at("test:block/sample").at("marker") == 1, "first logical entry wins");
    for (const size_t count : {2, 4, 7, 64}) {
        std::vector<Resources> parts(count);
        std::vector<std::thread> threads;
        for (size_t i = 0; i < count; ++i) {
            threads.emplace_back([&, i] { parts[i] = scan(first, i, count); });
        }
        for (auto& thread : threads) thread.join();
        Resources combined;
        for (const auto& part : parts) merge(combined, part);
        check(combined, serial);
    }
    for (const auto [range, count] : {std::pair<size_t, size_t>{0, 0}, {2, 2}}) {
        bool rejected = false;
        try { scan(first, range, count); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "invalid range rejected");
    }
    config.resourcepacksPaths = {first.string(), second.string()};
    config.jarPath = vanilla.string();
    InitializeAllCaches();
    Resources expected;
    merge(expected, serial, "pack:");
    merge(expected, scan(second), "pack:");
    merge(expected, scan(vanilla), "minecraft:");
#define CHECK_GLOBAL(name, type) require(GlobalCache::name == expected.name, #name " global merge differs");
    RESOURCE_MAPS(CHECK_GLOBAL)
#undef CHECK_GLOBAL
    checkIndex(GlobalCache::textureIndex, serial.textures, "textures", "pack");
    checkIndex(GlobalCache::blockstateIndex, serial.blockstates, "blockstates", "pack");
    checkIndex(GlobalCache::modelIndex, serial.models, "models", "pack");
    checkIndex(GlobalCache::modelAssetIndex, serial.modelAssets, "modelassets", "pack");
    checkIndex(GlobalCache::mcmetaIndex, serial.mcmetaCache, "mcmetas", "pack");
    checkIndex(GlobalCache::biomeIndex, serial.biomes, "biomes", "pack");
    checkIndex(GlobalCache::paintingVariantIndex, serial.paintingVariants, "painting_variants", "pack");
    checkIndex(GlobalCache::colormapIndex, serial.colormaps, "colormaps", "pack");
    checkIndex(GlobalCache::ctmPropertiesIndex, serial.ctmProperties, "ctmproperties", "pack");
    checkIndex(GlobalCache::ctmTexturesIndex, serial.ctmTextures, "ctmtextures", "pack");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("wi-cache-init-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        run(root);
        std::filesystem::remove_all(root);
        std::cout << "PASS: range reads, wrapped paths, empty entries and deterministic cache/index priority\n";
        return 0;
    } catch (const std::exception& e) {
        std::filesystem::remove_all(root);
        std::cerr << e.what() << '\n';
        return 1;
    }
}
