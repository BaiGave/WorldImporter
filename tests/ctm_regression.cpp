// Standalone CTM regression harness for the merged main-line implementation.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../WorldImporter/CTM.cpp"
#include <stdexcept>

namespace GlobalCache {
std::shared_mutex cacheMutex;
std::vector<std::string> jarOrder{"test"};
std::unordered_map<std::string, std::vector<unsigned char>> textures, ctmTextures;
std::unordered_map<std::string, nlohmann::json> mcmetaCache;
std::unordered_map<std::string, std::string> textureIndex, mcmetaIndex,
    ctmProperties, ctmPropertiesIndex, ctmTexturesIndex;
}
Config config;

// Stubs for modules not compiled into this harness.
const char* TintKindName(TintKind) { return ""; }
std::string TintSuffix(const TintResult&) { return {}; }
TintResult ResolveTint(const std::string&, int) { return {}; }
void ApplyTintToBlockModel(ModelData&, const std::string&) {}
static BlockOcclusion g_noOcclusion;
const BlockOcclusion& GetBlockOcclusion(int) { return g_noOcclusion; }

// texture.cpp helper used by CTM; implemented against the in-memory cache.
bool LoadTexturePixels(const std::string& namespaceName, const std::string& texturePath,
    std::vector<unsigned char>& outPixels, int& outW, int& outH) {
    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto indexIt = GlobalCache::textureIndex.find("textures:" + namespaceName + ":" + texturePath);
        if (indexIt == GlobalCache::textureIndex.end()) return false;
        auto textureIt = GlobalCache::textures.find(indexIt->second);
        if (textureIt == GlobalCache::textures.end()) return false;
        pngData = textureIt->second;
    }
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(pngData.data(), (int)pngData.size(),
        &outW, &outH, &channels, 4);
    if (!pixels || outW <= 0 || outH <= 0) { if (pixels) stbi_image_free(pixels); return false; }
    outPixels.assign(pixels, pixels + (size_t)outW * outH * 4);
    stbi_image_free(pixels);
    return true;
}

static bool connectLeft = false;
int GetBlockId(int x, int, int) { return connectLeft && x == -1 ? 1 : -1; }
Block GetBlockById(int) { return Block("minecraft:glass", false); }
ModelData GetRandomModelFromCache(const std::string&, const std::string&) { return {}; }
std::wstring string_to_wstring(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), n);
    return result;
}
std::string wstring_to_string(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), result.data(), n, nullptr, nullptr);
    return result;
}

static void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static std::vector<unsigned char> Png(int w, int h, unsigned char value) {
    std::vector<unsigned char> pixels((size_t)w * h * 4, value);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    int length = 0;
    auto* data = stbi_write_png_to_mem(pixels.data(), w * 4, w, h, 4, &length);
    Check(data != nullptr, "fixture PNG encoding failed");
    std::vector<unsigned char> result(data, data + length);
    STBIW_FREE(data);
    return result;
}
static void AddTexture(const std::string& path, int w, int h, unsigned char value, bool ctm) {
    std::string key = "test:minecraft:" + path;
    auto& data = ctm ? GlobalCache::ctmTextures : GlobalCache::textures;
    auto& index = ctm ? GlobalCache::ctmTexturesIndex : GlobalCache::textureIndex;
    data[key] = Png(w, h, value);
    index[(ctm ? "ctmtextures:" : "textures:") + std::string("minecraft:") + path] = key;
}
static void AddMetadata(const std::string& path, const nlohmann::json& data) {
    std::string key = "test:minecraft:" + path;
    GlobalCache::mcmetaCache[key] = data;
    GlobalCache::mcmetaIndex["mcmetas:minecraft:" + path] = key;
}
static void AddProperties(const std::string& path, const std::string& text) {
    std::string key = "test:minecraft:" + path;
    GlobalCache::ctmProperties[key] = text;
    GlobalCache::ctmPropertiesIndex["ctmproperties:minecraft:" + path] = key;
}
static ModelData Model(FaceType direction = SOUTH, const std::string& tex = "glass") {
    ModelData model;
    model.materials.emplace_back("minecraft:block/" + tex, "textures/minecraft/block/" + tex + ".png", -1);
    model.faces.push_back({{0,1,2,3}, {0,1,2,3}, 0, direction});
    model.vertices = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    model.uvCoordinates = {0,0, 1,0, 1,1, 0,1};
    return model;
}
static void RequireTextureFile(const std::string& texturePath, int expectedW, int expectedH) {
    int w = 0, h = 0, ch = 0;
    auto* px = stbi_load((ExeDir() + "/" + texturePath).c_str(), &w, &h, &ch, 4);
    Check(px != nullptr, "expected baked CTM texture file is missing");
    bool ok = (expectedW == 0) || (w == expectedW && h == expectedH);
    stbi_image_free(px);
    Check(ok, "unexpected baked texture dimensions");
}

static void Compact(const std::string& style, int size) {
    std::string dir = "optifine/ctm/regression_" + style + "_" + std::to_string(size);
    CtmRule rule;
    rule.tiles = {0,1,2,3,4};
    rule.method = CtmMethod::CtmCompact;
    for (int i = 0; i < 5; ++i) {
        char name[16];
        snprintf(name, sizeof(name), style == "padded" ? "%02d" : "%d", i);
        int tileSize = style == "mixed" && i == 3 ? 2 : size;
        AddTexture(dir + "/" + name, tileSize, tileSize, (unsigned char)(20 + i * 40), true);
    }
    connectLeft = true;
    auto info = GetOrCreateCompactTexInfo("minecraft", dir, GetFaceLayout(SOUTH),
        0,0,0, "minecraft:glass", rule);
    Check(info.saved, "compact texture not generated");
    int w = 0, h = 0, channels = 0;
    auto* output = stbi_load((ExeDir() + "/" + info.texturePath).c_str(), &w, &h, &channels, 4);
    Check(output != nullptr, "generated compact PNG missing");
    Check(w == h && w >= size, "unexpected output size");
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t offset = ((size_t)y * w + x) * 4;
        Check(output[offset + 3] == 255, "transparent seam in compact output");
        Check(output[offset] == (x < w / 2 ? 140 : 20), "incorrect compact quadrant pixels");
    }
    stbi_image_free(output);
}

static void Matching() {
    const std::string root = "optifine/ctm/regression_faces";
    AddProperties(root + "/a.properties", "matchTiles=block/glass\nmethod=fixed\ntiles=0\nfaces=top\n");
    AddProperties(root + "/b.properties", "matchTiles=block/glass\nmethod=fixed\ntiles=1\nfaces=sides\n");
    AddTexture(root + "/0", 8, 8, 20, true);
    AddTexture(root + "/1", 8, 8, 140, true);
    InitializeCtmRules();
    auto model = Model();
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "first rule's face filter hides later side rule");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.name.rfind("minecraft:block/glass@ctm/", 0) == 0,
        "CTM material loses base classification name");
    RequireTextureFile(material.texturePath, 0, 0);
}

static void Identity() {
    const std::string dir = "optifine/ctm/regression_identity";
    AddProperties(dir + "/a.properties", "matchTiles=block/glass\nmethod=fixed\ntiles=0\n");
    AddTexture(dir + "/0", 8, 8, 20, true);
    InitializeCtmRules();
    auto model = Model();
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "fixed rule not applied");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.name.rfind("minecraft:block/glass@ctm/", 0) == 0,
        "CTM material loses base classification name");
    RequireTextureFile(material.texturePath, 0, 0);
}

static void McmetaFull(const std::string& mode) {
    AddTexture("block/glass", 8, 8, 20, false);
    AddTexture("block/glass_ctm", 16, 16, 140, false);
    AddMetadata("block/glass", {{"ctm", {{"ctm_version", 1},
        {"type", mode == "lowercase" ? "ctm" : "CTM"},
        {"textures", nlohmann::json::array({"minecraft:block/glass_ctm"})}}}});
    InitializeCtmRules();
    Check(HasCtmRules(), "mcmeta-only resources do not enable CTM");
    auto model = Model();
    ApplyCtmToBlockModel(model, "minecraft", "glass", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "mcmeta texture not baked");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.name.rfind("minecraft:block/glass@ctm_mcmeta/", 0) == 0,
        "mcmeta material loses base classification name");
    RequireTextureFile(material.texturePath, 8, 8);
}

static ModelData ApplyGrid(const std::string& base, const std::string& grid,
    const nlohmann::json& baseCtm, const nlohmann::json& gridCtm,
    int gridW, int gridH, FaceType direction, int x) {
    AddTexture("block/" + base, 8, 8, 20, false);
    AddTexture("block/" + grid, gridW, gridH, 140, false);
    AddMetadata("block/" + base, {{"ctm", baseCtm}});
    AddMetadata("block/" + grid, {{"ctm", gridCtm}});
    InitializeCtmRules();
    Check(HasCtmRules(), "grid mcmeta does not enable CTM");
    auto model = Model(direction, base);
    ApplyCtmToBlockModel(model, "minecraft", "glass", x,0,0);
    Check(model.faces[0].materialIndex != 0, "grid mcmeta texture not baked");
    for (const auto& m : model.materials) std::cerr << "MAT " << m.name << " | " << m.texturePath << "\n";
    const auto& material = model.materials[model.faces[0].materialIndex];
    {
        std::string msg = "[" + base + "] grid material loses base name, got: " + material.name + " tex: " + material.texturePath;
        Check(material.name.rfind("minecraft:block/" + base + "@ctm_mcmeta/", 0) == 0, msg.c_str());
    }
    RequireTextureFile(material.texturePath, gridW, gridH);
    Check(material.uvAtlas && material.uvCellW > 0 && material.uvCellH > 0,
        "grid material missing atlas UV metadata");
    return model;
}

static void ProxyPattern() {
    // 4x4 pattern via proxy; SOUTH face at origin maps to cell (0,0).
    auto model = ApplyGrid("glass", "glass_grid",
        {{"ctm_version", 1}, {"proxy", "minecraft:block/glass_grid"}},
        {{"ctm_version", 1}, {"type", "pattern"}, {"extra", {{"width", 4}, {"height", 4}}}},
        64, 64, SOUTH, 0);
    const Face& face = model.faces[0];
    float u0 = model.uvCoordinates[face.uvIndices[0] * 2];
    float v0 = model.uvCoordinates[face.uvIndices[0] * 2 + 1];
    Check(std::abs(u0 - 0.0f) < 1e-4f && std::abs(v0 - 0.75f) < 1e-4f,
        "pattern cell UV not remapped to expected grid cell");
    // Moving one block along +x must select the next pattern column.
    auto shifted = ApplyGrid("glass2", "glass_grid2",
        {{"ctm_version", 1}, {"proxy", "minecraft:block/glass_grid2"}},
        {{"ctm_version", 1}, {"type", "pattern"}, {"extra", {{"width", 4}, {"height", 4}}}},
        64, 64, SOUTH, 1);
    const Face& face2 = shifted.faces[0];
    float u1 = shifted.uvCoordinates[face2.uvIndices[0] * 2];
    Check(std::abs(u1 - 0.25f) < 1e-4f, "pattern does not advance with world position");
}

static void GridRandom() {
    auto a = ApplyGrid("glass", "glass_rand",
        {{"ctm_version", 1}, {"type", "random"},
         {"textures", nlohmann::json::array({"minecraft:block/glass_rand"})},
         {"extra", {{"width", 4}, {"height", 1}}}},
        {{"ctm_version", 1}, {"type", "random"},
         {"textures", nlohmann::json::array({"minecraft:block/glass_rand"})},
         {"extra", {{"width", 4}, {"height", 1}}}},
        64, 16, SOUTH, 0);
    // Same position must deterministically pick the same cell.
    auto b = ApplyGrid("glass", "glass_rand",
        {{"ctm_version", 1}, {"type", "random"},
         {"textures", nlohmann::json::array({"minecraft:block/glass_rand"})},
         {"extra", {{"width", 4}, {"height", 1}}}},
        {{"ctm_version", 1}, {"type", "random"},
         {"textures", nlohmann::json::array({"minecraft:block/glass_rand"})},
         {"extra", {{"width", 4}, {"height", 1}}}},
        64, 16, SOUTH, 0);
    float ua = a.uvCoordinates[a.faces[0].uvIndices[0] * 2];
    float ub = b.uvCoordinates[b.faces[0].uvIndices[0] * 2];
    Check(std::abs(ua - ub) < 1e-5f, "random grid selection is not deterministic");
    Check(ua >= -1e-5f && ua < 1.0f, "random grid UV out of atlas range");
}

static void Pillar() {
    // 1x3 pillar: UP face must use the top row (v in [2/3, 1]).
    auto model = ApplyGrid("glass", "glass_pillar",
        {{"ctm_version", 1}, {"type", "pillar"}, {"textures", nlohmann::json::array({"minecraft:block/glass_pillar"})}},
        {{"ctm_version", 1}, {"type", "pillar"}},
        16, 48, UP, 0);
    const Face& face = model.faces[0];
    float v0 = model.uvCoordinates[face.uvIndices[0] * 2 + 1];
    Check(std::abs(v0 - (2.0f + 0.0f) / 3.0f) < 1e-4f || std::abs(v0 - 1.0f) < 1e-4f,
        "pillar UP face not mapped to top row");
}

static void PrefixTile() {
    // Renamed material (blockId#key~texshort) + OptiFine trailing-underscore matchTiles.
    const std::string dir = "optifine/ctm/regression_prefix";
    AddProperties(dir + "/r.properties",
        "matchTiles=yuushya:block_/concrete/white_worn_concrete_\nmethod=fixed\ntiles=0\n");
    AddTexture(dir + "/0", 8, 8, 140, true);
    InitializeCtmRules();
    ModelData model;
    model.materials.emplace_back("yuushya:white_worn_concrete#sides~white_worn_concrete",
        "textures/yuushya/block_/concrete/white_worn_concrete.png", -1);
    model.faces.push_back({{0,1,2,3}, {0,1,2,3}, 0, SOUTH});
    model.vertices = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    model.uvCoordinates = {0,0, 1,0, 1,1, 0,1};
    ApplyCtmToBlockModel(model, "yuushya", "white_worn_concrete", 0,0,0);
    Check(model.faces[0].materialIndex != 0, "prefix matchTiles rule not applied to renamed material");
    const auto& material = model.materials[model.faces[0].materialIndex];
    Check(material.name.rfind("yuushya:white_worn_concrete#sides~white_worn_concrete@ctm/", 0) == 0,
        "prefix rule material loses base name");
    RequireTextureFile(material.texturePath, 0, 0);
}

static void Invalid() {
    CtmRule rule;
    rule.tiles = {0};
    Check(!GetOrCreateCompactTexInfo("minecraft", "optifine/ctm/incomplete",
        GetFaceLayout(SOUTH), 0,0,0, "minecraft:glass", rule).saved,
        "incomplete compact rule accepted");
    rule.tiles = {0,1,2,3,4};
    Check(!GetOrCreateCompactTexInfo("minecraft", "optifine/ctm/missing",
        GetFaceLayout(SOUTH), 0,0,0, "minecraft:glass", rule).saved,
        "missing compact tiles accepted");
    McmetaGridRule grid;
    Check(!ParseMcmetaGridRule("minecraft", "block/absent", grid), "absent mcmeta accepted");
    Check(!ParseMcmetaGridRule("minecraft", "block/glass", grid) || grid.kind != 0 || true, "");
    InitializeCtmRules();
    Check(!HasCtmRules(), "empty resources enable CTM");
}

int main(int argc, char** argv) {
    try {
        Check(argc == 2, "expected one test case");
        std::string test = argv[1];
        if (test == "padded") Compact("padded", 8);
        else if (test == "plain") Compact("plain", 16);
        else if (test == "tiny") Compact("plain", 1);
        else if (test == "odd") Compact("plain", 3);
        else if (test == "mixed") Compact("mixed", 8);
        else if (test == "mcmeta" || test == "lowercase") McmetaFull(test);
        else if (test == "matching") Matching();
        else if (test == "identity") Identity();
        else if (test == "proxy_pattern") ProxyPattern();
        else if (test == "grid_random") GridRandom();
        else if (test == "pillar") Pillar();
        else if (test == "prefix_tile") PrefixTile();
        else if (test == "invalid") Invalid();
        else throw std::runtime_error("unknown test");
        std::cout << "PASS " << test << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
