#ifndef ENTITY_NBT_H
#define ENTITY_NBT_H

// 实体/物品 NBT 的小工具 (EntityModelExporter 与 ItemRender 共用)

#include "nbtutils.h"
#include <string>
#include <utility>
#include <vector>

namespace EntityNbt {
inline bool Num(const NbtTagPtr& tag, double& v) {
    if (!tag) return false;
    switch (tag->type) {
    case TagType::DOUBLE: v = bytesToDouble(tag->payload); return true;
    case TagType::FLOAT: v = bytesToFloat(tag->payload); return true;
    case TagType::INT: v = bytesToInt(tag->payload); return true;
    case TagType::SHORT: v = bytesToShort(tag->payload); return true;
    case TagType::BYTE: v = bytesToByte(tag->payload); return true;
    case TagType::LONG: v = static_cast<double>(bytesToLong(tag->payload)); return true;
    default: return false;
    }
}
inline double NumOr(const NbtTagPtr& parent, const char* name, double def) {
    double v; return Num(getChildByName(parent, name), v) ? v : def;
}
inline std::string Str(const NbtTagPtr& parent, const char* name) {
    auto t = getChildByName(parent, name);
    return (t && t->type == TagType::STRING) ? getStringTag(t) : std::string();
}
inline std::vector<double> NumList(const NbtTagPtr& tag) {
    std::vector<double> out;
    if (!tag || tag->type != TagType::LIST) return out;
    for (const auto& c : tag->children) { double v; if (Num(c, v)) out.push_back(v); }
    return out;
}
// "minecraft:plains" -> {"minecraft", "plains"}
inline std::pair<std::string, std::string> SplitId(const std::string& id) {
    size_t c = id.find(':');
    if (c == std::string::npos) return { "minecraft", id };
    return { id.substr(0, c), id.substr(c + 1) };
}
}  // namespace EntityNbt

#endif
