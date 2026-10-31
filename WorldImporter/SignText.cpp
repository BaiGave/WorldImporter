#include "SignText.h"
#include "signtext_hash.h"
#include "signtext_style.h"
#include "signtext_unihex.h"

#include "config.h"
#include "decompressor.h"
#include "fileutils.h"
#include "include/stb_image.h"
#include "include/stb_image_write.h"
#include "JarReader.h"
#include "texture.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <numbers>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Vanilla sign text. Bitmap and space providers are loaded first. unihex
// (1.20+) and legacy_unicode (1.18/1.19) supply the remaining codepoints. The
// first provider that has a codepoint wins. ttf is not loaded.
//
// Per-glyph style matches Font.StringRenderOutput (1.21.1): a style colour
// replaces the sign colour, bold draws the glyph again at +getBoldOffset and
// advances by that offset, italic shears by 1 - 0.25*y (top of an 8-unit glyph
// is +1 relative to the bottom). Underline and strikethrough are the solid
// Font effect rects. Obfuscated draws the real glyph. The 8-neighbour glow
// outline uses the darkened dye for every glyph (drawInBatch8xOutline replaces
// the style colour). translate/score/selector are not resolved.
//
// Hanging signs use the block entity's line height of 9 (standing and wall signs
// use 10). The bake hash includes that line height and the max line width so a
// hanging layout cannot reuse a standing texture.

namespace {

constexpr int kNbtTextDataVersion = 4325;

enum class SignKind { None, Standing, Wall, Hanging, WallHanging };
enum class StringMode { Json, Literal, Heuristic };

struct FontImage {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> pixels;
};

struct Glyph {
    int image = -1;
    int srcX = 0;
    int srcY = 0;
    int cellW = 0;
    int cellH = 0;
    float scale = 1.0f;
    int advance = 0;
    int ascent = 7;
    // 1 for bitmap. 2 for unihex and legacy_unicode (16 px = 8 font units).
    float oversample = 1.0f;
    // Legacy unicode page index, or -1. Pixels live in gLegacyPages[page].
    int page = -1;
    // Unihex rows, already shifted the way UnihexProvider.LineData.line() does.
    bool useBits = false;
    int bitLeft = 0;
    int bitRight = 0;
    std::array<uint32_t, 16> bits{};
};

struct FontLib {
    bool ok = false;
    std::vector<FontImage> images;
    std::unordered_map<uint32_t, Glyph> glyphs;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct PlacedGlyph {
    const Glyph* glyph = nullptr;
    float penX = 0.0f;
    float lineY = 0.0f;
    float advance = 0.0f;
    bool bold = false;
    bool italic = false;
    bool underlined = false;
    bool strikethrough = false;
    bool hasColor = false;
    int color = 0;
};

// Resolved style, tri-state inheritance (RawStyle), StyledLine and the colour
// helpers now live in signtext_style.h so they can be unit-tested.
using signtext::ApplyLegacyCode;
using signtext::DarkColor;
using signtext::DarkenRgb;
using signtext::DyeTextColor;
using signtext::NamedChatColor;
using signtext::ParseColorString;
using signtext::ParseHexColor;
using signtext::RawStyle;
using signtext::RunStyle;
using signtext::StyledLine;
using signtext::TextRun;

FontLib gFont;
std::once_flag gFontOnce;
int gDataVersion = -1;
bool gDataVersionKnown = false;
std::once_flag gDataVersionOnce;
std::mutex gBakeMutex;
std::unordered_set<uint64_t> gBaked;

struct SizeOverride {
    uint32_t from = 0;
    uint32_t to = 0;
    int left = 0;
    int right = 0;
};

struct PendingProvider {
    enum class Kind { Unihex, Legacy };
    Kind kind = Kind::Unihex;
    std::string id;
    std::string templ;
    std::vector<SizeOverride> overrides;
};

// HexLine and the unihex parsers now live in signtext_unihex.h.
using signtext::HexLine;

std::vector<PendingProvider> gPending;
std::unordered_map<uint32_t, Glyph> gFallback;
std::once_flag gFallbackOnce;
std::array<FontImage, 256> gLegacyPages;
std::array<std::once_flag, 256> gLegacyOnce;
std::string gLegacyTemplate;
std::mutex gLegacyMutex;

std::once_flag gAssetOnce;
std::string gAssetDir;
std::unordered_map<std::string, std::string> gAssetHash;

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<uint32_t> Codepoints(const std::string& text) {
    std::vector<uint32_t> out;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t n = text.size();
    for (size_t i = 0; i < n;) {
        unsigned char c = bytes[i];
        uint32_t cp = c;
        size_t len = 1;
        if ((c & 0x80u) == 0) {
            cp = c;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < n) {
            cp = (static_cast<uint32_t>(c & 0x1Fu) << 6) | (bytes[i + 1] & 0x3Fu);
            len = 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < n) {
            cp = (static_cast<uint32_t>(c & 0x0Fu) << 12) |
                 (static_cast<uint32_t>(bytes[i + 1] & 0x3Fu) << 6) |
                 (bytes[i + 2] & 0x3Fu);
            len = 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < n) {
            cp = (static_cast<uint32_t>(c & 0x07u) << 18) |
                 (static_cast<uint32_t>(bytes[i + 1] & 0x3Fu) << 12) |
                 (static_cast<uint32_t>(bytes[i + 2] & 0x3Fu) << 6) |
                 (bytes[i + 3] & 0x3Fu);
            len = 4;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

std::string TagString(const NbtTagPtr& tag) {
    if (!tag || tag->type != TagType::STRING) return {};
    return std::string(tag->payload.begin(), tag->payload.end());
}

bool TagFlag(const NbtTagPtr& tag) {
    if (!tag || tag->payload.empty()) return false;
    if (tag->type == TagType::BYTE) return static_cast<unsigned char>(tag->payload[0]) != 0;
    if (tag->type == TagType::SHORT && tag->payload.size() >= 2) return bytesToShort(tag->payload) != 0;
    if (tag->type == TagType::INT && tag->payload.size() >= 4) return bytesToInt(tag->payload) != 0;
    return false;
}

int JsonTri(const nlohmann::json& node, const char* key) {
    if (!node.contains(key)) return -1;
    const auto& value = node[key];
    if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
    if (value.is_number_integer()) return value.get<int>() != 0 ? 1 : 0;
    return -1;
}

RawStyle StyleFromJson(const nlohmann::json& node) {
    RawStyle style;
    if (!node.is_object()) return style;
    if (node.contains("color") && node["color"].is_string()) {
        int rgb = 0;
        if (ParseColorString(node["color"].get<std::string>(), rgb)) {
            style.hasColor = true;
            style.color = rgb;
        }
    }
    style.bold = JsonTri(node, "bold");
    style.italic = JsonTri(node, "italic");
    style.underlined = JsonTri(node, "underlined");
    style.strikethrough = JsonTri(node, "strikethrough");
    style.obfuscated = JsonTri(node, "obfuscated");
    return style;
}

int NbtTri(const NbtTagPtr& parent, const char* key) {
    const NbtTagPtr tag = getChildByName(parent, key);
    if (!tag || tag->payload.empty()) return -1;
    if (tag->type == TagType::BYTE) return static_cast<unsigned char>(tag->payload[0]) != 0 ? 1 : 0;
    if (tag->type == TagType::SHORT && tag->payload.size() >= 2) return bytesToShort(tag->payload) != 0 ? 1 : 0;
    if (tag->type == TagType::INT && tag->payload.size() >= 4) return bytesToInt(tag->payload) != 0 ? 1 : 0;
    return -1;
}

RawStyle StyleFromNbt(const NbtTagPtr& tag) {
    RawStyle style;
    if (!tag || tag->type != TagType::COMPOUND) return style;
    const NbtTagPtr color = getChildByName(tag, "color");
    if (color && color->type == TagType::STRING) {
        int rgb = 0;
        if (ParseColorString(TagString(color), rgb)) {
            style.hasColor = true;
            style.color = rgb;
        }
    } else if (color && color->type == TagType::INT && color->payload.size() >= 4) {
        style.hasColor = true;
        style.color = bytesToInt(color->payload) & 0xFFFFFF;
    }
    style.bold = NbtTri(tag, "bold");
    style.italic = NbtTri(tag, "italic");
    style.underlined = NbtTri(tag, "underlined");
    style.strikethrough = NbtTri(tag, "strikethrough");
    style.obfuscated = NbtTri(tag, "obfuscated");
    return style;
}

void AddRun(std::vector<TextRun>& runs, const std::string& text, const RunStyle& style) {
    if (text.empty()) return;
    runs.push_back(TextRun{text, style});
}

// Component.visit: this style applied onto the parent, then contents, then extra.
void AppendJson(const nlohmann::json& node, const RawStyle& parent, std::vector<TextRun>& runs) {
    if (node.is_string()) {
        AddRun(runs, node.get<std::string>(), parent.Resolve());
        return;
    }
    if (node.is_array()) {
        for (const auto& child : node) AppendJson(child, parent, runs);
        return;
    }
    if (!node.is_object()) return;
    const RawStyle style = StyleFromJson(node).ApplyTo(parent);
    const RunStyle resolved = style.Resolve();
    if (node.contains("text") && node["text"].is_string()) {
        AddRun(runs, node["text"].get<std::string>(), resolved);
    } else if (node.contains("translate") && node["translate"].is_string()) {
        AddRun(runs, node["translate"].get<std::string>(), resolved);
    }
    if (node.contains("extra")) AppendJson(node["extra"], style, runs);
}

void AppendNbt(const NbtTagPtr& tag, const RawStyle& parent, std::vector<TextRun>& runs) {
    if (!tag) return;
    if (tag->type == TagType::STRING) {
        AddRun(runs, TagString(tag), parent.Resolve());
        return;
    }
    if (tag->type == TagType::LIST) {
        for (const auto& child : tag->children) AppendNbt(child, parent, runs);
        return;
    }
    if (tag->type != TagType::COMPOUND) return;
    const RawStyle style = StyleFromNbt(tag).ApplyTo(parent);
    const RunStyle resolved = style.Resolve();
    const NbtTagPtr text = getChildByName(tag, "text");
    if (text && text->type == TagType::STRING) {
        AddRun(runs, TagString(text), resolved);
    } else {
        // 1.21.5+ stores a bare string component as {"": "literal"}.
        const NbtTagPtr literal = getChildByName(tag, "");
        if (literal && literal->type == TagType::STRING) {
            AddRun(runs, TagString(literal), resolved);
        } else {
            const NbtTagPtr translate = getChildByName(tag, "translate");
            if (translate && translate->type == TagType::STRING) AddRun(runs, TagString(translate), resolved);
        }
    }
    const NbtTagPtr extra = getChildByName(tag, "extra");
    if (extra) AppendNbt(extra, style, runs);
}

StyledLine ParseJsonLine(const std::string& raw) {
    StyledLine line;
    if (raw.empty()) return line;
    try {
        AppendJson(nlohmann::json::parse(raw), RawStyle{}, line.runs);
    } catch (const std::exception&) {
        line.runs.push_back(TextRun{raw, {}});
    }
    return line;
}

StyledLine DecodeStyledMessage(const NbtTagPtr& tag, StringMode mode) {
    StyledLine line;
    if (!tag) return line;
    if (tag->type == TagType::STRING) {
        const std::string raw = TagString(tag);
        const bool asJson = mode == StringMode::Json ||
            (mode == StringMode::Heuristic && !raw.empty() && (raw[0] == '"' || raw[0] == '{' || raw[0] == '['));
        if (!asJson) {
            if (!raw.empty()) line.runs.push_back(TextRun{raw, {}});
            return line;
        }
        return ParseJsonLine(raw);
    }
    if (tag->type == TagType::COMPOUND || tag->type == TagType::LIST) {
        AppendNbt(tag, RawStyle{}, line.runs);
    }
    return line;
}

void ReadFourLines(const NbtTagPtr& messages, StringMode mode, std::array<StyledLine, 4>& lines) {
    lines = {};
    if (!messages || messages->type != TagType::LIST) return;
    const size_t count = std::min<size_t>(4, messages->children.size());
    for (size_t i = 0; i < count; ++i) lines[i] = DecodeStyledMessage(messages->children[i], mode);
}

bool SideHasText(const std::array<StyledLine, 4>& lines) {
    for (const auto& line : lines) {
        if (!line.Plain().empty()) return true;
    }
    return false;
}

std::string ExeDir() {
    static std::string dir;
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t buffer[MAX_PATH];
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) return;
        const std::string exePath = wstring_to_string(std::wstring(buffer));
        const size_t pos = exePath.find_last_of("\\/");
        dir = (pos == std::string::npos) ? std::string() : exePath.substr(0, pos);
    });
    return dir;
}

void ReadDataVersion() {
    gDataVersion = -1;
    gDataVersionKnown = false;
    if (config.worldPath.empty()) return;
    const std::string path = JoinPath(config.worldPath, "level.dat");
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::vector<char> packed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (packed.empty()) return;
    std::vector<char> raw;
    if (!DecompressGzip(packed, raw) || raw.empty()) raw = std::move(packed);
    NbtTagPtr root;
    try {
        size_t index = 0;
        root = readTag(raw, index);
    } catch (const std::exception&) {
        return;
    }
    if (!root || root->type != TagType::COMPOUND) return;
    NbtTagPtr version = getChildByName(root, "DataVersion");
    if (!version) {
        const NbtTagPtr data = getChildByName(root, "Data");
        if (data && data->type == TagType::COMPOUND) version = getChildByName(data, "DataVersion");
    }
    if (!version || version->type != TagType::INT || version->payload.size() < 4) return;
    gDataVersion = bytesToInt(version->payload);
    gDataVersionKnown = true;
}

StringMode MessageStringMode() {
    std::call_once(gDataVersionOnce, ReadDataVersion);
    if (!gDataVersionKnown) return StringMode::Heuristic;
    if (gDataVersion >= kNbtTextDataVersion) return StringMode::Literal;
    return StringMode::Json;
}

std::string ProviderType(const nlohmann::json& provider) {
    if (!provider.contains("type") || !provider["type"].is_string()) return {};
    std::string type = provider["type"].get<std::string>();
    const size_t colon = type.rfind(':');
    if (colon != std::string::npos) type = type.substr(colon + 1);
    return type;
}

// Default font options are empty, so a filter value of true (uniform, jp, ...) excludes the provider.
bool IncludeInDefaultFont(const nlohmann::json& provider) {
    if (!provider.contains("filter") || !provider["filter"].is_object()) return true;
    const auto& filter = provider["filter"];
    for (auto it = filter.begin(); it != filter.end(); ++it) {
        if (it.value().is_boolean() && it.value().get<bool>()) return false;
    }
    return true;
}

std::string LexicalParent(std::string path) {
    while (path.size() > 1 && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const size_t pos = path.find_last_of("/\\");
    if (pos == std::string::npos) return {};
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string ReadFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Gson lenient accepts the trailing comma in 1.21.1's unifont.json. nlohmann does not.
std::string StripTrailingCommas(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    bool escape = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            out.push_back(c);
            if (escape) escape = false;
            else if (c == '\\') escape = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }
        if (c == ',') {
            size_t j = i + 1;
            while (j < text.size() && std::isspace(static_cast<unsigned char>(text[j]))) ++j;
            if (j < text.size() && (text[j] == '}' || text[j] == ']')) continue;
        }
        out.push_back(c);
    }
    return out;
}

bool ParseLenient(const std::string& text, nlohmann::json& out) {
    try {
        out = nlohmann::json::parse(text);
        return true;
    } catch (const std::exception&) {
    }
    try {
        out = nlohmann::json::parse(StripTrailingCommas(text));
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::string ReadAssetIndexId() {
    if (config.versionJsonPath.empty()) return {};
    const std::string text = ReadFileBytes(config.versionJsonPath);
    if (text.empty()) return {};
    nlohmann::json root;
    if (!ParseLenient(text, root)) return {};
    if (!root.contains("assetIndex") || !root["assetIndex"].is_object()) return {};
    const auto& index = root["assetIndex"];
    if (!index.contains("id") || !index["id"].is_string()) return {};
    return index["id"].get<std::string>();
}

std::vector<std::string> AssetDirs() {
    std::vector<std::string> dirs;
    auto add = [&dirs](const std::string& dir) {
        if (dir.empty()) return;
        if (std::find(dirs.begin(), dirs.end(), dir) == dirs.end()) dirs.push_back(dir);
    };
    auto addRoot = [&add](const std::string& file) {
        if (file.empty()) return;
        const std::string root = LexicalParent(LexicalParent(LexicalParent(file)));
        if (root.empty()) return;
        add(JoinPath(root, "assets"));
        const std::string up = LexicalParent(root);
        if (!up.empty()) add(JoinPath(up, "assets"));
    };
    addRoot(config.jarPath);
    addRoot(config.versionJsonPath);
    return dirs;
}

void EnsureAssetIndex() {
    std::call_once(gAssetOnce, [] {
        const std::string id = ReadAssetIndexId();
        for (const std::string& dir : AssetDirs()) {
            std::string indexPath;
            std::error_code ec;
            if (!id.empty()) {
                const std::string candidate = JoinPath(dir, "indexes", id + ".json");
                if (std::filesystem::is_regular_file(candidate, ec)) indexPath = candidate;
            }
            if (indexPath.empty()) {
                const std::string indexes = JoinPath(dir, "indexes");
                if (!std::filesystem::is_directory(indexes, ec)) continue;
                std::vector<std::string> names;
                for (const auto& entry : std::filesystem::directory_iterator(indexes, ec)) {
                    if (ec) break;
                    if (!entry.is_regular_file()) continue;
                    const std::string name = entry.path().filename().string();
                    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".json") == 0) names.push_back(name);
                }
                std::sort(names.begin(), names.end());
                for (const std::string& name : names) {
                    const std::string candidate = JoinPath(indexes, name);
                    const std::string text = ReadFileBytes(candidate);
                    if (text.find("\"minecraft/font/unifont.zip\"") == std::string::npos) continue;
                    indexPath = candidate;
                    break;
                }
            }
            if (indexPath.empty()) continue;
            nlohmann::json root;
            if (!ParseLenient(ReadFileBytes(indexPath), root)) continue;
            if (!root.contains("objects") || !root["objects"].is_object()) continue;
            gAssetDir = dir;
            gAssetHash.clear();
            for (auto it = root["objects"].begin(); it != root["objects"].end(); ++it) {
                if (!it.value().is_object() || !it.value().contains("hash") || !it.value()["hash"].is_string()) continue;
                gAssetHash.emplace(it.key(), it.value()["hash"].get<std::string>());
            }
            return;
        }
    });
}

std::string AssetFileForKey(const std::string& key) {
    EnsureAssetIndex();
    const auto it = gAssetHash.find(key);
    if (it == gAssetHash.end() || it->second.size() < 2 || gAssetDir.empty()) return {};
    const std::string path = JoinPath(gAssetDir, "objects", it->second.substr(0, 2), it->second);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return {};
    return path;
}

std::string JarPathKey(const std::string& jarPath) {
    const std::string prefix = "assets/";
    if (jarPath.rfind(prefix, 0) == 0) return jarPath.substr(prefix.size());
    return jarPath;
}

std::string ResourceAssetKey(const std::string& id) {
    std::string ns = "minecraft";
    std::string path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    return ns + "/" + path;
}

std::string ResourceJarPath(const std::string& id, bool textures) {
    std::string ns = "minecraft";
    std::string path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    if (textures && path.rfind("textures/", 0) != 0) path = "textures/" + path;
    return "assets/" + ns + "/" + path;
}

std::string ReadMaybeAssetText(JarReader& jar, const std::string& jarPath) {
    const std::string asset = AssetFileForKey(JarPathKey(jarPath));
    if (!asset.empty()) {
        const std::string text = ReadFileBytes(asset);
        if (!text.empty()) return text;
    }
    return jar.getFileContent(jarPath);
}

std::vector<unsigned char> ReadResourceBinary(JarReader* jar, const std::string& jarPath) {
    const std::string asset = AssetFileForKey(JarPathKey(jarPath));
    if (!asset.empty()) {
        const std::string text = ReadFileBytes(asset);
        if (!text.empty()) return std::vector<unsigned char>(text.begin(), text.end());
    }
    if (jar) return jar->getBinaryFileContent(jarPath);
    return {};
}

bool OneCodepoint(const nlohmann::json& value, uint32_t& cp) {
    if (!value.is_string()) return false;
    const std::vector<uint32_t> cps = Codepoints(value.get<std::string>());
    if (cps.size() != 1) return false;
    cp = cps[0];
    return true;
}

bool IsMainUnifont(const std::string& id) {
    const std::string suffix = "unifont.zip";
    if (id.size() < suffix.size() || id.compare(id.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    if (id.size() == suffix.size()) return true;
    const char prev = id[id.size() - suffix.size() - 1];
    return prev == ':' || prev == '/';
}

void LogUnifont(const std::string& path) {
    static std::once_flag once;
    std::call_once(once, [&path] {
        std::cerr << "[SignText] unifont: " << (path.empty() ? "not found" : path) << "\n";
    });
}

void AddPendingUnihex(const nlohmann::json& provider) {
    if (!provider.contains("hex_file") || !provider["hex_file"].is_string()) return;
    if (!provider.contains("size_overrides") || !provider["size_overrides"].is_array()) return;
    PendingProvider pending;
    pending.kind = PendingProvider::Kind::Unihex;
    pending.id = provider["hex_file"].get<std::string>();
    for (const auto& range : provider["size_overrides"]) {
        if (!range.is_object()) return;
        if (!range.contains("left") || !range["left"].is_number_integer()) return;
        if (!range.contains("right") || !range["right"].is_number_integer()) return;
        SizeOverride over;
        if (!OneCodepoint(range.contains("from") ? range["from"] : nlohmann::json(), over.from)) return;
        if (!OneCodepoint(range.contains("to") ? range["to"] : nlohmann::json(), over.to)) return;
        if (over.from >= over.to) return;
        over.left = range["left"].get<int>();
        over.right = range["right"].get<int>();
        pending.overrides.push_back(over);
    }
    gPending.push_back(std::move(pending));
}

void AddPendingLegacy(const nlohmann::json& provider) {
    if (!provider.contains("sizes") || !provider["sizes"].is_string()) return;
    if (!provider.contains("template") || !provider["template"].is_string()) return;
    const std::string templ = provider["template"].get<std::string>();
    if (templ.find("%s") == std::string::npos) return;
    PendingProvider pending;
    pending.kind = PendingProvider::Kind::Legacy;
    pending.id = provider["sizes"].get<std::string>();
    pending.templ = templ;
    gPending.push_back(std::move(pending));
}

void SplitResource(const std::string& id, std::string& ns, std::string& path) {
    ns = "minecraft";
    path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    if (path.rfind("textures/", 0) == 0) path = path.substr(9);
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0) path.resize(path.size() - 4);
}

std::string FontJsonPath(const std::string& id) {
    std::string ns = "minecraft";
    std::string path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    if (path.size() < 5 || path.compare(path.size() - 5, 5, ".json") != 0) path += ".json";
    return "assets/" + ns + "/font/" + path;
}

bool LoadFontImage(JarReader& jar, const std::string& ns, const std::string& texPath, FontImage& image) {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    if (LoadTexturePixels(ns, texPath, pixels, width, height) && width > 0 && height > 0) {
        image.width = width;
        image.height = height;
        image.pixels = std::move(pixels);
        return true;
    }
    const std::vector<unsigned char> file =
        jar.getBinaryFileContent("assets/" + ns + "/textures/" + texPath + ".png");
    if (file.empty()) return false;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);
    if (!decoded || width <= 0 || height <= 0) {
        if (decoded) stbi_image_free(decoded);
        return false;
    }
    image.width = width;
    image.height = height;
    image.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    stbi_image_free(decoded);
    return true;
}

int ActualGlyphWidth(const FontImage& image, int srcX, int srcY, int cellW, int cellH) {
    for (int x = cellW - 1; x >= 0; --x) {
        for (int y = 0; y < cellH; ++y) {
            const int px = srcX + x;
            const int py = srcY + y;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const size_t index = (static_cast<size_t>(py) * image.width + px) * 4 + 3;
            if (image.pixels[index] != 0) return x + 1;
        }
    }
    return 0;
}

void AddBitmapProvider(FontLib& font, JarReader& jar, const nlohmann::json& provider) {
    if (!provider.contains("file") || !provider["file"].is_string()) return;
    if (!provider.contains("chars") || !provider["chars"].is_array() || provider["chars"].empty()) return;
    if (!provider.contains("ascent") || !provider["ascent"].is_number()) return;

    int height = 8;
    if (provider.contains("height") && provider["height"].is_number()) height = provider["height"].get<int>();
    const int ascent = provider["ascent"].get<int>();
    if (height <= 0) return;

    std::vector<std::vector<uint32_t>> rows;
    rows.reserve(provider["chars"].size());
    size_t columns = 0;
    for (const auto& row : provider["chars"]) {
        if (!row.is_string()) return;
        rows.push_back(Codepoints(row.get<std::string>()));
        if (columns == 0) columns = rows.back().size();
        if (rows.back().size() != columns || columns == 0) return;
    }
    if (rows.empty() || columns == 0) return;

    std::string ns;
    std::string texPath;
    SplitResource(provider["file"].get<std::string>(), ns, texPath);
    FontImage image;
    if (!LoadFontImage(jar, ns, texPath, image)) return;
    const int cellW = image.width / static_cast<int>(columns);
    const int cellH = image.height / static_cast<int>(rows.size());
    if (cellW <= 0 || cellH <= 0) return;
    const float scale = static_cast<float>(height) / static_cast<float>(cellH);

    const int imageIndex = static_cast<int>(font.images.size());
    font.images.push_back(std::move(image));
    const FontImage& stored = font.images.back();

    for (size_t row = 0; row < rows.size(); ++row) {
        for (size_t col = 0; col < columns; ++col) {
            const uint32_t cp = rows[row][col];
            if (cp == 0 || font.glyphs.find(cp) != font.glyphs.end()) continue;
            const int srcX = static_cast<int>(col) * cellW;
            const int srcY = static_cast<int>(row) * cellH;
            const int raw = ActualGlyphWidth(stored, srcX, srcY, cellW, cellH);
            if (raw <= 0) continue;
            const float product = static_cast<float>(raw) * scale;
            Glyph glyph;
            glyph.image = imageIndex;
            glyph.srcX = srcX;
            glyph.srcY = srcY;
            glyph.cellW = cellW;
            glyph.cellH = cellH;
            glyph.scale = scale;
            glyph.advance = static_cast<int>(0.5 + static_cast<double>(product)) + 1;
            glyph.ascent = ascent;
            font.glyphs.emplace(cp, glyph);
        }
    }
}

void AddSpaceProvider(FontLib& font, const nlohmann::json& provider) {
    if (!provider.contains("advances") || !provider["advances"].is_object()) return;
    for (auto it = provider["advances"].begin(); it != provider["advances"].end(); ++it) {
        if (!it.value().is_number()) continue;
        const std::vector<uint32_t> cps = Codepoints(it.key());
        if (cps.empty() || cps[0] == 0) continue;
        if (font.glyphs.find(cps[0]) != font.glyphs.end()) continue;
        Glyph glyph;
        glyph.advance = it.value().get<int>();
        font.glyphs.emplace(cps[0], glyph);
    }
}

void LoadFontProviders(FontLib& font, JarReader& jar, const nlohmann::json& root, std::unordered_set<std::string>& seen, int depth);

void LoadFontFile(FontLib& font, JarReader& jar, const std::string& jarPath, std::unordered_set<std::string>& seen, int depth) {
    if (depth > 8 || !seen.insert(jarPath).second) return;
    const std::string content = ReadMaybeAssetText(jar, jarPath);
    if (content.empty()) return;
    nlohmann::json root;
    if (!ParseLenient(content, root)) return;
    LoadFontProviders(font, jar, root, seen, depth);
}

void LoadFontProviders(FontLib& font, JarReader& jar, const nlohmann::json& root, std::unordered_set<std::string>& seen, int depth) {
    if (root.is_object() && root.contains("providers") && root["providers"].is_array()) {
        for (const auto& provider : root["providers"]) {
            if (!provider.is_object() || !IncludeInDefaultFont(provider)) continue;
            const std::string type = ProviderType(provider);
            if (type == "reference") {
                if (provider.contains("id") && provider["id"].is_string())
                    LoadFontFile(font, jar, FontJsonPath(provider["id"].get<std::string>()), seen, depth + 1);
            } else if (type == "bitmap") {
                AddBitmapProvider(font, jar, provider);
            } else if (type == "space") {
                AddSpaceProvider(font, provider);
            } else if (type == "unihex") {
                AddPendingUnihex(provider);
            } else if (type == "legacy_unicode") {
                AddPendingLegacy(provider);
            }
        }
        return;
    }
    if (root.is_object() && root.contains("type")) {
        if (!IncludeInDefaultFont(root)) return;
        const std::string type = ProviderType(root);
        if (type == "bitmap") AddBitmapProvider(font, jar, root);
        else if (type == "space") AddSpaceProvider(font, root);
        else if (type == "unihex") AddPendingUnihex(root);
        else if (type == "legacy_unicode") AddPendingLegacy(root);
        else if (type == "reference" && root.contains("id") && root["id"].is_string())
            LoadFontFile(font, jar, FontJsonPath(root["id"].get<std::string>()), seen, depth + 1);
        return;
    }
    if (root.is_array()) {
        for (const auto& provider : root) {
            if (provider.is_object()) LoadFontProviders(font, jar, provider, seen, depth);
        }
    }
}

FontLib LoadFont() {
    FontLib font;
    if (config.jarPath.empty()) return font;
    JarReader jar(string_to_wstring(config.jarPath));
    if (!jar.open()) return font;
    std::unordered_set<std::string> seen;
    LoadFontFile(font, jar, "assets/minecraft/font/default.json", seen, 0);
    if (font.glyphs.find(32) == font.glyphs.end()) {
        Glyph space;
        space.advance = 4;
        font.glyphs.emplace(32, space);
    }
    font.ok = !font.glyphs.empty();
    if (!font.ok) std::cerr << "SignText: could not load assets/minecraft/font/default.json\n";
    return font;
}

const FontLib& Font() {
    std::call_once(gFontOnce, [] { gFont = LoadFont(); });
    return gFont;
}

std::string Hex64(uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016" PRIx64, value);
    return buffer;
}

std::string LegacyPageJarPath(const std::string& templ, int page) {
    const size_t pos = templ.find("%s");
    if (pos == std::string::npos) return {};
    char hex[8];
    std::snprintf(hex, sizeof(hex), "%02x", page & 255);
    const std::string id = templ.substr(0, pos) + hex + templ.substr(pos + 2);
    std::string ns = "minecraft";
    std::string path = id;
    const size_t colon = id.find(':');
    if (colon != std::string::npos) {
        ns = id.substr(0, colon);
        path = id.substr(colon + 1);
    }
    return "assets/" + ns + "/textures/" + path;
}

void BuildFallback();

void LoadLegacyPage(int page) {
    if (gLegacyTemplate.empty() || page < 0 || page >= 256) return;
    const std::string jarPath = LegacyPageJarPath(gLegacyTemplate, page);
    if (jarPath.empty() || config.jarPath.empty()) return;
    std::lock_guard<std::mutex> lock(gLegacyMutex);
    JarReader jar(string_to_wstring(config.jarPath));
    if (!jar.open()) return;
    const std::vector<unsigned char> file = ReadResourceBinary(&jar, jarPath);
    if (file.empty()) return;
    int width = 0;
    int height = 0;
    int channels = 0;
    unsigned char* decoded = stbi_load_from_memory(file.data(), static_cast<int>(file.size()), &width, &height, &channels, 4);
    if (!decoded) return;
    if (width == 256 && height == 256) {
        FontImage image;
        image.width = width;
        image.height = height;
        image.pixels.assign(decoded, decoded + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        gLegacyPages[page] = std::move(image);
    }
    stbi_image_free(decoded);
}

bool EnsureLegacyPage(int page) {
    if (page < 0 || page >= 256) return false;
    // gLegacyTemplate is only assigned by LoadLegacyProvider, which runs inside
    // BuildFallback. Without this, calling EnsureLegacyPage first burns the
    // once_flag on a no-op and the page can never load (call_once does not retry
    // after an early return). Build the fallback first so the template exists.
    std::call_once(gFallbackOnce, BuildFallback);
    if (gLegacyTemplate.empty()) return false;
    std::call_once(gLegacyOnce[page], [page] { LoadLegacyPage(page); });
    const FontImage& image = gLegacyPages[page];
    return image.width == 256 && image.height == 256;
}

void RememberGlyph(uint32_t cp, Glyph glyph) {
    if (gFont.glyphs.find(cp) != gFont.glyphs.end()) return;
    if (gFallback.find(cp) != gFallback.end()) return;
    gFallback.emplace(cp, std::move(glyph));
}

void LoadUnihexProvider(const PendingProvider& pending) {
    const std::string path = AssetFileForKey(ResourceAssetKey(pending.id));
    if (IsMainUnifont(pending.id)) LogUnifont(path);
    if (path.empty()) return;
    JarReader zip(string_to_wstring(path));
    if (!zip.open()) return;
    std::unordered_map<uint32_t, HexLine> lines;
    lines.reserve(131072);
    const std::vector<std::string> names = zip.getFilesInSubDirectory("");
    for (const std::string& name : names) {
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".hex") != 0) continue;
        const std::string text = zip.getFileContent(name);
        if (text.empty()) continue;
        if (!ParseHex(text, lines)) return;
    }
    for (const auto& entry : lines) {
        const uint32_t cp = entry.first;
        const HexLine& line = entry.second;
        if (gFont.glyphs.find(cp) != gFont.glyphs.end() || gFallback.find(cp) != gFallback.end()) continue;
        int left = 0;
        int right = 0;
        bool replaced = false;
        for (const SizeOverride& over : pending.overrides) {
            if (cp < over.from || cp > over.to) continue;
            left = over.left;
            right = over.right;
            replaced = true;
            break;
        }
        if (!replaced) {
            uint32_t mask = 0;
            for (uint32_t row : line.rows) mask |= row;
            if (mask == 0) {
                left = 0;
                right = static_cast<int>(static_cast<int8_t>(line.bitWidth));
            } else {
                const int lead = std::countl_zero(mask);
                const int trail = std::countr_zero(mask);
                left = static_cast<int>(static_cast<int8_t>(lead));
                right = static_cast<int>(static_cast<int8_t>(32 - trail - 1));
            }
        }
        const int width = right - left + 1;
        if (width <= 0) continue;
        Glyph glyph;
        glyph.useBits = true;
        glyph.bitLeft = left;
        glyph.bitRight = right;
        glyph.bits = line.rows;
        glyph.cellW = width;
        glyph.cellH = 16;
        glyph.scale = 0.5f;
        glyph.oversample = 2.0f;
        glyph.advance = width / 2 + 1;
        glyph.ascent = 7;
        RememberGlyph(cp, std::move(glyph));
    }
}

void LoadLegacyProvider(JarReader& jar, const PendingProvider& pending) {
    const std::vector<unsigned char> file = ReadResourceBinary(&jar, ResourceJarPath(pending.id, false));
    if (file.empty()) return;
    std::array<unsigned char, 65536> sizes{};
    std::memcpy(sizes.data(), file.data(), std::min<size_t>(file.size(), sizes.size()));
    if (gLegacyTemplate.empty()) gLegacyTemplate = pending.templ;
    for (int cp = 0; cp < 65536; ++cp) {
        const unsigned char s = sizes[cp];
        if (s == 0) continue;
        const int start = (s >> 4) & 15;
        const int end = (s & 15) + 1;
        if (start > end) continue;
        const int width = end - start;
        Glyph glyph;
        glyph.page = cp >> 8;
        glyph.srcX = (cp & 15) * 16 + start;
        glyph.srcY = ((cp >> 4) & 15) * 16;
        glyph.cellW = width;
        glyph.cellH = 16;
        glyph.scale = 0.5f;
        glyph.oversample = 2.0f;
        glyph.advance = width / 2 + 1;
        glyph.ascent = 7;
        RememberGlyph(static_cast<uint32_t>(cp), std::move(glyph));
    }
}

void BuildFallback() {
    JarReader jar(string_to_wstring(config.jarPath));
    const bool jarOpen = !config.jarPath.empty() && jar.open();
    for (const PendingProvider& pending : gPending) {
        if (pending.kind == PendingProvider::Kind::Unihex) LoadUnihexProvider(pending);
        else if (jarOpen) LoadLegacyProvider(jar, pending);
    }
}

const Glyph* FindGlyph(const FontLib& font, uint32_t cp) {
    const auto it = font.glyphs.find(cp);
    if (it != font.glyphs.end()) return &it->second;
    std::call_once(gFallbackOnce, BuildFallback);
    const auto fallback = gFallback.find(cp);
    if (fallback == gFallback.end()) return nullptr;
    if (fallback->second.page >= 0 && !EnsureLegacyPage(fallback->second.page)) return nullptr;
    return &fallback->second;
}

// GlyphInfo.getBoldOffset: 1 for bitmap, 0.5 for unihex / legacy (oversample 2).
float BoldOffset(const Glyph& glyph) {
    return glyph.oversample >= 1.5f ? 0.5f : 1.0f;
}

std::vector<PlacedGlyph> LayoutLine(const StyledLine& line, float lineY, int maxWidth, const FontLib& font) {
    struct Item {
        const Glyph* glyph = nullptr;
        RunStyle style;
        float advance = 0.0f;
    };
    std::vector<Item> items;
    float used = 0.0f;
    bool stop = false;
    for (const auto& run : line.runs) {
        if (stop) break;
        const std::vector<uint32_t> cps = Codepoints(run.text);
        RunStyle current = run.style;
        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == 0xA7u) {
                if (i + 1 >= cps.size()) {
                    stop = true;
                    break;
                }
                ApplyLegacyCode(current, run.style, cps[++i]);
                continue;
            }
            const Glyph* glyph = FindGlyph(font, cps[i]);
            if (!glyph) continue;
            const float advance = static_cast<float>(glyph->advance) + (current.bold ? BoldOffset(*glyph) : 0.0f);
            const float next = used + advance;
            if (!items.empty() && next > static_cast<float>(maxWidth)) {
                stop = true;
                break;
            }
            items.push_back(Item{glyph, current, advance});
            used = next;
        }
    }
    const int width = static_cast<int>(std::ceil(static_cast<double>(used)));
    float pen = static_cast<float>(-(width / 2));
    std::vector<PlacedGlyph> placed;
    placed.reserve(items.size());
    for (const Item& item : items) {
        PlacedGlyph stamp;
        stamp.glyph = item.glyph;
        stamp.penX = pen;
        stamp.lineY = lineY;
        stamp.advance = item.advance;
        stamp.bold = item.style.bold;
        stamp.italic = item.style.italic;
        stamp.underlined = item.style.underlined;
        stamp.strikethrough = item.style.strikethrough;
        stamp.hasColor = item.style.hasColor;
        stamp.color = item.style.color;
        placed.push_back(stamp);
        pen += item.advance;
    }
    return placed;
}

void Plot(std::vector<unsigned char>& canvas, int width, int height, int x, int y,
    int srcR, int srcG, int srcB, int srcA, int rgb) {
    if (x < 0 || y < 0 || x >= width || y >= height || srcA <= 0) return;
    unsigned char* dest = canvas.data() + (static_cast<size_t>(y) * width + x) * 4;
    const int r = srcR * ((rgb >> 16) & 255) / 255;
    const int g = srcG * ((rgb >> 8) & 255) / 255;
    const int b = srcB * (rgb & 255) / 255;
    if (srcA >= 255 || dest[3] == 0) {
        dest[0] = static_cast<unsigned char>(r);
        dest[1] = static_cast<unsigned char>(g);
        dest[2] = static_cast<unsigned char>(b);
        dest[3] = static_cast<unsigned char>(srcA);
        return;
    }
    const int inv = 255 - srcA;
    dest[0] = static_cast<unsigned char>((r * srcA + dest[0] * inv) / 255);
    dest[1] = static_cast<unsigned char>((g * srcA + dest[1] * inv) / 255);
    dest[2] = static_cast<unsigned char>((b * srcA + dest[2] * inv) / 255);
    dest[3] = static_cast<unsigned char>(srcA + dest[3] * inv / 255);
}

void BlitGlyph(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY,
    const PlacedGlyph& placed, float dx, float dy, int rgb, const FontLib& font) {
    const Glyph& glyph = *placed.glyph;
    if (glyph.image < 0 || glyph.image >= static_cast<int>(font.images.size())) return;
    const FontImage& image = font.images[glyph.image];
    const float gw = static_cast<float>(glyph.cellW) * glyph.scale;
    const float gh = static_cast<float>(glyph.cellH) * glyph.scale;
    if (gw <= 0.0f || gh <= 0.0f) return;
    const float ox = placed.penX + dx;
    const float oy = placed.lineY + static_cast<float>(7 - glyph.ascent) + dy;
    int x0 = static_cast<int>(std::floor(ox));
    const int y0 = static_cast<int>(std::floor(oy));
    int x1 = static_cast<int>(std::ceil(ox + gw));
    const int y1 = static_cast<int>(std::ceil(oy + gh));
    if (placed.italic) {
        const float localTop = static_cast<float>(7 - glyph.ascent);
        const float localBot = localTop + gh;
        const float s0 = 1.0f - 0.25f * localTop;
        const float s1 = 1.0f - 0.25f * localBot;
        const float sMin = s0 < s1 ? s0 : s1;
        const float sMax = s0 > s1 ? s0 : s1;
        x0 = static_cast<int>(std::floor(ox + sMin));
        x1 = static_cast<int>(std::ceil(ox + gw + sMax));
    }
    const int canvasH = canvas.empty() || canvasW <= 0 ? 0 : static_cast<int>(canvas.size() / (static_cast<size_t>(canvasW) * 4));
    for (int fy = y0; fy < y1; ++fy) {
        for (int fx = x0; fx < x1; ++fx) {
            float u;
            if (placed.italic) {
                const float localY = (static_cast<float>(fy) + 0.5f) - (placed.lineY + dy);
                const float shear = 1.0f - 0.25f * localY;
                u = ((static_cast<float>(fx) + 0.5f) - ox - shear) / gw;
            } else {
                u = ((static_cast<float>(fx) + 0.5f) - ox) / gw;
            }
            const float v = ((static_cast<float>(fy) + 0.5f) - oy) / gh;
            if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) continue;
            const int sx = std::min(glyph.cellW - 1, static_cast<int>(u * static_cast<float>(glyph.cellW)));
            const int sy = std::min(glyph.cellH - 1, static_cast<int>(v * static_cast<float>(glyph.cellH)));
            const int px = glyph.srcX + sx;
            const int py = glyph.srcY + sy;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const size_t index = (static_cast<size_t>(py) * image.width + px) * 4;
            const int a = image.pixels[index + 3];
            if (a == 0) continue;
            Plot(canvas, canvasW, canvasH, fx - originX, fy - originY,
                image.pixels[index], image.pixels[index + 1], image.pixels[index + 2], a, rgb);
        }
    }
}

bool GlyphDrawn(const Glyph& glyph) {
    if (glyph.cellW <= 0 || glyph.cellH <= 0) return false;
    if (glyph.useBits || glyph.page >= 0) return true;
    return glyph.image >= 0;
}

void BlitGlyphHi(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY,
    const PlacedGlyph& placed, float dx, float dy, int rgb, const FontLib& font) {
    const Glyph& glyph = *placed.glyph;
    const float gw = static_cast<float>(glyph.cellW) * glyph.scale;
    const float gh = static_cast<float>(glyph.cellH) * glyph.scale;
    if (gw <= 0.0f || gh <= 0.0f) return;
    const float ox = placed.penX + dx;
    const float oy = placed.lineY + static_cast<float>(7 - glyph.ascent) + dy;
    const int canvasH = canvas.empty() || canvasW <= 0 ? 0 : static_cast<int>(canvas.size() / (static_cast<size_t>(canvasW) * 4));

    // Oversample-2 glyphs are already 2 texels per font unit. Copy them 1:1.
    if (glyph.oversample >= 1.5f && (glyph.useBits || glyph.page >= 0)) {
        const int baseX = static_cast<int>(std::lround((ox - static_cast<float>(originX)) * 2.0f));
        const int baseY = static_cast<int>(std::lround((oy - static_cast<float>(originY)) * 2.0f));
        const FontImage* image = nullptr;
        if (glyph.page >= 0) {
            if (!EnsureLegacyPage(glyph.page)) return;
            image = &gLegacyPages[glyph.page];
        }
        for (int sy = 0; sy < glyph.cellH; ++sy) {
            for (int sx = 0; sx < glyph.cellW; ++sx) {
                int pr = 255;
                int pg = 255;
                int pb = 255;
                int pa = 0;
                if (glyph.useBits) {
                    const int bit = 32 - glyph.bitLeft - 1 - sx;
                    const uint32_t row = (sy >= 0 && sy < 16) ? glyph.bits[static_cast<size_t>(sy)] : 0u;
                    if (bit >= 0 && bit < 32 && ((row >> static_cast<unsigned>(bit)) & 1u) != 0) pa = 255;
                } else if (image) {
                    const int px = glyph.srcX + sx;
                    const int py = glyph.srcY + sy;
                    if (px < 0 || py < 0 || px >= image->width || py >= image->height) continue;
                    const size_t index = (static_cast<size_t>(py) * image->width + px) * 4;
                    pr = image->pixels[index];
                    pg = image->pixels[index + 1];
                    pb = image->pixels[index + 2];
                    pa = image->pixels[index + 3];
                }
                if (pa == 0) continue;
                int shiftX = 0;
                if (placed.italic) {
                    const float localY = static_cast<float>(7 - glyph.ascent) +
                        (static_cast<float>(sy) + 0.5f) / glyph.oversample;
                    shiftX = static_cast<int>(std::lround((1.0f - 0.25f * localY) * glyph.oversample));
                }
                Plot(canvas, canvasW, canvasH, baseX + sx + shiftX, baseY + sy, pr, pg, pb, pa, rgb);
            }
        }
        return;
    }

    // Bitmap glyphs on a 2x side: nearest-neighbour double of the 1x sample.
    if (glyph.image < 0 || glyph.image >= static_cast<int>(font.images.size())) return;
    const FontImage& image = font.images[glyph.image];
    int x0 = static_cast<int>(std::floor(ox));
    const int y0 = static_cast<int>(std::floor(oy));
    int x1 = static_cast<int>(std::ceil(ox + gw));
    const int y1 = static_cast<int>(std::ceil(oy + gh));
    if (placed.italic) {
        const float localTop = static_cast<float>(7 - glyph.ascent);
        const float localBot = localTop + gh;
        const float s0 = 1.0f - 0.25f * localTop;
        const float s1 = 1.0f - 0.25f * localBot;
        const float sMin = s0 < s1 ? s0 : s1;
        const float sMax = s0 > s1 ? s0 : s1;
        x0 = static_cast<int>(std::floor(ox + sMin));
        x1 = static_cast<int>(std::ceil(ox + gw + sMax));
    }
    for (int fy = y0; fy < y1; ++fy) {
        for (int fx = x0; fx < x1; ++fx) {
            float u;
            if (placed.italic) {
                const float localY = (static_cast<float>(fy) + 0.5f) - (placed.lineY + dy);
                const float shear = 1.0f - 0.25f * localY;
                u = ((static_cast<float>(fx) + 0.5f) - ox - shear) / gw;
            } else {
                u = ((static_cast<float>(fx) + 0.5f) - ox) / gw;
            }
            const float v = ((static_cast<float>(fy) + 0.5f) - oy) / gh;
            if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f) continue;
            const int sx = std::min(glyph.cellW - 1, static_cast<int>(u * static_cast<float>(glyph.cellW)));
            const int sy = std::min(glyph.cellH - 1, static_cast<int>(v * static_cast<float>(glyph.cellH)));
            const int px = glyph.srcX + sx;
            const int py = glyph.srcY + sy;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const size_t index = (static_cast<size_t>(py) * image.width + px) * 4;
            const int a = image.pixels[index + 3];
            if (a == 0) continue;
            const int cx = (fx - originX) * 2;
            const int cy = (fy - originY) * 2;
            for (int oy2 = 0; oy2 < 2; ++oy2) {
                for (int ox2 = 0; ox2 < 2; ++ox2) {
                    Plot(canvas, canvasW, canvasH, cx + ox2, cy + oy2,
                        image.pixels[index], image.pixels[index + 1], image.pixels[index + 2], a, rgb);
                }
            }
        }
    }
}

// Unstyled glyphs keep the original floor/ceil box. Styled glyphs also cover the
// bold copy, the italic shear, and the underline / strikethrough rects.
bool StampBox(const PlacedGlyph& stamp, bool hi, int margin, int& x0, int& y0, int& x1, int& y1) {
    const Glyph& glyph = *stamp.glyph;
    const bool pixels = hi ? GlyphDrawn(glyph) : glyph.image >= 0;
    const bool decorated = stamp.bold || stamp.italic || stamp.underlined || stamp.strikethrough;
    if (!pixels && !stamp.underlined && !stamp.strikethrough) return false;
    const float gw = static_cast<float>(glyph.cellW) * glyph.scale;
    const float gh = static_cast<float>(glyph.cellH) * glyph.scale;
    const float top = stamp.lineY + static_cast<float>(7 - glyph.ascent);
    if (!decorated) {
        x0 = static_cast<int>(std::floor(stamp.penX)) - margin;
        y0 = static_cast<int>(std::floor(top)) - margin;
        x1 = static_cast<int>(std::ceil(stamp.penX + gw)) + margin;
        y1 = static_cast<int>(std::ceil(top + gh)) + margin;
        return true;
    }
    float left = 0.0f;
    float right = 0.0f;
    float boxTop = 0.0f;
    float boxBot = 0.0f;
    bool any = false;
    auto add = [&](float l, float t, float r, float b) {
        if (!any) {
            left = l;
            boxTop = t;
            right = r;
            boxBot = b;
            any = true;
        } else {
            left = std::min(left, l);
            boxTop = std::min(boxTop, t);
            right = std::max(right, r);
            boxBot = std::max(boxBot, b);
        }
    };
    if (pixels) {
        float l = stamp.penX;
        float r = stamp.penX + gw;
        if (stamp.bold) r += BoldOffset(glyph);
        if (stamp.italic) {
            const float y0r = top - stamp.lineY;
            const float y1r = y0r + gh;
            const float s0 = 1.0f - 0.25f * y0r;
            const float s1 = 1.0f - 0.25f * y1r;
            l += s0 < s1 ? s0 : s1;
            r += s0 > s1 ? s0 : s1;
        }
        add(l, top, r, top + gh);
    }
    if (stamp.strikethrough || stamp.underlined) {
        const float xL = stamp.penX - 1.0f;
        const float xR = stamp.penX + stamp.advance;
        if (stamp.strikethrough) add(xL, stamp.lineY + 3.5f, xR, stamp.lineY + 4.5f);
        if (stamp.underlined) add(xL, stamp.lineY + 8.0f, xR, stamp.lineY + 9.0f);
    }
    if (!any) return false;
    x0 = static_cast<int>(std::floor(left)) - margin;
    y0 = static_cast<int>(std::floor(boxTop)) - margin;
    x1 = static_cast<int>(std::ceil(right)) + margin;
    y1 = static_cast<int>(std::ceil(boxBot)) + margin;
    return true;
}

void AccumulateBox(bool& any, int& minX, int& minY, int& maxX, int& maxY, int x0, int y0, int x1, int y1) {
    if (!any) {
        minX = x0;
        minY = y0;
        maxX = x1;
        maxY = y1;
        any = true;
    } else {
        minX = std::min(minX, x0);
        minY = std::min(minY, y0);
        maxX = std::max(maxX, x1);
        maxY = std::max(maxY, y1);
    }
}

int GlyphRgb(const PlacedGlyph& stamp, int textRgb, bool glowing) {
    if (stamp.hasColor) return stamp.color;
    return glowing ? textRgb : DarkColor(textRgb, false);
}

void BlitStyled(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY,
    const PlacedGlyph& stamp, float dx, float dy, int rgb, const FontLib& font, bool hi) {
    if (hi) BlitGlyphHi(canvas, canvasW, originX, originY, stamp, dx, dy, rgb, font);
    else BlitGlyph(canvas, canvasW, originX, originY, stamp, dx, dy, rgb, font);
    if (!stamp.bold) return;
    const float off = BoldOffset(*stamp.glyph);
    if (hi) BlitGlyphHi(canvas, canvasW, originX, originY, stamp, dx + off, dy, rgb, font);
    else BlitGlyph(canvas, canvasW, originX, originY, stamp, dx + off, dy, rgb, font);
}

void FillFontRect(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY, float scale,
    float x0, float y0, float x1, float y1, int rgb) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    int px0 = 0;
    int px1 = 0;
    int py0 = 0;
    int py1 = 0;
    if (scale <= 1.01f) {
        const int row = static_cast<int>(std::floor((y0 + y1) * 0.5f));
        py0 = row - originY;
        py1 = py0 + 1;
        px0 = static_cast<int>(std::floor(x0)) - originX;
        px1 = static_cast<int>(std::ceil(x1)) - originX;
    } else {
        px0 = static_cast<int>(std::floor((x0 - static_cast<float>(originX)) * scale));
        px1 = static_cast<int>(std::ceil((x1 - static_cast<float>(originX)) * scale));
        py0 = static_cast<int>(std::floor((y0 - static_cast<float>(originY)) * scale));
        py1 = static_cast<int>(std::ceil((y1 - static_cast<float>(originY)) * scale));
    }
    const int canvasH = canvas.empty() || canvasW <= 0 ? 0 : static_cast<int>(canvas.size() / (static_cast<size_t>(canvasW) * 4));
    for (int y = py0; y < py1; ++y) {
        for (int x = px0; x < px1; ++x) Plot(canvas, canvasW, canvasH, x, y, 255, 255, 255, 255, rgb);
    }
}

void DrawEffects(std::vector<unsigned char>& canvas, int canvasW, int originX, int originY, float scale,
    const std::vector<PlacedGlyph>& placed, int textRgb, bool glowing) {
    for (const auto& stamp : placed) {
        if (!stamp.underlined && !stamp.strikethrough) continue;
        const int rgb = GlyphRgb(stamp, textRgb, glowing);
        const float x0 = stamp.penX - 1.0f;
        const float x1 = stamp.penX + stamp.advance;
        if (stamp.strikethrough) {
            FillFontRect(canvas, canvasW, originX, originY, scale, x0, stamp.lineY + 3.5f, x1, stamp.lineY + 4.5f, rgb);
        }
        if (stamp.underlined) {
            FillFontRect(canvas, canvasW, originX, originY, scale, x0, stamp.lineY + 8.0f, x1, stamp.lineY + 9.0f, rgb);
        }
    }
}

bool BakeSideHi(const std::array<StyledLine, 4>& lines, const std::vector<PlacedGlyph>& placed, int textRgb, bool glowing,
    int lineHeight, int maxWidth, std::string& hexOut, float& xMin, float& yMin, float& xMax, float& yMax, const FontLib& font) {
    const int margin = glowing ? 1 : 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool any = false;
    for (const auto& stamp : placed) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        if (!StampBox(stamp, true, margin, x0, y0, x1, y1)) continue;
        AccumulateBox(any, minX, minY, maxX, maxY, x0, y0, x1, y1);
    }
    if (!any || maxX <= minX || maxY <= minY) return false;
    if (maxX - minX > 4096 || maxY - minY > 4096) return false;

    const int canvasW = (maxX - minX) * 2;
    const int canvasH = (maxY - minY) * 2;
    std::vector<unsigned char> canvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);
    const int outline = DarkColor(textRgb, true);
    if (glowing) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                for (const auto& stamp : placed) {
                    BlitStyled(canvas, canvasW, minX, minY, stamp, static_cast<float>(dx), static_cast<float>(dy), outline, font, true);
                }
            }
        }
    }
    for (const auto& stamp : placed) {
        BlitStyled(canvas, canvasW, minX, minY, stamp, 0.0f, 0.0f, GlyphRgb(stamp, textRgb, glowing), font, true);
    }
    DrawEffects(canvas, canvasW, minX, minY, 2.0f, placed, textRgb, glowing);

    int cropX0 = canvasW;
    int cropY0 = canvasH;
    int cropX1 = 0;
    int cropY1 = 0;
    for (int y = 0; y < canvasH; ++y) {
        for (int x = 0; x < canvasW; ++x) {
            if (canvas[(static_cast<size_t>(y) * canvasW + x) * 4 + 3] == 0) continue;
            cropX0 = std::min(cropX0, x);
            cropY0 = std::min(cropY0, y);
            cropX1 = std::max(cropX1, x + 1);
            cropY1 = std::max(cropY1, y + 1);
        }
    }
    if (cropX1 <= cropX0 || cropY1 <= cropY0) return false;

    int outW = cropX1 - cropX0;
    int outH = cropY1 - cropY0;
    if (outH > outW) outW = outH;
    std::vector<unsigned char> cropped(static_cast<size_t>(outW) * outH * 4, 0);
    for (int y = 0; y < cropY1 - cropY0; ++y) {
        const unsigned char* src = canvas.data() + (static_cast<size_t>(cropY0 + y) * canvasW + cropX0) * 4;
        unsigned char* dst = cropped.data() + static_cast<size_t>(y) * outW * 4;
        std::memcpy(dst, src, static_cast<size_t>(cropX1 - cropX0) * 4);
    }

    const uint64_t hash = HashText(lines, textRgb, glowing, lineHeight, maxWidth);
    const std::string hex = Hex64(hash);
    const std::string exe = ExeDir();
    if (exe.empty()) return false;
    const std::string relative = "textures/minecraft/sign_text/" + hex + ".png";
    const std::string full = JoinPath({exe, "textures", "minecraft", "sign_text", hex + ".png"});
    {
        std::lock_guard<std::mutex> lock(gBakeMutex);
        if (!gBaked.count(hash)) {
            try {
                std::filesystem::create_directories(std::filesystem::path(full).parent_path());
            } catch (const std::exception&) {
                return false;
            }
            if (stbi_write_png(full.c_str(), outW, outH, 4, cropped.data(), outW * 4) == 0) return false;
            RegisterTexture("minecraft", "sign_text/" + hex, relative);
            gBaked.insert(hash);
        }
    }

    hexOut = hex;
    xMin = static_cast<float>(minX) + static_cast<float>(cropX0) / 2.0f;
    yMin = static_cast<float>(minY) + static_cast<float>(cropY0) / 2.0f;
    xMax = xMin + static_cast<float>(outW) / 2.0f;
    yMax = yMin + static_cast<float>(outH) / 2.0f;
    return true;
}

bool BakeSide(const std::array<StyledLine, 4>& lines, int textRgb, bool glowing, int lineHeight, int maxWidth,
    std::string& hexOut, float& xMin, float& yMin, float& xMax, float& yMax) {
    const FontLib& font = Font();
    if (!font.ok) return false;

    std::vector<PlacedGlyph> placed;
    for (int i = 0; i < 4; ++i) {
        const float lineY = static_cast<float>(i * lineHeight - (4 * lineHeight / 2));
        auto line = LayoutLine(lines[i], lineY, maxWidth, font);
        placed.insert(placed.end(), line.begin(), line.end());
    }

    bool hiRes = false;
    for (const auto& stamp : placed) {
        if (stamp.glyph->oversample >= 1.5f && (stamp.glyph->useBits || stamp.glyph->page >= 0)) hiRes = true;
    }
    if (hiRes) return BakeSideHi(lines, placed, textRgb, glowing, lineHeight, maxWidth, hexOut, xMin, yMin, xMax, yMax, font);

    const int margin = glowing ? 1 : 0;
    int minX = 0;
    int minY = 0;
    int maxX = 0;
    int maxY = 0;
    bool any = false;
    for (const auto& stamp : placed) {
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
        if (!StampBox(stamp, false, margin, x0, y0, x1, y1)) continue;
        AccumulateBox(any, minX, minY, maxX, maxY, x0, y0, x1, y1);
    }
    if (!any || maxX <= minX || maxY <= minY) return false;

    const int canvasW = maxX - minX;
    const int canvasH = maxY - minY;
    if (canvasW > 4096 || canvasH > 4096) return false;
    std::vector<unsigned char> canvas(static_cast<size_t>(canvasW) * canvasH * 4, 0);

    const int outline = DarkColor(textRgb, true);
    if (glowing) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                for (const auto& stamp : placed) {
                    BlitStyled(canvas, canvasW, minX, minY, stamp, static_cast<float>(dx), static_cast<float>(dy), outline, font, false);
                }
            }
        }
    }
    for (const auto& stamp : placed) {
        BlitStyled(canvas, canvasW, minX, minY, stamp, 0.0f, 0.0f, GlyphRgb(stamp, textRgb, glowing), font, false);
    }
    DrawEffects(canvas, canvasW, minX, minY, 1.0f, placed, textRgb, glowing);

    int cropX0 = canvasW;
    int cropY0 = canvasH;
    int cropX1 = 0;
    int cropY1 = 0;
    for (int y = 0; y < canvasH; ++y) {
        for (int x = 0; x < canvasW; ++x) {
            if (canvas[(static_cast<size_t>(y) * canvasW + x) * 4 + 3] == 0) continue;
            cropX0 = std::min(cropX0, x);
            cropY0 = std::min(cropY0, y);
            cropX1 = std::max(cropX1, x + 1);
            cropY1 = std::max(cropY1, y + 1);
        }
    }
    if (cropX1 <= cropX0 || cropY1 <= cropY0) return false;

    int outW = cropX1 - cropX0;
    int outH = cropY1 - cropY0;
    // objrender treats a PNG with height > width and height % width == 0 as an
    // animated strip and keeps only the top square. Pad the right edge so the
    // bake stays one frame. The quad grows by the same empty columns.
    if (outH > outW) outW = outH;
    std::vector<unsigned char> cropped(static_cast<size_t>(outW) * outH * 4, 0);
    for (int y = 0; y < cropY1 - cropY0; ++y) {
        const unsigned char* src = canvas.data() + (static_cast<size_t>(cropY0 + y) * canvasW + cropX0) * 4;
        unsigned char* dst = cropped.data() + static_cast<size_t>(y) * outW * 4;
        std::memcpy(dst, src, static_cast<size_t>(cropX1 - cropX0) * 4);
    }

    const uint64_t hash = HashText(lines, textRgb, glowing, lineHeight, maxWidth);
    const std::string hex = Hex64(hash);
    const std::string exe = ExeDir();
    if (exe.empty()) return false;
    const std::string relative = "textures/minecraft/sign_text/" + hex + ".png";
    const std::string full = JoinPath({exe, "textures", "minecraft", "sign_text", hex + ".png"});

    {
        std::lock_guard<std::mutex> lock(gBakeMutex);
        if (!gBaked.count(hash)) {
            try {
                std::filesystem::create_directories(std::filesystem::path(full).parent_path());
            } catch (const std::exception&) {
                return false;
            }
            if (stbi_write_png(full.c_str(), outW, outH, 4, cropped.data(), outW * 4) == 0) return false;
            RegisterTexture("minecraft", "sign_text/" + hex, relative);
            gBaked.insert(hash);
        }
    }

    hexOut = hex;
    xMin = static_cast<float>(minX + cropX0);
    yMin = static_cast<float>(minY + cropY0);
    xMax = xMin + static_cast<float>(outW);
    yMax = yMin + static_cast<float>(outH);
    return true;
}

Vec3 RotY(Vec3 p, float degrees) {
    const float rad = degrees * (std::numbers::pi_v<float> / 180.0f);
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return Vec3{c * p.x + s * p.z, p.y, -s * p.x + c * p.z};
}

Vec3 TransformPoint(Vec3 p, float scale, const Vec3& offset, bool back, bool wall, bool hanging, float angle, float yBase) {
    p.x *= scale;
    p.y *= -scale;
    p.z *= scale;
    p.x += offset.x;
    p.y += offset.y;
    p.z += offset.z;
    if (back) p = RotY(p, 180.0f);
    // Hanging signs (ceiling and wall) share one local shift. Only a standing
    // wall sign uses the extra -Z that pulls the text onto the wall board.
    if (hanging) {
        p.y += -0.3125f;
    } else if (wall) {
        p.y += -0.3125f;
        p.z += -0.4375f;
    }
    p = RotY(p, -angle);
    p.x += 0.5f;
    p.y += yBase;
    p.z += 0.5f;
    return p;
}

void AddQuad(ModelData& out, const Vec3 corners[4], int materialIndex) {
    const int vbase = static_cast<int>(out.vertices.size() / 3);
    const int ubase = static_cast<int>(out.uvCoordinates.size() / 2);
    for (int i = 0; i < 4; ++i) {
        out.vertices.push_back(corners[i].x);
        out.vertices.push_back(corners[i].y);
        out.vertices.push_back(corners[i].z);
    }
    // v = 1 is the top of the PNG. objrender treats vt v = 0 as the bottom row.
    const float uvs[4][2] = {{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (const auto& uv : uvs) {
        out.uvCoordinates.push_back(uv[0]);
        out.uvCoordinates.push_back(uv[1]);
    }
    Face face{};
    face.vertexIndices = {vbase, vbase + 1, vbase + 2, vbase + 3};
    face.uvIndices = {ubase, ubase + 1, ubase + 2, ubase + 3};
    face.materialIndex = materialIndex;
    face.faceDirection = FaceType::DO_NOT_CULL;
    face.tintIndex = -1;
    face.bothSided = false;
    face.thinEdge = false;
    out.faces.push_back(face);
}

int AddMaterial(ModelData& out, const std::string& name, const std::string& texturePath) {
    for (size_t i = 0; i < out.materials.size(); ++i) {
        if (out.materials[i].name == name) return static_cast<int>(i);
    }
    Material material;
    material.name = name;
    material.texturePath = texturePath;
    material.tintIndex = -1;
    material.type = NORMAL;
    material.aspectRatio = 1.0f;
    out.materials.push_back(material);
    return static_cast<int>(out.materials.size() - 1);
}

SignKind Classify(const std::string& base) {
    if (base.size() >= 18 && base.compare(base.size() - 18, 18, "_wall_hanging_sign") == 0) return SignKind::WallHanging;
    if (base.size() >= 13 && base.compare(base.size() - 13, 13, "_hanging_sign") == 0) return SignKind::Hanging;
    if (base.size() >= 10 && base.compare(base.size() - 10, 10, "_wall_sign") == 0) return SignKind::Wall;
    if (base.size() >= 5 && base.compare(base.size() - 5, 5, "_sign") == 0) return SignKind::Standing;
    return SignKind::None;
}

std::unordered_map<std::string, std::string> ParseState(const std::string& state) {
    std::unordered_map<std::string, std::string> props;
    size_t i = 0;
    while (i < state.size()) {
        const size_t comma = state.find(',', i);
        const std::string part = state.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
        const size_t eq = part.find('=');
        if (eq != std::string::npos) props[part.substr(0, eq)] = part.substr(eq + 1);
        if (comma == std::string::npos) break;
        i = comma + 1;
    }
    return props;
}

bool ReadSide(const NbtTagPtr& side, StringMode mode, std::array<StyledLine, 4>& lines, int& rgb, bool& glowing) {
    if (!side || side->type != TagType::COMPOUND) return false;
    ReadFourLines(getChildByName(side, "messages"), mode, lines);
    rgb = 0;
    const NbtTagPtr color = getChildByName(side, "color");
    if (color && color->type == TagType::STRING) rgb = DyeTextColor(TagString(color));
    glowing = TagFlag(getChildByName(side, "has_glowing_text"));
    return SideHasText(lines);
}

void EmitSide(ModelData& out, const std::array<StyledLine, 4>& lines, int textRgb, bool glowing,
    int lineHeight, int maxWidth, float scale, const Vec3& offset, bool back, bool wall, bool hanging,
    float angle, float yBase, int x, int y, int z) {
    std::string hex;
    float xMin = 0.0f;
    float yMin = 0.0f;
    float xMax = 0.0f;
    float yMax = 0.0f;
    if (!BakeSide(lines, textRgb, glowing, lineHeight, maxWidth, hex, xMin, yMin, xMax, yMax)) return;
    const std::string name = "minecraft:sign_text#" + hex;
    const std::string texture = "textures/minecraft/sign_text/" + hex + ".png";
    const int material = AddMaterial(out, name, texture);
    const Vec3 local[4] = {
        {xMin, yMin, 0.0f},
        {xMax, yMin, 0.0f},
        {xMax, yMax, 0.0f},
        {xMin, yMax, 0.0f},
    };
    Vec3 world[4];
    for (int i = 0; i < 4; ++i) {
        world[i] = TransformPoint(local[i], scale, offset, back, wall, hanging, angle, yBase);
        world[i].x += static_cast<float>(x);
        world[i].y += static_cast<float>(y);
        world[i].z += static_cast<float>(z);
    }
    AddQuad(out, world, material);
}

}  // namespace

bool BuildSignTextModel(const std::string& ns, const std::string& blockNameWithState,
    const NbtTagPtr& blockEntityNbt, int x, int y, int z, ModelData& out) {
    if (ns != "minecraft" || !blockEntityNbt || blockEntityNbt->type != TagType::COMPOUND) return false;

    std::string base = blockNameWithState;
    std::string state;
    const size_t bracket = base.find('[');
    if (bracket != std::string::npos) {
        state = base.substr(bracket + 1);
        base = base.substr(0, bracket);
        if (!state.empty() && state.back() == ']') state.pop_back();
    }
    const SignKind kind = Classify(base);
    if (kind == SignKind::None) return false;

    const auto props = ParseState(state);
    const bool wall = kind == SignKind::Wall || kind == SignKind::WallHanging;
    const bool hanging = kind == SignKind::Hanging || kind == SignKind::WallHanging;
    float angle = 0.0f;
    if (wall) {
        const auto facing = props.find("facing");
        const std::string face = facing == props.end() ? "north" : facing->second;
        if (face == "south") angle = 0.0f;
        else if (face == "west") angle = 90.0f;
        else if (face == "east") angle = 270.0f;
        else angle = 180.0f;
    } else {
        int rotation = 0;
        const auto it = props.find("rotation");
        if (it != props.end()) {
            try {
                rotation = std::stoi(it->second);
            } catch (const std::exception&) {
                rotation = 0;
            }
        }
        angle = static_cast<float>(rotation) * 22.5f;
    }

    const int lineHeight = hanging ? 9 : 10;
    const int maxWidth = hanging ? 60 : 90;
    const float scale = hanging ? (0.015625f * 0.9f) : (0.015625f * 0.6666667f);
    const float yBase = hanging ? 0.9375f : (0.75f * 0.6666667f);
    const Vec3 offset = hanging ? Vec3{0.0f, -0.32f, 0.073f} : Vec3{0.0f, 0.33333334f, 0.046666667f};

    const size_t facesBefore = out.faces.size();
    const NbtTagPtr front = getChildByName(blockEntityNbt, "front_text");
    if (front && front->type == TagType::COMPOUND) {
        const StringMode mode = MessageStringMode();
        std::array<StyledLine, 4> lines;
        int rgb = 0;
        bool glowing = false;
        if (ReadSide(front, mode, lines, rgb, glowing)) {
            EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, false, wall, hanging, angle, yBase, x, y, z);
        }
        const NbtTagPtr back = getChildByName(blockEntityNbt, "back_text");
        if (back && back->type == TagType::COMPOUND && ReadSide(back, mode, lines, rgb, glowing)) {
            EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, true, wall, hanging, angle, yBase, x, y, z);
        }
    } else {
        std::array<StyledLine, 4> lines;
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            lines[i] = ParseJsonLine(TagString(getChildByName(blockEntityNbt, "Text" + std::to_string(i + 1))));
            if (!lines[i].Plain().empty()) any = true;
        }
        if (!any) return false;
        int rgb = 0;
        const NbtTagPtr color = getChildByName(blockEntityNbt, "Color");
        if (color && color->type == TagType::STRING) rgb = DyeTextColor(TagString(color));
        const bool glowing = TagFlag(getChildByName(blockEntityNbt, "GlowingText"));
        EmitSide(out, lines, rgb, glowing, lineHeight, maxWidth, scale, offset, false, wall, hanging, angle, yBase, x, y, z);
    }
    return out.faces.size() > facesBefore;
}
