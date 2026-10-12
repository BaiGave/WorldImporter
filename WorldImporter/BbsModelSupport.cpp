#include "BbsModelSupport.h"
#include "config.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <cmath>

namespace fs = std::filesystem;
namespace {
struct Vec3 { float x = 0, y = 0, z = 0; };

const NbtTagPtr Child(const NbtTagPtr& tag, const std::string& name) {
    return tag ? getChildByName(tag, name) : nullptr;
}
std::string StringValue(const NbtTagPtr& tag) {
    return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
}
Vec3 JsonVec(const nlohmann::json& value, Vec3 fallback = {}) {
    if (!value.is_array() || value.size() < 3) return fallback;
    return { value[0].get<float>(), value[1].get<float>(), value[2].get<float>() };
}
std::vector<float> NbtNumberList(const NbtTagPtr& tag) {
    std::vector<float> result;
    if (!tag || tag->type != TagType::LIST) return result;
    for (const auto& child : tag->children) {
        if (child->type == TagType::FLOAT) result.push_back(bytesToFloat(child->payload));
        else if (child->type == TagType::DOUBLE) result.push_back(static_cast<float>(bytesToDouble(child->payload)));
    }
    return result;
}

void Rotate(Vec3& point, const Vec3& origin, const Vec3& degrees) {
    float x = point.x - origin.x, y = point.y - origin.y, z = point.z - origin.z;
    const float rx = degrees.x * static_cast<float>(M_PI / 180.0);
    const float ry = degrees.y * static_cast<float>(M_PI / 180.0);
    const float rz = degrees.z * static_cast<float>(M_PI / 180.0);
    if (degrees.x != 0) { float c=std::cos(rx),s=std::sin(rx),ny=y*c-z*s,nz=y*s+z*c; y=ny;z=nz; }
    if (degrees.y != 0) { float c=std::cos(ry),s=std::sin(ry),nx=x*c+z*s,nz=-x*s+z*c; x=nx;z=nz; }
    if (degrees.z != 0) { float c=std::cos(rz),s=std::sin(rz),nx=x*c-y*s,ny=x*s+y*c; x=nx;y=ny; }
    point = {x + origin.x, y + origin.y, z + origin.z};
}

fs::path FindModelRoot() {
    fs::path mods = fs::u8path(config.modsPath);
    fs::path candidate = mods.parent_path() / "config" / "bbs" / "assets" / "models";
    return fs::exists(candidate) ? candidate : fs::path();
}
fs::path FindModelFile(const fs::path& directory) {
    if (!fs::is_directory(directory)) return {};
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        std::string name = entry.path().filename().string();
        if (name.size() >= 9 && name.ends_with(".bbs.json")) return entry.path();
    }
    return {};
}
fs::path FindTextureFile(const fs::path& directory) {
    fs::path fallback;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".png") continue;
        std::string stem = entry.path().stem().string();
        if (stem.ends_with("_s") || stem.ends_with("_n")) continue;
        if (stem == "texture" || stem == "model" || stem == "default") return entry.path();
        if (fallback.empty()) fallback = entry.path();
    }
    return fallback;
}
std::string CopyTexture(const fs::path& source, const std::string& modelName) {
    if (source.empty()) return "None";
    size_t hash = std::hash<std::string>{}(modelName);
    fs::path relative = fs::path("textures") / "bbs_custom" / (std::to_string(hash) + ".png");
    fs::create_directories(relative.parent_path());
    std::error_code ec;
    fs::copy_file(source, relative, fs::copy_options::overwrite_existing, ec);
    return relative.generic_string();
}

void AddCube(ModelData& model, const nlohmann::json& cube, float texW, float texH,
             const std::vector<std::pair<Vec3, Vec3>>& rotations) {
    if (!cube.contains("from") || !cube.contains("size")) return;
    Vec3 from = JsonVec(cube["from"]), size = JsonVec(cube["size"]);
    Vec3 to{from.x + size.x, from.y + size.y, from.z + size.z};
    // BBS model units are 1/16 block and are centered on X/Z.
    auto cv = [](Vec3 v) { return Vec3{v.x/16.0f + 0.5f, v.y/16.0f, v.z/16.0f + 0.5f}; };
    Vec3 a=cv(from), b=cv(to);
    std::array<Vec3,8> p = {{{a.x,a.y,a.z},{b.x,a.y,a.z},{b.x,b.y,a.z},{a.x,b.y,a.z},
                              {a.x,a.y,b.z},{b.x,a.y,b.z},{b.x,b.y,b.z},{a.x,b.y,b.z}}};
    std::vector<std::pair<Vec3,Vec3>> all = rotations;
    if (cube.contains("rotate")) all.insert(all.begin(), {cv(JsonVec(cube.value("origin", nlohmann::json::array({0,0,0})))), JsonVec(cube["rotate"])});
    for (auto& v : p) for (const auto& transform : all) Rotate(v, transform.first, transform.second);

    struct Def { const char* name; std::array<int,4> vi; };
    const std::array<Def,6> defs = {{{"front",{0,1,2,3}}, {"back",{5,4,7,6}},
        {"right",{1,5,6,2}}, {"left",{4,0,3,7}}, {"bottom",{4,5,1,0}}, {"top",{3,2,6,7}}}};
    const auto uvs = cube.value("uvs", nlohmann::json::object());
    for (const auto& def : defs) {
        if (!uvs.contains(def.name) || !uvs[def.name].is_array() || uvs[def.name].size() < 4) continue;
        const auto& uv=uvs[def.name]; float u1=uv[0].get<float>()/texW,v1=1.0f-uv[1].get<float>()/texH;
        float u2=uv[2].get<float>()/texW,v2=1.0f-uv[3].get<float>()/texH;
        Face face{}; face.materialIndex=0; face.faceDirection=DO_NOT_CULL;
        const std::array<std::pair<float,float>,4> tc={{{u1,v2},{u2,v2},{u2,v1},{u1,v1}}};
        for(int i=0;i<4;++i){ const Vec3& v=p[def.vi[i]]; face.vertexIndices[i]=static_cast<int>(model.vertices.size()/3);
            model.vertices.insert(model.vertices.end(),{v.x,v.y,v.z}); face.uvIndices[i]=static_cast<int>(model.uvCoordinates.size()/2);
            model.uvCoordinates.insert(model.uvCoordinates.end(),{tc[i].first,tc[i].second}); }
        model.faces.push_back(face);
    }
}
}

bool TryGenerateBbsModel(const NbtTagPtr& nbt, int x, int y, int z, ModelData& outModel) {
    (void)x; (void)y; (void)z;
    if (StringValue(Child(nbt,"id")) != "bbs:model_block_entity") return false;
    auto properties=Child(nbt,"Properties"), form=Child(properties,"form");
    std::string modelName=StringValue(Child(form,"model"));
    fs::path root=FindModelRoot(), directory=root/fs::u8path(modelName), modelFile=FindModelFile(directory);
    if (modelName.empty() || modelFile.empty()) { std::cerr << "BBS model not found: " << modelName << std::endl; return false; }
    nlohmann::json json; try { std::ifstream input(modelFile); input >> json; } catch(const std::exception& e) {
        std::cerr << "BBS model JSON error: " << modelFile.string() << " - " << e.what() << std::endl; return false; }
    if (!json.contains("model")) return false;
    const auto& body=json["model"]; Vec3 textureSize=JsonVec(body.value("texture",nlohmann::json::array({64,64,0})),{64,64,0});
    Material material("bbs_custom:"+modelName,CopyTexture(FindTextureFile(directory),modelName),-1); outModel.materials.push_back(material);
    auto groups=body.value("groups",nlohmann::json::object());

    // BBS stores the selected bone pose in form/pose/pose. Bone rotations are radians.
    // Apply them in addition to the static rotations from the .bbs.json model.
    std::unordered_map<std::string, Vec3> poseRotations;
    auto poseRoot = Child(Child(form, "pose"), "pose");
    if (poseRoot && poseRoot->type == TagType::COMPOUND) {
        for (const auto& bone : poseRoot->children) {
            auto values = NbtNumberList(Child(bone, "r"));
            if (values.size() >= 3) {
                constexpr float radiansToDegrees = 180.0f / static_cast<float>(M_PI);
                poseRotations[bone->name] = { values[0] * radiansToDegrees,
                    values[1] * radiansToDegrees, values[2] * radiansToDegrees };
            }
        }
    }

    for (auto& item : groups.items()) {
        const auto& group=item.value(); std::vector<std::pair<Vec3,Vec3>> rotations; std::string current=item.key();
        std::unordered_map<std::string,bool> visited;
        while(groups.contains(current) && !visited[current]) { visited[current]=true; const auto& g=groups[current];
            Vec3 origin = JsonVec(g.value("origin",nlohmann::json::array({0,0,0})));
            if(g.contains("rotate")) rotations.push_back({origin,JsonVec(g["rotate"])});
            auto pose = poseRotations.find(current);
            if (pose != poseRotations.end()) rotations.push_back({origin, pose->second});
            current=g.value("parent",""); }
        for(auto& r:rotations) r.first={r.first.x/16.0f+0.5f,r.first.y/16.0f,r.first.z/16.0f+0.5f};
        if(group.contains("cubes")) for(const auto& cube:group["cubes"]) AddCube(outModel,cube,textureSize.x,textureSize.y,rotations);
    }
    if (body.contains("cubes")) for(const auto& cube:body["cubes"]) AddCube(outModel,cube,textureSize.x,textureSize.y,{});

    // Block entity transform uses block units; rotations are radians in BBS NBT.
    auto transform=Child(properties,"transform"); auto t=NbtNumberList(Child(transform,"t")); auto r=NbtNumberList(Child(transform,"r")); auto s=NbtNumberList(Child(transform,"s"));
    if(s.size()>=3) ApplyScaleToVertices(std::span<float>(outModel.vertices.data(),outModel.vertices.size()),s[0],s[1],s[2]);
    if(r.size()>=3) ApplyRotationToVertices(std::span<float>(outModel.vertices.data(),outModel.vertices.size()),r[0]*180.0f/static_cast<float>(M_PI),r[1]*180.0f/static_cast<float>(M_PI),r[2]*180.0f/static_cast<float>(M_PI));
    if(t.size()>=3) ApplyDoublePositionOffset(outModel,t[0],t[1],t[2]);
    return !outModel.vertices.empty();
}
