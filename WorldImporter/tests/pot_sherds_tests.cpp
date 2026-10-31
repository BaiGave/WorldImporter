#include "SpecialBlock.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// The application normally supplies this in main.cpp.
Config config;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

NbtTagPtr MakeString(const std::string& name, const std::string& value) {
    auto tag = std::make_shared<NbtTag>(TagType::STRING, name);
    tag->payload.assign(value.begin(), value.end());
    return tag;
}

NbtTagPtr MakeCompound(const std::string& name, std::vector<NbtTagPtr> children) {
    auto tag = std::make_shared<NbtTag>(TagType::COMPOUND, name);
    tag->children = std::move(children);
    return tag;
}

NbtTagPtr MakeList(const std::string& name, std::vector<NbtTagPtr> children) {
    auto tag = std::make_shared<NbtTag>(TagType::LIST, name);
    tag->listType = TagType::STRING;
    tag->children = std::move(children);
    return tag;
}

// 26.1+ 物品: {id:"..."} , 可带无关子标签
NbtTagPtr MakeItem(const std::string& name, const std::string& id) {
    return MakeCompound(name, {MakeString("count", "1"), MakeString("id", id)});
}

void expectSherds(const NbtTagPtr& blockEntity, const std::vector<std::string>& expected, const char* message) {
    require(SpecialBlock::ReadPotSherds(blockEntity) == expected, message);
}

void checkListFormat() {
    // 1.20-1.21: sherds 是物品 id 字符串列表, 顺序 back, left, right, front
    auto sherds = MakeList("sherds", {
        MakeString("", "minecraft:angler_pottery_sherd"),
        MakeString("", "minecraft:archer_pottery_sherd"),
        MakeString("", "minecraft:arms_up_pottery_sherd"),
        MakeString("", "minecraft:blade_pottery_sherd"),
    });
    auto entity = MakeCompound("", {MakeString("id", "minecraft:decorated_pot"), sherds});
    expectSherds(entity, {
        "minecraft:angler_pottery_sherd",
        "minecraft:archer_pottery_sherd",
        "minecraft:arms_up_pottery_sherd",
        "minecraft:blade_pottery_sherd",
    }, "list sherds not read in back, left, right, front order");
}

void checkCompoundFormat() {
    // 键顺序与 back/left/right/front 不同; left 缺失应为空串
    auto sherds = MakeCompound("sherds", {
        MakeItem("front", "minecraft:brewer_pottery_sherd"),
        MakeItem("right", "minecraft:burn_pottery_sherd"),
        MakeItem("back", "minecraft:danger_pottery_sherd"),
    });
    expectSherds(MakeCompound("", {sherds}), {
        "minecraft:danger_pottery_sherd",
        "",
        "minecraft:burn_pottery_sherd",
        "minecraft:brewer_pottery_sherd",
    }, "compound sherds ignored key order or a missing side");
}

void checkBareStringCompound() {
    auto sherds = MakeCompound("sherds", {
        MakeString("left", "minecraft:explorer_pottery_sherd"),
        MakeString("front", "minecraft:friend_pottery_sherd"),
        MakeString("back", "minecraft:heart_pottery_sherd"),
        MakeString("right", "minecraft:heartbreak_pottery_sherd"),
    });
    expectSherds(MakeCompound("", {sherds}), {
        "minecraft:heart_pottery_sherd",
        "minecraft:explorer_pottery_sherd",
        "minecraft:heartbreak_pottery_sherd",
        "minecraft:friend_pottery_sherd",
    }, "bare-string compound sherds not read by side");
}

void checkMissingTag() {
    expectSherds(nullptr, {}, "null block entity should yield no sherds");
    auto entity = MakeCompound("", {MakeString("id", "minecraft:decorated_pot")});
    expectSherds(entity, {}, "missing sherds tag should yield an empty vector");
}
}

int main() {
    try {
        checkListFormat();
        checkCompoundFormat();
        checkBareStringCompound();
        checkMissingTag();
        std::cout << "Pot sherds tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
