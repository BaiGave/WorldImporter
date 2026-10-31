#include "ModelDeduplicator.h"

#include <algorithm>
#include <bit>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>

// The application normally supplies this in main.cpp.
Config config;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void checkVertexQuantization() {
    auto check = [](float value) {
        const int expected = static_cast<int>(std::round(value * 10000.0f));
        const auto key = MakeVertexKey(value, -value, value);
        require(key.x == expected && key.y == -expected && key.z == expected,
                "vertex quantization differs from std::round");
    };
    // Every half-step boundary in the dense world's coordinate range, and
    // the adjacent floats on both sides (including negative coordinates).
    for (int bucket = -1000000; bucket <= 2100000; ++bucket) {
        const float value = (bucket + 0.5f) / 10000.0f;
        check(value);
        check(std::nextafter(value, -std::numeric_limits<float>::infinity()));
        check(std::nextafter(value, std::numeric_limits<float>::infinity()));
    }
    // Cover the wider defined int-key domain too, especially scaled floats
    // that are already integral, signed zero, and subnormal coordinates.
    for (float scaled : {0.0f, -0.0f, 0.5f, -0.5f, 8388607.5f, -8388607.5f,
                         8388608.0f, -8388608.0f, 2147483520.0f, -2147483520.0f})
        check(scaled / 10000.0f);
    check(std::numeric_limits<float>::denorm_min());
    check(-std::numeric_limits<float>::denorm_min());
    uint32_t bits = 0x12345678u;
    for (int i = 0; i < 1000000; ++i) {
        bits ^= bits << 13;
        bits ^= bits >> 17;
        bits ^= bits << 5;
        const float value = std::bit_cast<float>(bits);
        if (std::isfinite(value) && std::fabs(value * 10000.0f) <= 2147483520.0f)
            check(value);
    }
}

void checkFirstWins(int budget) {
    SetModelThreadBudget(budget);
    ModelData data;
    // Deliberately unsorted, with distinct original floats in the same bucket.
    // Negative and positive half steps must round away from zero, not truncate.
    data.vertices = {
        4.00001f, 0, -2,
        -0.00006f, -1, 3,
        4.00002f, 0, -2,
        -0.00005f, -1, 3,
        0.00005f, -1, 3,
        -0.00004f, -1, 3,
        0.00004f, -1, 3,
        0, 0.5f, 0,
        1, 0, 0, // Collided with the previous key under the old XOR hash.
    };
    const std::vector<float> expectedVertices = {
        4.00001f, 0, -2,
        -0.00006f, -1, 3,
        0.00005f, -1, 3,
        -0.00004f, -1, 3,
        0, 0.5f, 0,
        1, 0, 0,
    };
    Face first{};
    first.vertexIndices = {2, 3, 4, 6};
    first.uvIndices = {7, 2, 9, 1};
    first.materialIndex = 3;
    first.faceDirection = NORTH;
    first.tintIndex = 2;
    first.bothSided = true;
    first.thinEdge = true;
    Face second{};
    second.vertexIndices = {8, 7, 5, 0};
    second.uvIndices = {4, 5, 6, 7};
    second.materialIndex = 1;
    second.faceDirection = UP;
    data.faces = {first, second};
    data.uvCoordinates = {0, 0, 1, 1};
    data.overlayPairs.push_back({0, 1});

    ModelDeduplicator::DeduplicateVertices(data);
    require(data.vertices == expectedVertices, "first original coordinates/order not preserved");
    require(data.faces.size() == 2, "face count changed");
    require(data.faces[0].vertexIndices == std::array<int, 4>{0, 1, 2, 3}, "first face connectivity/winding changed");
    require(data.faces[1].vertexIndices == std::array<int, 4>{5, 4, 3, 0}, "second face connectivity/winding changed");
    for (size_t i = 0; i < data.faces.size(); ++i) {
        const auto& before = i == 0 ? first : second;
        const auto& after = data.faces[i];
        require(after.uvIndices == before.uvIndices && after.materialIndex == before.materialIndex &&
                after.faceDirection == before.faceDirection && after.tintIndex == before.tintIndex &&
                after.bothSided == before.bothSided && after.thinEdge == before.thinEdge,
                "face attributes changed");
    }
    require(data.uvCoordinates == std::vector<float>{0, 0, 1, 1}, "UV coordinates changed");
    require(data.overlayPairs.size() == 1 && data.overlayPairs[0].baseFace == 0 &&
            data.overlayPairs[0].overlayFace == 1, "overlay face references changed");

    const auto faces = data.faces;
    ModelDeduplicator::DeduplicateVertices(data);
    require(data.vertices == expectedVertices && data.faces[0].vertexIndices == faces[0].vertexIndices &&
            data.faces[1].vertexIndices == faces[1].vertexIndices, "deduplication is not idempotent");

    ModelData empty;
    ModelDeduplicator::DeduplicateVertices(empty);
    require(empty.vertices.empty() && empty.faces.empty(), "empty mesh changed");
    ModelData noFaces;
    noFaces.vertices = {2, 3, 4, 2, 3, 4};
    ModelDeduplicator::DeduplicateVertices(noFaces);
    require(noFaces.vertices == std::vector<float>{2, 3, 4}, "mesh without faces not deduplicated");
}

bool sameFace(const Face& a, const Face& b) {
    return a.vertexIndices == b.vertexIndices && a.uvIndices == b.uvIndices &&
           a.materialIndex == b.materialIndex && a.faceDirection == b.faceDirection &&
           a.tintIndex == b.tintIndex && a.bothSided == b.bothSided && a.thinEdge == b.thinEdge;
}

void checkVertexTableGrowth() {
    SetModelThreadBudget(2);
    ModelData data;
    // Mostly unique keys force the initially smaller table to grow. Include
    // negative coordinates and later, distinct floats in existing buckets.
    for (int i = 0; i < 24000; ++i) {
        const float x = (i % 97 - 48) * 0.25f;
        const float y = (i / 97 % 101 - 50) * 0.25f;
        const float z = (i / 9797 - 1) * 0.25f;
        data.vertices.insert(data.vertices.end(), {x, y, z});
        if (i % 17 == 0)
            data.vertices.insert(data.vertices.end(), {x + 0.00001f, y, z});
    }
    // An ordered-map oracle is independent of the table's hash/probe scheme.
    std::map<VertexKey, int> reference;
    std::vector<float> expected;
    std::vector<int> remap;
    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        const VertexKey key{
            static_cast<int>(std::round(data.vertices[i] * 10000.0f)),
            static_cast<int>(std::round(data.vertices[i + 1] * 10000.0f)),
            static_cast<int>(std::round(data.vertices[i + 2] * 10000.0f))};
        auto [it, inserted] = reference.try_emplace(key, static_cast<int>(expected.size() / 3));
        if (inserted)
            expected.insert(expected.end(), data.vertices.begin() + i, data.vertices.begin() + i + 3);
        remap.push_back(it->second);
    }
    for (size_t i = 0; i + 3 < remap.size(); i += 4) {
        Face face{};
        face.vertexIndices = {static_cast<int>(i + 3), static_cast<int>(i),
                              static_cast<int>(i + 2), static_cast<int>(i + 1)};
        data.faces.push_back(face);
    }
    auto expectedFaces = data.faces;
    for (auto& face : expectedFaces)
        for (int& index : face.vertexIndices) index = remap[index];
    ModelDeduplicator::DeduplicateVertices(data);
    require(data.vertices == expected, "vertex table growth lost keys/first coordinates/order");
    for (size_t i = 0; i < expectedFaces.size(); ++i)
        require(sameFace(data.faces[i], expectedFaces[i]), "vertex table growth changed connectivity");
}

void checkFaceDeduplication(bool strict) {
    config.strictDeduplication = strict;
    Face first{};
    first.vertexIndices = {9, 2, 7, 4};
    first.uvIndices = {3, 8, 1, 6};
    first.materialIndex = 2;
    first.faceDirection = NORTH;
    first.tintIndex = 3;
    first.bothSided = true;
    first.thinEdge = true;
    Face cyclic = first;
    cyclic.vertexIndices = {7, 4, 9, 2};
    cyclic.uvIndices = {1, 6, 3, 8};
    cyclic.tintIndex = 5; // The first copy supplies attributes.
    Face reversed = first;
    reversed.vertexIndices = {9, 4, 7, 2};
    reversed.uvIndices = {3, 6, 1, 8};
    Face overlay = first;
    overlay.uvIndices = {13, 18, 11, 16};
    Face material = first;
    material.materialIndex = 7;
    // Repeated vertex keys require comparing UVs and the complete cyclic
    // sequence, rather than simply choosing the lowest vertex as the start.
    Face repeated = first;
    repeated.vertexIndices = {1, 1, 1, 1};
    repeated.uvIndices = {2, 0, 2, 1};
    Face repeatedCyclic = repeated;
    repeatedCyclic.uvIndices = {2, 1, 2, 0};

    ModelData data;
    data.faces = {first, cyclic, reversed, overlay, material, repeated, repeatedCyclic, first};
    std::vector<Face> expected = {first, reversed, overlay};
    if (strict) expected.push_back(material);
    expected.push_back(repeated);
    for (int pass = 0; pass < 2; ++pass) {
        ModelDeduplicator::DeduplicateFaces(data);
        require(data.faces.size() == expected.size(), "face dedup count/winding/UV/material changed");
        for (size_t i = 0; i < expected.size(); ++i) {
            require(sameFace(data.faces[i], expected[i]), "first face attributes/order not preserved");
        }
    }
    ModelData empty;
    ModelDeduplicator::DeduplicateFaces(empty);
    require(empty.faces.empty(), "empty face list changed");
}

void checkFaceHashCollisions() {
    config.strictDeduplication = true;
    ModelData data;
    std::vector<Face> expected;
    // Force a cluster at the end of the power-of-two table, including wraparound.
    // UV-only differences must survive collisions; later cyclic copies must not.
    for (int uv = 0; uv < 100000 && expected.size() < 32; ++uv) {
        Face face{};
        face.vertexIndices = {0, 1, 2, 3};
        face.uvIndices = {uv, uv + 1, uv + 2, uv + 3};
        face.materialIndex = 2;
        if ((FaceKeyHasher{}(FaceKey{face.vertexIndices, face.uvIndices, 2}) & 255) != 255)
            continue;
        face.tintIndex = static_cast<int>(expected.size());
        expected.push_back(face);
        data.faces.push_back(face);
        std::rotate(face.vertexIndices.begin(), face.vertexIndices.begin() + 1, face.vertexIndices.end());
        std::rotate(face.uvIndices.begin(), face.uvIndices.begin() + 1, face.uvIndices.end());
        face.tintIndex = -1;
        data.faces.push_back(face);
    }
    require(expected.size() == 32, "could not generate colliding face keys");
    ModelDeduplicator::DeduplicateFaces(data);
    require(data.faces.size() == expected.size(), "face hash collisions lost distinct UV keys");
    for (size_t i = 0; i < expected.size(); ++i)
        require(sameFace(data.faces[i], expected[i]), "face hash collisions changed first face/order");
}

ModelData adjacentQuads() {
    ModelData data;
    // Unique, deliberately unsorted keys, including negative coordinates.
    data.vertices = {0, 1, 0, -2, 0, 0, -1, 1, 0, 0, 0, 0, -2, 1, 0, -1, 0, 0};
    data.uvCoordinates = {0, 0, 1, 0, 1, 1, 0, 1};
    data.materials.emplace_back();
    Face first{};
    first.vertexIndices = {1, 5, 2, 4};
    first.uvIndices = {0, 1, 2, 3};
    first.materialIndex = 0;
    first.faceDirection = SOUTH;
    first.tintIndex = 2;
    first.bothSided = true;
    first.thinEdge = true;
    Face second = first;
    second.vertexIndices = {5, 3, 0, 2};
    data.faces = {first, second};
    return data;
}

void checkGreedyVertexIndices() {
    SetModelThreadBudget(2);
    auto data = adjacentQuads();
    const auto originalVertices = data.vertices;
    ModelDeduplicator::GreedyMesh(data);
    require(data.vertices == originalVertices, "greedy changed/appended existing vertex keys");
    require(data.faces.size() == 1, "adjacent quads did not merge");
    require(data.faces[0].vertexIndices == std::array<int, 4>{4, 1, 3, 0},
            "greedy lookup did not preserve existing indices/winding");
    require(data.faces[0].tintIndex == 2 && data.faces[0].bothSided && data.faces[0].thinEdge,
            "merged face attributes changed");
}

void checkGreedyBucketOrder() {
    SetModelThreadBudget(2);
    for (bool wideFirst : {false, true}) {
        for (bool incompatibleFirst : {false, true}) {
            auto data = adjacentQuads();
            // Two neighbors share the same min edge but overlap each other.
            // Only the lower-index compatible neighbor may extend the first
            // face; the other must remain in publication order on later passes.
            data.vertices.insert(data.vertices.end(), {1, 0, 0, 1, 1, 0});
            data.uvCoordinates.insert(data.uvCoordinates.end(), {0, 0, 2, 0, 2, 1, 0, 1});
            const Face narrow = data.faces.back();
            Face wide = narrow;
            wide.vertexIndices = {5, 6, 7, 2};
            wide.uvIndices = {4, 5, 6, 7};
            data.faces.resize(1);
            if (incompatibleFirst) {
                Face mirrored = narrow;
                mirrored.uvIndices = {3, 2, 1, 0};
                data.faces.push_back(mirrored);
            }
            data.faces.push_back(wideFirst ? wide : narrow);
            data.faces.push_back(wideFirst ? narrow : wide);
            const auto originalVertices = data.vertices;
            const size_t originalUVs = data.uvCoordinates.size();
            ModelDeduplicator::GreedyMesh(data);
            require(data.vertices == originalVertices, "bucket merge appended/reordered vertices");
            require(data.faces.size() == (incompatibleFirst ? 3u : 2u),
                    "overlapping or incompatible bucket neighbors merged");
            const std::array<int, 4> merged = wideFirst
                ? std::array<int, 4>{4, 1, 6, 7} : std::array<int, 4>{4, 1, 3, 0};
            const std::array<int, 4> leftover = wideFirst
                ? std::array<int, 4>{2, 5, 3, 0} : std::array<int, 4>{2, 5, 6, 7};
            require(data.faces.front().vertexIndices == merged,
                    "merge did not pick the lowest-index compatible bucket neighbor");
            require(data.faces.back().vertexIndices == leftover,
                    "overlapping bucket neighbor lost publication order");
            const std::vector<float> mergedUVs = wideFirst
                ? std::vector<float>{0, 1, 0, 0, 3, 0, 3, 1}
                : std::vector<float>{0, 1, 0, 0, 2, 0, 2, 1};
            require(std::equal(mergedUVs.begin(), mergedUVs.end(), data.uvCoordinates.begin() + originalUVs),
                    "bucket merge changed anchor UV mapping");
        }
    }
}

void checkGreedyParallelEquivalence() {
    auto tile = adjacentQuads();
    ModelData data;
    data.uvCoordinates = tile.uvCoordinates;
    data.materials = tile.materials;
    data.materials.emplace_back("animated", "", -1, ANIMATED);
    // Exceed the parallel threshold, with distinct components and a mix of
    // eligible, invalid-UV, animated and rotated faces.
    for (int i = 0; i < 5001; ++i) {
        const int base = static_cast<int>(data.vertices.size() / 3);
        for (size_t v = 0; v < tile.vertices.size(); v += 3) {
            const float x = tile.vertices[v], y = tile.vertices[v + 1];
            data.vertices.push_back(i * 4.0f + x + (i % 4 == 3 ? 0.5f * y : 0));
            data.vertices.push_back(y);
            data.vertices.push_back(0);
        }
        for (auto face : tile.faces) {
            for (int& index : face.vertexIndices) index += base;
            if (i % 4 == 1) face.uvIndices[0] = -1;
            if (i % 4 == 2) face.materialIndex = 1;
            data.faces.push_back(face);
        }
    }
    auto serial = data;
    SetModelThreadBudget(2);
    ModelDeduplicator::GreedyMesh(serial);
    SetModelThreadBudget(1);
    ModelDeduplicator::GreedyMesh(data);
    require(data.faces.size() < 10002, "parallel fixture did not exercise merging");
    require(data.vertices == serial.vertices && data.uvCoordinates == serial.uvCoordinates &&
            data.faces.size() == serial.faces.size(), "parallel greedy geometry differs");
    for (size_t i = 0; i < data.faces.size(); ++i) {
        require(sameFace(data.faces[i], serial.faces[i]), "parallel greedy face order/attributes differ");
    }
}

void checkGreedyEdgeClique() {
    auto data = adjacentQuads();
    const auto last = data.faces.back();
    data.faces.pop_back();
    // Put the two eligible neighbors in distant face chunks. Ineligible faces
    // between them must not hide their shared edge: every occurrence connects
    // to every earlier one, rather than only the most recent incident face.
    Face invalid = data.faces.front();
    invalid.uvIndices[0] = -1;
    Face repeated = invalid;
    repeated.vertexIndices = {1, 5, 1, 5}; // The same undirected edge four times.
    for (int i = 0; i < 24; ++i) {
        data.faces.push_back(invalid);
        data.faces.push_back(repeated);
    }
    data.faces.push_back(last);
    const auto original = data;
    auto serial = data;
    SetModelThreadBudget(2);
    ModelDeduplicator::GreedyMesh(serial);
    SetModelThreadBudget(1);
    ModelDeduplicator::GreedyMesh(data);
    require(data.faces.size() == original.faces.size() - 1,
            "non-manifold edge lost eligible neighbors across chunks");
    require(data.faces[0].vertexIndices == std::array<int, 4>{4, 1, 3, 0},
            "cross-chunk neighbors did not merge with stable winding");
    require(data.vertices == serial.vertices && data.uvCoordinates == serial.uvCoordinates &&
            data.faces.size() == serial.faces.size(), "edge clique parallel geometry differs");
    for (size_t i = 0; i < data.faces.size(); ++i) {
        require(sameFace(data.faces[i], serial.faces[i]), "edge clique face order/attributes differ");
        if (i > 0) require(sameFace(data.faces[i], original.faces[i]),
                           "ineligible/repeated-edge face changed");
    }
}

void checkGreedyGroupOrder() {
    SetModelThreadBudget(2);
    auto data = adjacentQuads();
    const auto tileVertices = data.vertices;
    const auto tileFaces = data.faces;
    data.materials.emplace_back("animated", "", -1, ANIMATED);
    for (int component = 1; component < 3; ++component) {
        for (size_t v = 0; v < tileVertices.size(); v += 3) {
            data.vertices.push_back(tileVertices[v] + component * 4.0f);
            data.vertices.push_back(tileVertices[v + 1]);
            data.vertices.push_back(tileVertices[v + 2]);
        }
    }
    auto shifted = [&](Face face, int offset) {
        for (int& index : face.vertexIndices) index += offset;
        return face;
    };
    Face invalidUV = tileFaces[0];
    invalidUV.uvIndices[0] = -1;
    Face animated = shifted(tileFaces[0], 6);
    animated.materialIndex = 1;
    // Interleave two components with ineligible singleton groups, followed by
    // an eligible singleton. Later members must return to their first group.
    data.faces = {tileFaces[0], invalidUV, shifted(tileFaces[0], 6), animated,
                  shifted(tileFaces[1], 6), tileFaces[1], shifted(tileFaces[0], 12)};
    const auto original = data;
    ModelDeduplicator::GreedyMesh(data);
    Face mergedA = tileFaces[0];
    mergedA.faceDirection = UNKNOWN;
    mergedA.vertexIndices = {4, 1, 3, 0};
    mergedA.uvIndices = {4, 5, 6, 7};
    Face mergedB = shifted(mergedA, 6);
    mergedB.uvIndices = {8, 9, 10, 11};
    const std::vector<Face> expected = {mergedA, invalidUV, mergedB, animated, original.faces[6]};
    require(data.vertices == original.vertices && data.faces.size() == expected.size(),
            "interleaved grouping changed geometry/component count");
    for (size_t i = 0; i < expected.size(); ++i)
        require(sameFace(data.faces[i], expected[i]),
                "interleaved grouping changed first-seen group/member order");
    require(data.uvCoordinates == std::vector<float>{
                0, 0, 1, 0, 1, 1, 0, 1,
                0, 1, 0, 0, 2, 0, 2, 1,
                0, 1, 0, 0, 2, 0, 2, 1},
            "interleaved grouping changed UV publication order");

    // No eligible roots: every face must retain its original group position.
    data = original;
    for (auto& face : data.faces) face.uvIndices[0] = -1;
    const auto ineligible = data;
    ModelDeduplicator::GreedyMesh(data);
    require(data.vertices == ineligible.vertices && data.uvCoordinates == ineligible.uvCoordinates &&
            data.faces.size() == ineligible.faces.size(), "ineligible singleton geometry changed");
    for (size_t i = 0; i < data.faces.size(); ++i)
        require(sameFace(data.faces[i], ineligible.faces[i]), "ineligible singleton order changed");
}
}

int main() {
    try {
        checkVertexQuantization();
        checkFirstWins(2); // Multiple model workers: remap serially.
        checkFirstWins(1); // Single model worker: use available hardware threads.
        checkVertexTableGrowth();
        checkFaceDeduplication(true);
        checkFaceDeduplication(false);
        checkFaceHashCollisions();
        checkGreedyVertexIndices();
        checkGreedyBucketOrder();
        checkGreedyParallelEquivalence();
        checkGreedyEdgeClique();
        checkGreedyGroupOrder();
        std::cout << "Model deduplicator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
