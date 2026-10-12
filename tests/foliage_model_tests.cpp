#include "model.h"
#include "ModelDeduplicator.h"
#include <cmath>
#include <iostream>

// Exercise the real model parser without loading game archives.
void processElements(const nlohmann::json&, ModelData&,
    const std::unordered_map<std::string, int>&);
void ApplyRotationToUV(ModelData&, int, int);
Config config;
static int failures = 0;
static void check(bool ok, const char* label) {
    std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
    failures += !ok;
}
static bool near(float a, float b) { return std::fabs(a-b) < 0.0001f; }
static ModelData parse(const nlohmann::json& elements) {
    ModelData m;
    m.materials.emplace_back("test:leaves", "test.png", -1);
    m.materials.emplace_back("test:leaves_back", "back.png", -1);
    processElements({{"elements", elements}}, m, {{"front",0}, {"back",1}});
    return m;
}
int main() {
    using json = nlohmann::json;
    // A 22.5-degree rescaled frond should expand by 1/cos(22.5), not shrink.
    auto element = json{{"from",{0,0,8}}, {"to",{16,16,8}},
        {"rotation",{{"axis","y"},{"angle",22.5},{"origin",{8,8,8}},{"rescale",true}}},
        {"faces",{{"north",{{"texture","#front"},{"uv",{0,0,16,16}}}}}}};
    auto rotated = parse(json::array({element}));
    float xmin = 2, xmax = -2;
    for (size_t i=0; i<rotated.vertices.size(); i+=3) {
        xmin=std::min(xmin,rotated.vertices[i]); xmax=std::max(xmax,rotated.vertices[i]);
    }
    check(near(xmin,0) && near(xmax,1), "22.5-degree rescaled leaf spans full original width");

    // Same zero-thickness element can explicitly define different front/back art.
    json plane = {{"from",{0,0,8}}, {"to",{16,16,8}}, {"faces",{
        {"north",{{"texture","#front"},{"uv",{0,0,16,16}}}},
        {"south",{{"texture","#back"},{"uv",{16,0,0,16}}}}}}};
    auto twoSided=parse(json::array({plane}));
    check(twoSided.faces.size()==2, "zero-thickness leaf preserves explicit front and back faces");

    // uvlock must use geometric direction, not whether cullface was specified.
    json top = {{"from",{0,0,0}}, {"to",{16,16,16}},
        {"faces",{{"up",{{"texture","#front"},{"uv",{2,3,12,15}}}}}}};
    auto freeFace=parse(json::array({top}));
    top["faces"]["up"]["cullface"]="up";
    auto culledFace=parse(json::array({top}));
    for (auto* m : {&freeFace,&culledFace}) {
        ApplyRotationToVertices(std::span<float>(m->vertices),0,90);
        ApplyRotationToUV(*m,0,90);
    }
    bool same=true;
    for (int i=0;i<4;++i) for (int axis=0;axis<2;++axis)
        same &= near(freeFace.uvCoordinates[freeFace.faces[0].uvIndices[i]*2+axis],
                     culledFace.uvCoordinates[culledFace.faces[0].uvIndices[i]*2+axis]);
    check(same, "unculled foliage receives same uvlock correction as boundary face");
    bool allRotations = true;
    for (const std::string face : {"up","down","north","south","east","west"}) {
        for (int rx : {0,90,180,270}) for (int ry : {0,90,180,270}) {
            json e={{"from",{0,0,0}},{"to",{16,16,16}},
                {"faces",{{face,{{"texture","#front"},{"uv",{2,3,12,15}}}}}}};
            auto uncull=parse(json::array({e}));
            e["faces"][face]["cullface"]=face;
            auto cull=parse(json::array({e}));
            for (auto* m : {&uncull,&cull}) {
                ApplyRotationToVertices(std::span<float>(m->vertices),rx,ry);
                ApplyRotationToUV(*m,rx,ry);
            }
            for (int i=0;i<4;++i) for (int a=0;a<2;++a)
                allRotations &= near(uncull.uvCoordinates[uncull.faces[0].uvIndices[i]*2+a],
                                     cull.uvCoordinates[cull.faces[0].uvIndices[i]*2+a]);
        }
    }
    check(allRotations,"all 96 axis-face/blockstate rotations preserve unculled uvlock");

    // Extended UVs are valid for hanging foliage; UV locking must not clamp them.
    top["faces"]["up"]["uv"]={-8,0,24,16};
    auto extended=parse(json::array({top}));
    ApplyRotationToVertices(std::span<float>(extended.vertices),0,90);
    ApplyRotationToUV(extended,0,90);
    bool outside=false;
    for (float v:extended.uvCoordinates) outside |= v<0 || v>1;
    check(outside,"uvlock preserves out-of-range resource-pack UVs");

    ModelData animated;
    animated.materials.emplace_back("test:leaves", "test.png", -1, ANIMATED, 4.0f);
    // Use unit UV range before animation cropping.
    top["faces"]["up"]["uv"]={0,0,16,16};
    processElements({{"elements",json::array({top})}},animated,{{"front",0}});
    ApplyRotationToVertices(std::span<float>(animated.vertices),0,90);
    ApplyRotationToUV(animated,0,90);
    bool inFrame=true;
    for (int i:animated.faces[0].uvIndices) {
        float u=animated.uvCoordinates[i*2],v=animated.uvCoordinates[i*2+1];
        inFrame &= u>=-0.0001f && u<=1.0001f && v>=0.7499f && v<=1.0001f;
    }
    check(inFrame,"animated foliage uvlock stays in the first animation frame");

    // Opposite windings are not duplicates, even with identical material/UV pairs.
    ModelData mesh;
    mesh.vertices={0,0,0, 1,0,0, 1,1,0, 0,1,0};
    mesh.uvCoordinates={0,0,1,0,1,1,0,1};
    mesh.materials.emplace_back("test:leaf","test.png",-1);
    mesh.faces.push_back({{0,1,2,3},{0,1,2,3},0,DO_NOT_CULL});
    mesh.faces.push_back({{3,2,1,0},{3,2,1,0},0,DO_NOT_CULL});
    mesh.faces.push_back(mesh.faces[0]);
    ModelDeduplicator::DeduplicateFaces(mesh);
    check(mesh.faces.size()==2,"dedup removes same winding only, keeps leaf reverse side");
    return failures ? 1 : 0;
}
