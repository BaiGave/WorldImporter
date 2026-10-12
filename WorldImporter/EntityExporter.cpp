#include "EntityExporter.h"
#include "config.h"
#include "nbtutils.h"
#include "decompressor.h"
#include "include/json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <vector>
#include <cstdint>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
uint32_t ReadBE32(const char* p) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 8) |
            static_cast<uint32_t>(static_cast<unsigned char>(p[3]));
}

fs::path GetEntitiesDirectory() {
    fs::path world(config.worldPath);
    const std::string& dim = config.selectedDimension;
    if (dim == "minecraft:the_nether" || dim == "the_nether" || dim == "DIM-1")
        return world / "DIM-1" / "entities";
    if (dim == "minecraft:the_end" || dim == "the_end" || dim == "DIM1")
        return world / "DIM1" / "entities";
    if (dim.empty() || dim == "minecraft:overworld" || dim == "overworld")
        return world / "entities";

    size_t colon = dim.find(':');
    std::string ns = colon == std::string::npos ? "minecraft" : dim.substr(0, colon);
    std::string path = colon == std::string::npos ? dim : dim.substr(colon + 1);
    fs::path result = world / "dimensions" / ns;
    std::stringstream ss(path); std::string part;
    while (std::getline(ss, part, '/')) if (!part.empty()) result /= part;
    return result / "entities";
}

bool ReadChunkNbt(const std::vector<char>& region, size_t slot, NbtTagPtr& root) {
    size_t hp = slot * 4;
    if (hp + 4 > region.size()) return false;
    uint32_t loc = ReadBE32(region.data() + hp);
    if (!loc) return false;
    size_t offset = static_cast<size_t>(loc >> 8) * 4096;
    if (offset + 5 > region.size()) return false;
    uint32_t length = ReadBE32(region.data() + offset);
    if (length < 2 || offset + 4 + length > region.size()) return false;
    unsigned char compression = static_cast<unsigned char>(region[offset + 4]);
    std::vector<char> packed(region.begin() + offset + 5, region.begin() + offset + 4 + length);
    std::vector<char> raw;
    if (compression == 2) {
        if (!DecompressData(packed, raw)) return false;
    } else if (compression == 3) {
        raw = std::move(packed);
    } else if (compression == 1) {
        if (!DecompressGzip(packed, raw)) return false;
    } else {
        // 未知压缩类型,跳过该区块
        return false;
    }
    try {
        size_t index = 0;
        root = readTag(raw, index);
        return static_cast<bool>(root);
    } catch (...) { return false; }
}

bool GetNumber(const NbtTagPtr& tag, double& value) {
    if (!tag) return false;
    switch (tag->type) {
    case TagType::DOUBLE: value = bytesToDouble(tag->payload); return true;
    case TagType::FLOAT: value = bytesToFloat(tag->payload); return true;
    case TagType::INT: value = bytesToInt(tag->payload); return true;
    case TagType::SHORT: value = bytesToShort(tag->payload); return true;
    case TagType::BYTE: value = bytesToByte(tag->payload); return true;
    default: return false;
    }
}

json NumberList(const NbtTagPtr& tag) {
    json out = json::array();
    if (!tag || tag->type != TagType::LIST) return out;
    for (const auto& child : tag->children) {
        double v; if (GetNumber(child, v)) out.push_back(v);
    }
    return out;
}

std::string UuidString(const NbtTagPtr& entity) {
    auto uuid = getChildByName(entity, "UUID");
    if (!uuid || uuid->type != TagType::INT_ARRAY) return "";
    auto values = readIntArray(uuid->payload);
    if (values.size() != 4) return "";
    std::ostringstream s; s << std::hex << std::setfill('0');
    s << std::setw(8) << static_cast<uint32_t>(values[0]) << '-'
      << std::setw(4) << (static_cast<uint32_t>(values[1]) >> 16) << '-'
      << std::setw(4) << (static_cast<uint32_t>(values[1]) & 0xffff) << '-'
      << std::setw(4) << (static_cast<uint32_t>(values[2]) >> 16) << '-'
      << std::setw(4) << (static_cast<uint32_t>(values[2]) & 0xffff)
      << std::setw(8) << static_cast<uint32_t>(values[3]);
    return s.str();
}

void AddScalar(json& out, const NbtTagPtr& entity, const char* name, const char* outputName = nullptr) {
    auto tag = getChildByName(entity, name); if (!tag) return;
    std::string key = outputName ? outputName : name;
    if (tag->type == TagType::STRING) out[key] = getStringTag(tag);
    else { double v; if (GetNumber(tag, v)) out[key] = v; }
}

void ExportEntityRecursive(const NbtTagPtr& entity, json& output) {
    if (!entity || entity->type != TagType::COMPOUND) return;
    auto idTag = getChildByName(entity, "id");
    auto posTag = getChildByName(entity, "Pos");
    if (!idTag || !posTag) return;
    std::string id = getStringTag(idTag);
    json pos = NumberList(posTag);
    if (id.empty() || pos.size() < 3) return;
    double x = pos[0].get<double>(), y = pos[1].get<double>(), z = pos[2].get<double>();
    if (x < config.minX || x > config.maxX + 1.0 ||
        y < config.minY || y > config.maxY + 1.0 ||
        z < config.minZ || z > config.maxZ + 1.0) return;

    json e;
    e["id"] = id;
    e["pos"] = pos;
    e["rotation"] = NumberList(getChildByName(entity, "Rotation"));
    std::string uuid = UuidString(entity); if (!uuid.empty()) e["uuid"] = uuid;
    AddScalar(e, entity, "CustomName", "custom_name");
    AddScalar(e, entity, "Age", "age");
    AddScalar(e, entity, "Variant", "variant");
    if (!e.contains("variant")) AddScalar(e, entity, "variant", "variant");
    AddScalar(e, entity, "CatType", "cat_type");
    AddScalar(e, entity, "CollarColor", "collar_color");
    AddScalar(e, entity, "NoAI", "no_ai");
    AddScalar(e, entity, "Silent", "silent");
    AddScalar(e, entity, "Invisible", "invisible");
    AddScalar(e, entity, "Health", "health");
    auto villager = getChildByName(entity, "VillagerData");
    if (villager) {
        json vd; AddScalar(vd, villager, "type"); AddScalar(vd, villager, "profession"); AddScalar(vd, villager, "level");
        if (!vd.empty()) e["villager_data"] = vd;
    }
    output.push_back(std::move(e));

    auto passengers = getChildByName(entity, "Passengers");
    if (passengers) for (const auto& p : passengers->children) ExportEntityRecursive(p, output);
}
}

namespace EntityExporter {
size_t Export(const std::string& outputPath) {
    json result;
    result["format"] = 1;
    result["dimension"] = config.selectedDimension;
    result["entities"] = json::array();
    fs::path dir = GetEntitiesDirectory();
    if (!fs::exists(dir)) {
        std::ofstream out(outputPath);
        if (out) out << result.dump(2);
        else std::cerr << "[Entities] 无法写入: " << outputPath << std::endl;
        std::cout << "[Entities] Directory not found: " << dir.string() << std::endl;
        return 0;
    }
    size_t chunks = 0;
    for (const auto& entry : fs::directory_iterator(dir)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".mca") continue;
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
            std::cerr << "[Entities] 无法打开区域文件: " << entry.path().string() << std::endl;
            continue;
        }
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t slot = 0; slot < 1024; ++slot) {
            NbtTagPtr root;
            if (!ReadChunkNbt(bytes, slot, root)) continue;
            ++chunks;
            auto list = getChildByName(root, "Entities");
            if (!list) list = getChildByName(root, "entities");
            if (!list) continue;
            for (const auto& e : list->children) ExportEntityRecursive(e, result["entities"]);
        }
    }
    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::cerr << "[Entities] 无法写入输出文件: " << outputPath << std::endl;
        return 0;
    }
    out << result.dump(2);
    std::cout << "[Entities] Exported " << result["entities"].size()
              << " entities from " << chunks << " chunks -> " << outputPath << std::endl;
    return result["entities"].size();
}
}
