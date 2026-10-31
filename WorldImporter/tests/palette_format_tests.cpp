// 26.3+ chunk palette formats: bare default-state ids, {"": id} wrappers,
// {id, properties}; plus legacy {Name, Properties}.
#include "nbtutils.h"
#include "config.h"
#include <iostream>
#include <stdexcept>

Config config;

namespace {
NbtTagPtr str(const std::string& name, const std::string& v) {
    auto t = std::make_shared<NbtTag>(TagType::STRING, name);
    t->payload.assign(v.begin(), v.end());
    return t;
}
NbtTagPtr compound(const std::string& name, std::vector<NbtTagPtr> kids) {
    auto t = std::make_shared<NbtTag>(TagType::COMPOUND, name);
    t->children = std::move(kids);
    return t;
}
void expect(const std::string& got, const std::string& want) {
    if (got != want) throw std::runtime_error("got '" + got + "' want '" + want + "'");
}
}

int main() {
    try {
        // bare string, default props filled
        expect(ParseBlockStateTag(str("", "minecraft:stone")), "minecraft:stone");
        expect(ParseBlockStateTag(str("", "minecraft:oak_log")), "minecraft:oak_log[axis:y]");
        // mixed-list wrapper {"": id}
        expect(ParseBlockStateTag(compound("", {str("", "minecraft:tall_grass")})), "minecraft:tall_grass[half:lower]");
        // {id, properties}, full and partial
        expect(ParseBlockStateTag(compound("", {str("id", "minecraft:oak_log"),
            compound("properties", {str("axis", "z")})})), "minecraft:oak_log[axis:z]");
        expect(ParseBlockStateTag(compound("", {str("id", "minecraft:water"),
            compound("properties", {str("level", "1")})})), "minecraft:water[level:1]");
        expect(ParseBlockStateTag(compound("", {str("id", "minecraft:grass_block"),
            compound("properties", {})})), "minecraft:grass_block[snowy:false]");
        // legacy full state untouched
        expect(ParseBlockStateTag(compound("", {str("Name", "minecraft:oak_log"),
            compound("Properties", {str("axis", "x")})})), "minecraft:oak_log[axis:x]");
        expect(ParseBlockStateTag(compound("", {str("Name", "minecraft:stone")})), "minecraft:stone");
        // legacy with omitted default props (Arnis): filled like the game does
        expect(ParseBlockStateTag(compound("", {str("Name", "minecraft:grass_block")})), "minecraft:grass_block[snowy:false]");
        expect(ParseBlockStateTag(compound("", {str("Name", "minecraft:grass_block"),
            compound("Properties", {str("snowy", "true")})})), "minecraft:grass_block[snowy:true]");
        expect(ParseBlockStateTag(compound("", {str("Name", "minecraft:snow")})), "minecraft:snow[layers:1]");
        // unknown modded id stays bare
        expect(ParseBlockStateTag(str("", "mymod:thing")), "mymod:thing");

        // palette keeps index alignment for List[String]
        auto list = std::make_shared<NbtTag>(TagType::LIST, "palette");
        list->children = {str("", "minecraft:air"), str("", "minecraft:oak_log")};
        auto bs = compound("block_states", {list});
        auto pal = getBlockPalette(bs);
        if (pal.size() != 2) throw std::runtime_error("palette size");
        expect(pal[1], "minecraft:oak_log[axis:y]");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
    std::cout << "palette_format_tests OK" << std::endl;
    return 0;
}
