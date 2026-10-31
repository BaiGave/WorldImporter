#include "model.h"

#include <iostream>
#include <stdexcept>
#include <string>

Config config;

namespace {
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ModelData makePair(bool sharedMaterial = true, bool sameUv = true) {
    ModelData model;
    model.vertices = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    model.uvCoordinates = {0, 0, 1, 0, 1, 1, 0, 1};
    model.materials.emplace_back("test:front", "textures/test/shared.png", -1);
    model.materials.emplace_back("test:back", "textures/test/shared.png", -1);
    Face front{};
    front.vertexIndices = {0, 1, 2, 3};
    front.uvIndices = {0, 1, 2, 3};
    front.materialIndex = 0;
    front.bothSided = true;
    Face back = front;
    back.vertexIndices = {3, 2, 1, 0};
    back.uvIndices = sameUv ? std::array<int, 4>{3, 2, 1, 0}
                           : std::array<int, 4>{0, 1, 2, 3};
    back.materialIndex = sharedMaterial ? 0 : 1;
    model.faces = {front, back};
    return model;
}

void expectFaces(ModelData model, size_t count, const std::string& message) {
    const auto frontName = model.materials[model.faces[0].materialIndex].name;
    const auto backName = model.materials[model.faces[1].materialIndex].name;
    SeparateCoincidentFaces(model);
    require(model.faces.size() == count, message);
    // The geometry optimization must not rename or rewrite the materials.
    require(model.materials[0].name == frontName, "front material name changed");
    require(model.materials[1].name == "test:back", "back material name changed");
    if (count == 2) {
        require(model.materials[model.faces[1].materialIndex].name == backName,
                "retained back material changed");
    }
}

void checkMergeModes() {
    config = Config{};
    expectFaces(makePair(), 2, "default mode must keep both faces");
    config.mergeSameDoubleSided = true;
    expectFaces(makePair(), 1, "same UV may merge");
    expectFaces(makePair(true, false), 2, "same mode must keep mirrored UV");
    expectFaces(makePair(false), 2, "same mode must keep different materials");
    config.mergeSameDoubleSided = false;
    config.mergeMirrorDoubleSided = true;
    expectFaces(makePair(true, true), 2, "mirror mode must keep same-direction pairs");
    expectFaces(makePair(true, false), 1, "mirror mode may merge mirrored UV");
    expectFaces(makePair(false), 1, "mirror mode may merge equivalent materials");
    config.doubleSidedGeometry = false;
    expectFaces(makePair(false), 2, "merge flags require double-sided geometry");
}

void checkTintProtection() {
    config = Config{};
    config.mergeMirrorDoubleSided = true;
    // CTM can lock different resolved tints onto the same texture while the
    // per-face tintIndex on both sides is -1.
    auto fixed = makePair(false);
    fixed.materials[0].tint = {TintKind::Fixed, 0xFF0000};
    fixed.materials[1].tint = {TintKind::Fixed, 0x0000FF};
    fixed.materials[0].tintLocked = fixed.materials[1].tintLocked = true;
    expectFaces(fixed, 2, "different fixed tints must keep both sides");
    fixed.materials[1].tint = fixed.materials[0].tint;
    expectFaces(fixed, 1, "equivalent fixed tints should still merge");

    auto biome = makePair(false);
    biome.materials[0].tint.kind = TintKind::Grass;
    biome.materials[1].tint.kind = TintKind::Foliage;
    expectFaces(biome, 2, "different biome tint kinds must keep both sides");
    biome.materials[1].tint.kind = TintKind::None;
    expectFaces(biome, 2, "tinted and untinted faces must remain distinct");
    biome.materials[1].tint = biome.materials[0].tint;
    expectFaces(biome, 1, "equivalent biome tint kinds should merge");

    auto faceTint = makePair(false);
    faceTint.faces[0].tintIndex = 0;
    expectFaces(faceTint, 2, "different face tint indices must keep both sides");

    auto texture = makePair(false);
    texture.materials[1].texturePath = "textures/test/other.png";
    expectFaces(texture, 2, "different textures must keep both sides");

    auto uv = makePair(false);
    uv.uvCoordinates.insert(uv.uvCoordinates.end(), {0.5f, 0.5f});
    uv.faces[1].uvIndices[0] = 4;
    expectFaces(uv, 2, "different UV regions must keep both sides");
}
} // namespace

int main() {
    try {
        checkMergeModes();
        checkTintProtection();
        std::cout << "double-sided merge tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
