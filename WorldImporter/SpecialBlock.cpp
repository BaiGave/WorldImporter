#include "SpecialBlock.h"
#include <vector>
#include <algorithm>
#include <sstream>
#include <span>
#include "texture.h"
#include "blockstate.h"
#include <unordered_map>
#include <cmath>

using namespace std;
float centerX = 0.5f;
float centerY = 0.5f;
float centerZ = 0.5f;

namespace {
    unordered_map<string, string> ParseBlockStates(const string& blockName) {
        unordered_map<string, string> states;
        size_t begin = blockName.find('[');
        size_t end = blockName.rfind(']');
        if (begin == string::npos || end == string::npos || begin >= end) return states;

        stringstream stream(blockName.substr(begin + 1, end - begin - 1));
        string pair;
        while (getline(stream, pair, ',')) {
            size_t separator = pair.find('=');
            if (separator == string::npos) separator = pair.find(':');
            if (separator != string::npos) {
                states[pair.substr(0, separator)] = pair.substr(separator + 1);
            }
        }
        return states;
    }

    string BaseBlockName(const string& blockName) {
        size_t end = blockName.find('[');
        return blockName.substr(0, end);
    }

    void DisableCulling(ModelData& model) {
        for (auto& face : model.faces) face.faceDirection = FaceType::DO_NOT_CULL;
    }

    ModelData LoadCreateModel(const string& path, int index) {
        ModelData model = ProcessModelJson("create", path, 0, 0, false, index);
        DisableCulling(model);
        return model;
    }

    string GetCreateModelAsset(const string& path) {
        shared_lock<shared_mutex> lock(GlobalCache::cacheMutex);
        auto index = GlobalCache::modelAssetIndex.find("modelassets:create:" + path);
        if (index == GlobalCache::modelAssetIndex.end()) return "";
        auto asset = GlobalCache::modelAssets.find(index->second);
        return asset == GlobalCache::modelAssets.end() ? "" : asset->second;
    }

    string ResolveTexture(const nlohmann::json& modelJson, string value) {
        unordered_set<string> visited;
        while (!value.empty() && value[0] == '#' && visited.insert(value).second) {
            string key = value.substr(1);
            if (!modelJson.contains("textures") || !modelJson["textures"].contains(key)) return "";
            value = modelJson["textures"][key].get<string>();
        }
        return value;
    }

    Material CreateMaterial(const string& texture) {
        string ns = "minecraft";
        string path = texture;
        size_t colon = texture.find(':');
        if (colon != string::npos) {
            ns = texture.substr(0, colon);
            path = texture.substr(colon + 1);
        }
        string root = "textures";
        string savedPath = "textures/" + ns + "/" + path + ".png";
        if (SaveTextureToFile(ns, path, root)) RegisterTexture(ns, path, savedPath);
        Material material(texture, savedPath, -1);
        material.type = DetectMaterialType(ns, path, material.aspectRatio);
        return material;
    }

    ModelData LoadCreateObjModel(const string& jsonPath, int index) {
        (void)index;
        nlohmann::json modelJson = GetModelJson("create", jsonPath);
        if (modelJson.is_null()) return {};
        modelJson = LoadParentModel("create", jsonPath, modelJson);
        if (!modelJson.contains("model")) return {};

        string objPath = modelJson["model"].get<string>();
        size_t colon = objPath.find(':');
        if (colon != string::npos) objPath = objPath.substr(colon + 1);
        if (objPath.starts_with("models/")) objPath = objPath.substr(7);
        string obj = GetCreateModelAsset(objPath);
        if (obj.empty()) return {};

        vector<array<float, 3>> positions;
        vector<array<float, 2>> uvs;
        unordered_map<string, string> mtlTextures;
        stringstream objStream(obj);
        string line;
        while (getline(objStream, line)) {
            if (!line.starts_with("mtllib ")) continue;
            string mtlName = line.substr(7);
            string directory = objPath.substr(0, objPath.find_last_of('/') + 1);
            stringstream mtlStream(GetCreateModelAsset(directory + mtlName));
            string materialName;
            while (getline(mtlStream, line)) {
                if (line.starts_with("newmtl ")) materialName = line.substr(7);
                else if (line.starts_with("map_Kd ") && !materialName.empty()) {
                    mtlTextures[materialName] = ResolveTexture(modelJson, line.substr(7));
                }
            }
        }

        ModelData result;
        unordered_map<string, int> materialIndices;
        string currentMaterial;
        objStream.clear();
        objStream.seekg(0);
        bool flipV = modelJson.value("flip_v", false);
        while (getline(objStream, line)) {
            stringstream values(line);
            string type;
            values >> type;
            if (type == "v") {
                array<float, 3> value{};
                values >> value[0] >> value[1] >> value[2];
                positions.push_back(value);
            }
            else if (type == "vt") {
                array<float, 2> value{};
                values >> value[0] >> value[1];
                if (flipV) value[1] = 1.0f - value[1];
                uvs.push_back(value);
            }
            else if (type == "usemtl") values >> currentMaterial;
            else if (type == "f") {
                vector<pair<int, int>> corners;
                string corner;
                while (values >> corner) {
                    size_t slash = corner.find('/');
                    int vertex = stoi(corner.substr(0, slash)) - 1;
                    size_t nextSlash = slash == string::npos ? string::npos : corner.find('/', slash + 1);
                    int uv = slash == string::npos ? -1 : stoi(corner.substr(slash + 1, nextSlash - slash - 1)) - 1;
                    corners.emplace_back(vertex, uv);
                }
                if (corners.size() < 3) continue;
                int materialIndex;
                auto materialIt = materialIndices.find(currentMaterial);
                if (materialIt == materialIndices.end()) {
                    string texture = mtlTextures.contains(currentMaterial) ? mtlTextures[currentMaterial] : "minecraft:block/missingno";
                    materialIndex = static_cast<int>(result.materials.size());
                    result.materials.push_back(CreateMaterial(texture));
                    materialIndices[currentMaterial] = materialIndex;
                }
                else materialIndex = materialIt->second;

                for (size_t triangle = 1; triangle + 1 < corners.size(); ++triangle) {
                    array<pair<int, int>, 4> quad = { corners[0], corners[triangle], corners[triangle + 1], corners[triangle + 1] };
                    Face face{};
                    face.materialIndex = materialIndex;
                    face.faceDirection = FaceType::DO_NOT_CULL;
                    for (size_t i = 0; i < quad.size(); ++i) {
                        const auto& position = positions[quad[i].first];
                        face.vertexIndices[i] = static_cast<int>(result.vertices.size() / 3);
                        result.vertices.insert(result.vertices.end(), position.begin(), position.end());
                        face.uvIndices[i] = static_cast<int>(result.uvCoordinates.size() / 2);
                        if (quad[i].second >= 0 && static_cast<size_t>(quad[i].second) < uvs.size()) {
                            result.uvCoordinates.push_back(uvs[quad[i].second][0]);
                            result.uvCoordinates.push_back(uvs[quad[i].second][1]);
                        }
                        else {
                            result.uvCoordinates.push_back(0.0f);
                            result.uvCoordinates.push_back(0.0f);
                        }
                    }
                    result.faces.push_back(face);
                }
            }
        }
        return result;
    }

    void Rotate(ModelData& model, float x, float y, float z) {
        if (model.vertices.empty()) return;
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), x, y, z);
        DisableCulling(model);
    }

    void Append(ModelData& target, ModelData part) {
        if (part.vertices.empty()) return;
        if (target.vertices.empty()) target = std::move(part);
        else MergeModelsDirectly(target, part);
    }

    string NbtString(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::STRING ? bytesToString(tag->payload) : "";
    }

    bool NbtBool(const NbtTagPtr& nbt, const string& name) {
        auto tag = getChildByName(nbt, name);
        return tag && tag->type == TagType::BYTE && bytesToByte(tag->payload) != 0;
    }

    void ReplaceTexture(ModelData& model, const string& oldPath, const string& newPath) {
        string saveRoot = "textures";
        if (!SaveTextureToFile("create", newPath, saveRoot)) return;
        string savedPath = "textures/create/" + newPath + ".png";
        RegisterTexture("create", newPath, savedPath);
        for (auto& material : model.materials) {
            if (material.name == "create:" + oldPath) {
                material.name = "create:" + newPath;
                material.texturePath = savedPath;
                material.type = DetectMaterialType("create", newPath, material.aspectRatio);
            }
        }
    }

    float FacingY(const string& facing) {
        if (facing == "west") return 90.0f;
        if (facing == "north") return 180.0f;
        if (facing == "east") return 270.0f;
        return 0.0f;
    }

    void RotateKineticAxis(ModelData& model, const string& axis) {
        if (axis == "x") Rotate(model, 90.0f, 90.0f, 0.0f);
        else if (axis == "z") Rotate(model, 90.0f, 180.0f, 0.0f);
    }

    void RotateFromSouth(ModelData& model, const string& direction) {
        if (direction == "north") Rotate(model, 0.0f, 180.0f, 0.0f);
        else if (direction == "east") Rotate(model, 0.0f, 90.0f, 0.0f);
        else if (direction == "west") Rotate(model, 0.0f, 270.0f, 0.0f);
        else if (direction == "up") Rotate(model, 270.0f, 0.0f, 0.0f);
        else if (direction == "down") Rotate(model, 90.0f, 0.0f, 0.0f);
    }

    ModelData LoadBlockstateModel(const string& blockName) {
        size_t colon = blockName.find(':');
        string localName = colon == string::npos ? blockName : blockName.substr(colon + 1);
        return GetRandomModelFromCache("create", localName);
    }

    ModelData GenerateCreateWaterWheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result;
        string axis = "y";
        if (id == "water_wheel") {
            result = LoadBlockstateModel(blockName);
            string facing = states.contains("facing") ? states.at("facing") : "up";
            axis = facing == "east" || facing == "west" ? "x" :
                facing == "north" || facing == "south" ? "z" : "y";
            ModelData wheel = LoadCreateObjModel("block/water_wheel/wheel", 120);
            RotateKineticAxis(wheel, axis);
            Append(result, std::move(wheel));
        }
        else {
            axis = states.contains("axis") ? states.at("axis") : "y";
            bool extension = states.contains("extension") && states.at("extension") == "true";
            result = LoadCreateObjModel(extension ? "block/large_water_wheel/block_extension" :
                "block/large_water_wheel/block", 121);
            RotateKineticAxis(result, axis);
        }
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : 0.0f;
        if (axis == "x") Rotate(result, phase, 0.0f, 0.0f);
        else if (axis == "z") Rotate(result, 0.0f, 0.0f, phase);
        else Rotate(result, 0.0f, phase, 0.0f);
        return result;
    }

    ModelData GenerateCreateShaftedBlock(const string& id, const string& blockName,
        const unordered_map<string, string>& states) {
        ModelData result = LoadBlockstateModel(blockName);
        string axis = states.contains("axis") ? states.at("axis") : "y";
        ModelData shaft = LoadCreateModel("block/shaft", 132);
        RotateKineticAxis(shaft, axis);
        Append(result, std::move(shaft));
        return result;
    }

    void RotateAroundAxis(ModelData& model, const string& axis, float angle) {
        if (axis == "x") Rotate(model, angle, 0.0f, 0.0f);
        else if (axis == "z") Rotate(model, 0.0f, 0.0f, angle);
        else Rotate(model, 0.0f, angle, 0.0f);
    }

    ModelData GenerateCreateKinetic(const string& id,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData model = LoadCreateModel("block/" + id, 100);
        string axis = "y";
        auto axisIt = states.find("axis");
        if (axisIt != states.end()) axis = axisIt->second;
        RotateKineticAxis(model, axis);

        // Create offsets adjacent gears so their teeth mesh in a static export.
        int parity = axis == "x" ? y + z : axis == "z" ? x + y : x + z;
        float phase = parity % 2 == 0 ? 22.5f : (id == "large_cogwheel" ? 11.25f : 0.0f);
        if (id != "shaft") RotateAroundAxis(model, axis, phase);
        return model;
    }

    ModelData GenerateCreateEncasedCogwheel(const string& id, const string& blockName,
        const unordered_map<string, string>& states, int x, int y, int z) {
        ModelData result = LoadBlockstateModel(blockName);
        bool isLarge = id.find("large_cogwheel") != string::npos;
        string cogwheelId = isLarge ? "large_cogwheel" : "cogwheel";
        ModelData cogwheel = GenerateCreateKinetic(cogwheelId, states, x, y, z);
        Append(result, std::move(cogwheel));
        return result;
    }

    ModelData GenerateCreateBelt(const unordered_map<string, string>& states, const NbtTagPtr& nbt) {
        const string facing = states.contains("facing") ? states.at("facing") : "south";
        const string slope = states.contains("slope") ? states.at("slope") : "horizontal";
        const string part = states.contains("part") ? states.at("part") : "middle";
        const bool casing = states.contains("casing") && states.at("casing") == "true";
        const bool diagonal = slope == "upward" || slope == "downward";
        const bool sideways = slope == "sideways";
        const bool vertical = slope == "vertical";
        const bool downward = slope == "downward";
        const bool alongX = facing == "east" || facing == "west";
        const bool alongZ = !alongX;

        string beltPart = part == "pulley" ? "middle" : part;
        string beltPath = diagonal ? "block/belt/diagonal_" + beltPart : "block/belt/" + beltPart;
        ModelData result;
        Append(result, LoadCreateModel(beltPath, 110));
        if (!diagonal) Append(result, LoadCreateModel(beltPath + "_bottom", 111));

        float rx = ((!diagonal && slope != "horizontal") ? 90.0f : 0.0f)
            + (downward ? 180.0f : 0.0f) + (sideways ? 90.0f : 0.0f)
            + (vertical && alongZ ? 180.0f : 0.0f);
        float ry = FacingY(facing)
            + (((diagonal != alongX) && !downward) ? 180.0f : 0.0f)
            + (sideways && alongZ ? 180.0f : 0.0f)
            + (vertical && alongX ? 90.0f : 0.0f);
        float rz = (sideways ? 90.0f : 0.0f) + (vertical && alongX ? 90.0f : 0.0f);
        Rotate(result, rx, ry, rz);

        string dye = NbtString(nbt, "Dye");
        transform(dye.begin(), dye.end(), dye.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        if (!dye.empty()) {
            string baseTexture = diagonal ? "block/belt_diagonal" : "block/belt";
            string dyedTexture = "block/belt/" + dye + (diagonal ? "_diagonal_scroll" : "_scroll");
            ReplaceTexture(result, baseTexture, dyedTexture);
        }

        if (part != "middle") {
            ModelData pulley = LoadCreateModel("block/belt_pulley", 112);
            if (sideways) Rotate(pulley, 180.0f, 0.0f, 0.0f);
            else if (alongX) Rotate(pulley, 90.0f, 90.0f, 0.0f);
            else Rotate(pulley, 90.0f, 0.0f, 0.0f);
            Append(result, std::move(pulley));
        }

        if (casing) {
            string casingPart = part;
            bool negativeFacing = facing == "north" || facing == "west";
            if (part != "pulley" && ((vertical && negativeFacing) || downward || (sideways && negativeFacing))) {
                if (casingPart == "start") casingPart = "end";
                else if (casingPart == "end") casingPart = "start";
            }
            string shape = diagonal ? "diagonal" : (vertical ? "sideways" : "horizontal");
            ModelData casingModel = LoadCreateModel("block/belt_casing/" + shape + "_" + casingPart, 113);
            float casingX = vertical ? 90.0f : (sideways && negativeFacing ? 180.0f : 0.0f);
            float casingY = FacingY(facing) + (slope == "upward" ? 180.0f : 0.0f)
                + (vertical ? 90.0f : 0.0f);
            Rotate(casingModel, casingX, casingY, 0.0f);
            string casingType = NbtString(nbt, "Casing");
            if (casingType == "ANDESITE" || casingType == "andesite") {
                ReplaceTexture(casingModel, "block/belt/brass_belt_casing", "block/belt/andesite_belt_casing");
            }
            Append(result, std::move(casingModel));

            if (NbtBool(nbt, "Covered")) {
                bool brass = casingType == "BRASS" || casingType == "brass";
                string cover = "block/belt_cover/" + string(brass ? "brass" : "andesite")
                    + "_belt_cover_" + (alongX ? "x" : "z");
                Append(result, LoadCreateModel(cover, 114));
            }
        }
        return result;
    }
}

bool SpecialBlock::TryGenerateCreateBlockModel(const string& blockName,
    int x, int y, int z, const NbtTagPtr& blockEntityNbt, ModelData& outModel) {
    string baseName = BaseBlockName(blockName);
    if (baseName == "create:shaft" || baseName == "create:cogwheel" ||
        baseName == "create:large_cogwheel") {
        outModel = GenerateCreateKinetic(baseName.substr(7), ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:belt") {
        outModel = GenerateCreateBelt(ParseBlockStates(blockName), blockEntityNbt);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:water_wheel" || baseName == "create:large_water_wheel") {
        outModel = GenerateCreateWaterWheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_shaft" ||
        baseName == "create:brass_encased_shaft" ||
        baseName == "create:metal_girder_encased_shaft") {
        outModel = GenerateCreateShaftedBlock(baseName.substr(7), blockName, ParseBlockStates(blockName));
        return !outModel.vertices.empty();
    }
    if (baseName == "create:andesite_encased_cogwheel" ||
        baseName == "create:brass_encased_cogwheel" ||
        baseName == "create:andesite_encased_large_cogwheel" ||
        baseName == "create:brass_encased_large_cogwheel") {
        outModel = GenerateCreateEncasedCogwheel(baseName.substr(7), blockName,
            ParseBlockStates(blockName), x, y, z);
        return !outModel.vertices.empty();
    }
    return false;
}

ModelData SpecialBlock::GenerateSpecialBlockModel(const string& blockName) {
    string texturePath;
    int waterLevel;
    bool waterFalling;

    if (IsLightBlock(blockName, texturePath)&&config.exportLightBlock) {
        return GenerateLightBlockModel(texturePath);
    }

    // 检查床方块 (block ID 以 _bed 结尾)
    {
        string testName = blockName;
        size_t colonPos = testName.find(':');
        if (colonPos != string::npos) {
            testName = testName.substr(colonPos + 1);
        }
        size_t bracketPos = testName.find('[');
        string blockID = testName.substr(0, bracketPos);
        if (blockID.size() > 4 &&
            blockID.compare(blockID.size() - 4, 4, "_bed") == 0) {
            return GenerateBedModel(blockName);
        }
    }

    // 其他类型方块的生成逻辑
    ModelData defaultModel;
    return defaultModel;
}


bool SpecialBlock::IsLightBlock(const string& blockName, string& outTexturePath) {
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    string ns = "minecraft"; // 默认命名空间
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }

    // 过滤非minecraft命名空间
    if (ns != "minecraft") return false;

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 检查是否为光源方块
    if (blockID != "light") return false;

    // 提取光照等级
    string level = "15"; // 默认亮度
    if (bracketPos != string::npos) {
        size_t equalsPos = processed.find('=', bracketPos);
        size_t endPos = processed.find(']', equalsPos);
        if (equalsPos != string::npos && endPos != string::npos) {
            level = processed.substr(equalsPos + 1, endPos - equalsPos - 1);
        }
    }

    // 格式化为两位数
    if (level.length() == 1) level = "0" + level;

    // 构建材质路径
    outTexturePath = ns + ":block/light_block_" + level;
    return true;
}

ModelData SpecialBlock::GenerateLightBlockModel(const string& texturePath) {
    ModelData cubeModel;
    float halfSize = config.lightBlockSize;

    cubeModel.vertices = {
        // 前面 (front)
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        // 后面 (back)
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        // 上面 (top)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        // 下面 (bottom)
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 左面 (left)
        centerX - halfSize, centerY + halfSize, centerZ - halfSize,
        centerX - halfSize, centerY + halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ + halfSize,
        centerX - halfSize, centerY - halfSize, centerZ - halfSize,
        // 右面 (right)
        centerX + halfSize, centerY + halfSize, centerZ + halfSize,
        centerX + halfSize, centerY + halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ - halfSize,
        centerX + halfSize, centerY - halfSize, centerZ + halfSize
    };

    // 设置 UV 坐标
    cubeModel.uvCoordinates = {
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f
    };

    // 创建材质
    Material material;
    material.name = texturePath;
    material.texturePath = "None";
    material.tintIndex = -1;  // 设置默认tint索引
    cubeModel.materials = { material };

    // 使用Face结构体创建六个面
    cubeModel.faces.resize(6);
    
    // 前面
    cubeModel.faces[0].vertexIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].uvIndices = { 0, 1, 2, 3 };
    cubeModel.faces[0].materialIndex = 0;
    cubeModel.faces[0].faceDirection = FaceType::DO_NOT_CULL;
    
    // 后面
    cubeModel.faces[1].vertexIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].uvIndices = { 4, 5, 6, 7 };
    cubeModel.faces[1].materialIndex = 0;
    cubeModel.faces[1].faceDirection = FaceType::DO_NOT_CULL;
    
    // 上面
    cubeModel.faces[2].vertexIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].uvIndices = { 8, 9, 10, 11 };
    cubeModel.faces[2].materialIndex = 0;
    cubeModel.faces[2].faceDirection = FaceType::DO_NOT_CULL;
    
    // 下面
    cubeModel.faces[3].vertexIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].uvIndices = { 12, 13, 14, 15 };
    cubeModel.faces[3].materialIndex = 0;
    cubeModel.faces[3].faceDirection = FaceType::DO_NOT_CULL;
    
    // 左面
    cubeModel.faces[4].vertexIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].uvIndices = { 16, 17, 18, 19 };
    cubeModel.faces[4].materialIndex = 0;
    cubeModel.faces[4].faceDirection = FaceType::DO_NOT_CULL;
    
    // 右面
    cubeModel.faces[5].vertexIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].uvIndices = { 20, 21, 22, 23 };
    cubeModel.faces[5].materialIndex = 0;
    cubeModel.faces[5].faceDirection = FaceType::DO_NOT_CULL;

    return cubeModel;
}

ModelData SpecialBlock::GenerateBedModel(const string& blockName) {
    // 解析方块名称: "minecraft:red_bed[facing=east,part=head]"
    string ns = "minecraft";
    string processed = blockName;

    // 提取命名空间
    size_t colonPos = processed.find(':');
    if (colonPos != string::npos) {
        ns = processed.substr(0, colonPos);
        processed = processed.substr(colonPos + 1);
    }
    if (ns != "minecraft") {
        return ModelData();
    }

    // 提取方块ID和状态
    size_t bracketPos = processed.find('[');
    string blockID = processed.substr(0, bracketPos);

    // 从方块ID中提取颜色 (移除 "_bed" 后缀)
    string color = blockID;
    size_t bedPos = color.rfind("_bed");
    if (bedPos != string::npos) {
        color = color.substr(0, bedPos);
    }

    // 解析状态属性
    string part = "head";
    string facing = "north";
    if (bracketPos != string::npos) {
        string statePart = processed.substr(bracketPos + 1, processed.size() - bracketPos - 2);
        stringstream ss(statePart);
        string pair;
        while (getline(ss, pair, ',')) {
            size_t eqPos = pair.find('=');
            if (eqPos != string::npos) {
                string key = pair.substr(0, eqPos);
                string value = pair.substr(eqPos + 1);
                if (key == "part") part = value;
                else if (key == "facing") facing = value;
            }
        }
    }

    // 构建材质路径
    string textureName = ns + ":entity/bed/" + color;
    string textureFileDir = "textures";
    string savedTexturePath;
    if (!SaveTextureToFile(ns, "entity/bed/" + color, textureFileDir)) {
        return ModelData(); // 纹理不存在,返回空模型
    }
    savedTexturePath = "textures/" + ns + "/entity/bed/" + color + ".png";
    RegisterTexture(ns, "entity/bed/" + color, savedTexturePath);

    ModelData model;

    // 辅助lambda: 添加一个面 (顶点使用床本地坐标系 -0.5~0.5, 自动转换到 0~1)
    auto addFace = [&](float v0x, float v0y, float v0z,
                        float v1x, float v1y, float v1z,
                        float v2x, float v2y, float v2z,
                        float v3x, float v3y, float v3z,
                        float u0, float v0,
                        float u1, float v1,
                        float u2, float v2,
                        float u3, float v3) {
        int vertexStart = (int)model.vertices.size() / 3;
        int uvStart = (int)model.uvCoordinates.size() / 2;

        // 从床本地坐标系 (-0.5~0.5) 转换到方块坐标系 (0~1)
        model.vertices.push_back(v0x + 0.5f);
        model.vertices.push_back(v0y + 0.5f);
        model.vertices.push_back(v0z + 0.5f);
        model.vertices.push_back(v1x + 0.5f);
        model.vertices.push_back(v1y + 0.5f);
        model.vertices.push_back(v1z + 0.5f);
        model.vertices.push_back(v2x + 0.5f);
        model.vertices.push_back(v2y + 0.5f);
        model.vertices.push_back(v2z + 0.5f);
        model.vertices.push_back(v3x + 0.5f);
        model.vertices.push_back(v3y + 0.5f);
        model.vertices.push_back(v3z + 0.5f);

        model.uvCoordinates.push_back(u0);
        model.uvCoordinates.push_back(v0);
        model.uvCoordinates.push_back(u1);
        model.uvCoordinates.push_back(v1);
        model.uvCoordinates.push_back(u2);
        model.uvCoordinates.push_back(v2);
        model.uvCoordinates.push_back(u3);
        model.uvCoordinates.push_back(v3);

        Face face;
        face.vertexIndices = {vertexStart, vertexStart + 1, vertexStart + 2, vertexStart + 3};
        face.uvIndices = {uvStart, uvStart + 1, uvStart + 2, uvStart + 3};
        face.materialIndex = 0;
        face.faceDirection = DO_NOT_CULL;
        model.faces.push_back(face);
    };

    // 设置材质
    Material material;
    material.name = textureName;
    material.texturePath = savedTexturePath;
    material.tintIndex = -1;
    model.materials = { material };

    if (part == "head")
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f, 22/64.0f, 58/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 42/64.0f, 28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 44/64.0f, 58/64.0f);
        // 前面 (床头板正面, z=-0.5), jmc2obj 的 UV 是非顺序分配 (uv[2],uv[3],uv[0],uv[1])
        addFace( 0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,  -0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f, -0.5f,
                6/64.0f, 64/64.0f, 22/64.0f, 64/64.0f, 22/64.0f, 58/64.0f, 6/64.0f, 58/64.0f);
        // 左面 (床头板左侧, x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 58/64.0f,  0/64.0f, 42/64.0f,  6/64.0f, 42/64.0f,  6/64.0f, 58/64.0f);
        // 右面 (床头板右侧, x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 42/64.0f, 28/64.0f, 58/64.0f, 22/64.0f, 58/64.0f, 22/64.0f, 42/64.0f);

        // --- 左床腿 (床头端, z=-0.5~-0.3125, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,    -0.5f, -0.3125f, -0.3125f,  -0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.3125f, -0.3125f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace(-0.5f, -0.3125f, -0.3125f,  -0.3125f, -0.3125f, -0.3125f,  -0.3125f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, -0.3125f,  -0.5f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.5f,  -0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床头端, z=-0.5~-0.3125, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.5f, -0.3125f, -0.3125f,   0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f, -0.5f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.3125f, -0.5f, -0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=-0.5)
        addFace( 0.5f, -0.3125f, -0.5f,     0.3125f, -0.3125f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.5f, -0.5f, -0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=-0.3125, 内侧)
        addFace( 0.5f, -0.3125f, -0.3125f,   0.3125f, -0.3125f, -0.3125f,   0.3125f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, -0.3125f,   0.5f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.5f,   0.3125f, -0.5f, -0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }
    else // foot
    {
        // --- 床板主体 ---
        // 顶面 (床垫表面)
        addFace(-0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f,  0.5f,   0.5f, 0.0625f, -0.5f,  -0.5f, 0.0625f, -0.5f,
                22/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f, 22/64.0f, 36/64.0f);
        // 底面 (床垫下)
        addFace(-0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f, -0.5f,
                44/64.0f, 20/64.0f, 28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 44/64.0f, 36/64.0f);
        // 左面 (x=-0.5)
        addFace(-0.5f, -0.3125f, -0.5f,  -0.5f, -0.3125f,  0.5f,  -0.5f, 0.0625f,  0.5f,  -0.5f, 0.0625f, -0.5f,
                 0/64.0f, 36/64.0f,  0/64.0f, 20/64.0f,  6/64.0f, 20/64.0f,  6/64.0f, 36/64.0f);
        // 右面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.5f, -0.3125f, -0.5f,   0.5f, 0.0625f, -0.5f,   0.5f, 0.0625f,  0.5f,
                28/64.0f, 20/64.0f, 28/64.0f, 36/64.0f, 22/64.0f, 36/64.0f, 22/64.0f, 20/64.0f);
        // 后面 (床尾板, z=0.5)
        addFace( 0.5f, -0.3125f, 0.5f,  -0.5f, -0.3125f, 0.5f,  -0.5f, 0.0625f, 0.5f,   0.5f, 0.0625f, 0.5f,
                38/64.0f, 42/64.0f, 22/64.0f, 42/64.0f, 22/64.0f, 36/64.0f, 38/64.0f, 36/64.0f);

        // --- 左床腿 (床尾端, z=0.3125~0.5, x=-0.5~-0.3125) ---
        // 左侧面 (x=-0.5)
        addFace(-0.5f, -0.3125f,  0.5f,    -0.5f, -0.3125f, 0.3125f,  -0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 右侧面 (x=-0.3125, 内侧)
        addFace(-0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace(-0.5f, -0.3125f,  0.5f,  -0.3125f, -0.3125f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace(-0.5f, -0.3125f, 0.3125f,  -0.3125f, -0.3125f, 0.3125f,  -0.3125f, -0.5f, 0.3125f,  -0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace(-0.5f, -0.5f, 0.3125f,  -0.5f, -0.5f,  0.5f,  -0.3125f, -0.5f,  0.5f,  -0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);

        // --- 右床腿 (床尾端, z=0.3125~0.5, x=0.3125~0.5) ---
        // 右侧面 (x=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,     0.5f, -0.3125f, 0.3125f,   0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 左侧面 (x=0.3125, 内侧)
        addFace( 0.3125f, -0.3125f,  0.5f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.3125f, -0.5f,  0.5f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 前面 (z=0.5)
        addFace( 0.5f, -0.3125f,  0.5f,   0.3125f, -0.3125f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.5f, -0.5f,  0.5f,
                53/64.0f, 61/64.0f, 56/64.0f, 61/64.0f, 56/64.0f, 58/64.0f, 53/64.0f, 58/64.0f);
        // 后面 (z=0.3125, 内侧)
        addFace( 0.5f, -0.3125f, 0.3125f,   0.3125f, -0.3125f, 0.3125f,   0.3125f, -0.5f, 0.3125f,   0.5f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 58/64.0f, 56/64.0f, 58/64.0f);
        // 底面 (y=-0.5)
        addFace( 0.5f, -0.5f, 0.3125f,   0.5f, -0.5f,  0.5f,   0.3125f, -0.5f,  0.5f,   0.3125f, -0.5f, 0.3125f,
                56/64.0f, 61/64.0f, 59/64.0f, 61/64.0f, 59/64.0f, 64/64.0f, 56/64.0f, 64/64.0f);
    }

    // 根据朝向应用旋转 (使用方块坐标系 0~1, 以 0.5,0.5 为中心)
    int rotY = 0;
    if (facing == "south") rotY = 180;
    else if (facing == "west") rotY = 270;
    else if (facing == "east") rotY = 90;

    if (rotY != 0) {
        ApplyRotationToVertices(span<float>(model.vertices.data(), model.vertices.size()), 0, rotY);
    }

    return model;
}
