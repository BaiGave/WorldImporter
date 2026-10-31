#include "EntityBlock.h"
#include "RegionModelExporter.h"  // 包含必要的头文件,确保相关函数可用
#include "blockstate.h"
#include "blocktint.h"         // 为了调用 ProcessBlockstate
#include "showblockrandom.h"
#include <span>                  // 为了 std::span (C++20)
#include <iostream>            // 为了 std::cout, std::cerr (如果尚未包含)
#include <map>
#include <vector>
#include <string>
#include <cstdint>
#include <optional>
#include <shared_mutex>

// blockstate.cpp 中的非静态定义。放进匿名命名空间会变成内部链接。
extern std::shared_mutex blockstateCachesMutex;

namespace {

// 面序与游戏 getQuads 一致：DOWN, UP, NORTH, SOUTH, WEST, EAST, 无 cullface。
const FaceType kShowSides[6] = {DOWN, UP, NORTH, SOUTH, WEST, EAST};

bool IsShowSide(FaceType dir, int side) {
    if (side >= 0 && side < 6) return dir == kShowSides[side];
    return dir == DO_NOT_CULL || dir == UNKNOWN;
}

// 只保留该面的面片，顶点/UV 压成被引用的子集，材质表原样保留。
ModelData ExtractSideFaces(const ModelData& src, int side) {
    ModelData out;
    out.materials = src.materials;

    std::vector<int> faceMap(src.faces.size(), -1);
    out.faces.reserve(src.faces.size());
    for (size_t i = 0; i < src.faces.size(); ++i) {
        if (!IsShowSide(src.faces[i].faceDirection, side)) continue;
        faceMap[i] = static_cast<int>(out.faces.size());
        out.faces.push_back(src.faces[i]);
    }

    const int vertCount = static_cast<int>(src.vertices.size() / 3);
    const int uvCount = static_cast<int>(src.uvCoordinates.size() / 2);
    std::vector<char> vertUsed(vertCount, 0);
    std::vector<char> uvUsed(uvCount, 0);
    for (const Face& face : out.faces) {
        for (int k = 0; k < 4; ++k) {
            const int vi = face.vertexIndices[k];
            if (vi >= 0 && vi < vertCount) vertUsed[vi] = 1;
            const int ui = face.uvIndices[k];
            if (ui >= 0 && ui < uvCount) uvUsed[ui] = 1;
        }
    }

    std::vector<int> vertMap(vertCount, -1);
    out.vertices.reserve(static_cast<size_t>(vertCount) * 3);
    for (int i = 0; i < vertCount; ++i) {
        if (!vertUsed[i]) continue;
        vertMap[i] = static_cast<int>(out.vertices.size() / 3);
        out.vertices.push_back(src.vertices[static_cast<size_t>(i) * 3]);
        out.vertices.push_back(src.vertices[static_cast<size_t>(i) * 3 + 1]);
        out.vertices.push_back(src.vertices[static_cast<size_t>(i) * 3 + 2]);
    }
    std::vector<int> uvMap(uvCount, -1);
    out.uvCoordinates.reserve(static_cast<size_t>(uvCount) * 2);
    for (int i = 0; i < uvCount; ++i) {
        if (!uvUsed[i]) continue;
        uvMap[i] = static_cast<int>(out.uvCoordinates.size() / 2);
        out.uvCoordinates.push_back(src.uvCoordinates[static_cast<size_t>(i) * 2]);
        out.uvCoordinates.push_back(src.uvCoordinates[static_cast<size_t>(i) * 2 + 1]);
    }

    for (Face& face : out.faces) {
        for (int k = 0; k < 4; ++k) {
            const int vi = face.vertexIndices[k];
            face.vertexIndices[k] = (vi >= 0 && vi < vertCount && vertMap[vi] >= 0) ? vertMap[vi] : 0;
            const int ui = face.uvIndices[k];
            face.uvIndices[k] = (ui >= 0 && ui < uvCount && uvMap[ui] >= 0) ? uvMap[ui] : 0;
        }
    }

    for (const OverlayPair& pair : src.overlayPairs) {
        if (pair.baseFace < 0 || pair.overlayFace < 0) continue;
        if (pair.baseFace >= static_cast<int>(faceMap.size()) ||
            pair.overlayFace >= static_cast<int>(faceMap.size())) continue;
        const int base = faceMap[pair.baseFace];
        const int overlay = faceMap[pair.overlayFace];
        if (base < 0 || overlay < 0) continue;
        out.overlayPairs.push_back(OverlayPair{base, overlay});
    }
    return out;
}

bool ShowSlotsUniform(const std::array<std::vector<int>, 7>& picks) {
    for (int i = 1; i < 7; ++i) {
        if (picks[i] != picks[0]) return false;
    }
    return true;
}

void AppendShowPiece(ModelData& merged, bool& any, ModelData piece) {
    if (piece.faces.empty()) return;
    if (!any) {
        merged = std::move(piece);
        any = true;
    } else {
        merged = MergeModelData(merged, piece);
    }
}

const ModelData& VariantModelAt(const std::vector<WeightedModelData>& models, int slot) {
    if (slot < 0 || slot >= static_cast<int>(models.size())) return models.front().model;
    return models[static_cast<size_t>(slot)].model;
}

ModelData AssembleVariant(const std::vector<WeightedModelData>& models,
                          const std::array<std::vector<int>, 7>& picks) {
    if (ShowSlotsUniform(picks)) {
        const int slot = picks[0].empty() ? 0 : picks[0][0];
        return VariantModelAt(models, slot);
    }
    ModelData merged;
    bool any = false;
    for (int side = 0; side < 7; ++side) {
        const int slot = picks[side].empty() ? 0 : picks[side][0];
        AppendShowPiece(merged, any, ExtractSideFaces(VariantModelAt(models, slot), side));
    }
    return merged;
}

const ModelData* MultipartModelAt(const std::vector<MultipartGroup>& groups,
                                  const std::array<std::vector<int>, 7>& picks,
                                  int side, size_t groupIndex) {
    if (groupIndex >= groups.size() || groups[groupIndex].models.empty()) return nullptr;
    int slot = 0;
    if (side >= 0 && side < 7 && groupIndex < picks[side].size()) slot = picks[side][groupIndex];
    const auto& models = groups[groupIndex].models;
    if (slot < 0 || slot >= static_cast<int>(models.size())) slot = 0;
    return &models[static_cast<size_t>(slot)].model;
}

ModelData AssembleMultipart(const std::vector<MultipartGroup>& groups,
                            const std::array<std::vector<int>, 7>& picks) {
    if (ShowSlotsUniform(picks)) {
        ModelData merged;
        for (size_t g = 0; g < groups.size(); ++g) {
            const ModelData* model = MultipartModelAt(groups, picks, 0, g);
            if (!model) continue;
            merged = MergeModelData(merged, *model);
        }
        return merged;
    }
    ModelData merged;
    bool any = false;
    for (int side = 0; side < 7; ++side) {
        for (size_t g = 0; g < groups.size(); ++g) {
            const ModelData* model = MultipartModelAt(groups, picks, side, g);
            if (!model) continue;
            AppendShowPiece(merged, any, ExtractSideFaces(*model, side));
        }
    }
    return merged;
}

// rng 为空时走原来的 GetRandomModelFromCache（First / Random 不变）。
ModelData PickShowContentModel(const std::string& ns, const std::string& blockName,
                               std::optional<LegacyRandom>& rng) {
    if (!rng) return GetRandomModelFromCache(ns, blockName);

    std::shared_lock<std::shared_mutex> lock(blockstateCachesMutex, std::defer_lock);
    if (!blockstateCachesFrozen.load(std::memory_order_acquire)) lock.lock();

    auto blockNsIt = BlockModelCache.find(ns);
    if (blockNsIt != BlockModelCache.end()) {
        auto blockIt = blockNsIt->second.find(blockName);
        if (blockIt != blockNsIt->second.end()) return blockIt->second;
    }

    auto variantNsIt = VariantModelCache.find(ns);
    if (variantNsIt != VariantModelCache.end()) {
        auto variantIt = variantNsIt->second.find(blockName);
        if (variantIt != variantNsIt->second.end() && !variantIt->second.empty()) {
            ShowRandEntry entry;
            entry.kind = ShowRandEntry::Variant;
            entry.weights.reserve(variantIt->second.size());
            for (const auto& wm : variantIt->second) entry.weights.push_back(wm.weight);
            const auto picks = ShowRandPickEntry(entry, *rng);
            return AssembleVariant(variantIt->second, picks);
        }
    }

    auto multipartNsIt = MultipartModelCache.find(ns);
    if (multipartNsIt != MultipartModelCache.end()) {
        auto multipartIt = multipartNsIt->second.find(blockName);
        if (multipartIt != multipartNsIt->second.end()) {
            ShowRandEntry entry;
            entry.kind = ShowRandEntry::Multipart;
            entry.groups.reserve(multipartIt->second.size());
            for (const auto& group : multipartIt->second) {
                if (!group.weighted) {
                    entry.groups.emplace_back();
                    continue;
                }
                std::vector<int> weights;
                weights.reserve(group.models.size());
                for (const auto& wm : group.models) weights.push_back(wm.weight);
                entry.groups.push_back(std::move(weights));
            }
            const auto picks = ShowRandPickEntry(entry, *rng);
            return AssembleMultipart(multipartIt->second, picks);
        }
    }
    return ModelData();
}

} // namespace

void EntityBlock::PrintDetails() const {
    std::cout << "EntityBlock - ID: " << id << ", X: " << x << ", Y: " << y << ", Z: " << z << std::endl;
}

void YuushyaShowBlockEntity::PrintDetails() const {
    std::cout << "YuushyaShowBlockEntity - ID: " << id << ", X: " << x << ", Y: " << y << ", Z: " << z << std::endl;
    std::cout << "ControlSlot: " << controlSlot << ", KeepPacked: " << keepPacked << std::endl;
    for (const auto& block : blocks) {
        std::cout << "  BlockState Name: " << ", BlockID: " << block.blockid << std::endl;
        std::cout << "  ShowPos: ";
        for (const auto& pos : block.showPos) std::cout << pos << " ";
        std::cout << "\n  ShowRotation: ";
        for (const auto& rot : block.showRotation) std::cout << rot << " ";
        std::cout << "\n  ShowScales: ";
        for (const auto& scale : block.showScales) std::cout << scale << " ";
        std::cout << "\n  IsShown: " << block.isShown << ", Slot: " << block.slot << std::endl;
    }
}

// 方块名形如 "yuushya:showblock[facing=east,lit=0]"(属性分隔符也可能是 ':'),
// 返回 Direction.toYRot(): south=0, west=90, north=180, east=270; 无 facing 时为 0。
float ShowBlockFacingYRot(const std::string& blockName) {
    const size_t bracket = blockName.find('[');
    if (bracket == std::string::npos) return 0.0f;
    size_t pos = bracket + 1;
    while (pos < blockName.size()) {
        size_t end = blockName.find_first_of(",]", pos);
        if (end == std::string::npos) end = blockName.size();
        const std::string kv = blockName.substr(pos, end - pos);
        const size_t sep = kv.find_first_of("=:");
        if (sep != std::string::npos && kv.compare(0, sep, "facing") == 0) {
            const std::string v = kv.substr(sep + 1);
            if (v == "west") return 90.0f;
            if (v == "north") return 180.0f;
            if (v == "east") return 270.0f;
            return 0.0f;
        }
        pos = end + 1;
    }
    return 0.0f;
}

ModelData YuushyaShowBlockEntity::GenerateModel() const {
    ModelData mainModel;
    std::optional<LegacyRandom> showRng;
    if (config.variantSeedMode == VariantSeedMode::Game) showRng.emplace(BlockPosSeed(x, y, z));
    for (const auto& block : blocks) {
        int id = block.blockid;
        // 位置偏移(除以16转换到模型空间)
        double tx = block.showPos[0] / 16.0f;
        double ty = block.showPos[1] / 16.0f;
        double tz = block.showPos[2] / 16.0f;

        // 旋转参数(直接使用原始值)
        float rx = block.showRotation[0]; // 假设已是角度值
        float ry = block.showRotation[1];
        float rz = block.showRotation[2];

        // 缩放参数
        float sx = block.showScales[0];
        float sy = block.showScales[1];
        float sz = block.showScales[2];

        if (!block.isShown) continue;

        Block b = GetBlockById(id);
        std::string blockName = b.GetModifiedNameWithNamespace();
        std::string ns = b.GetNamespace();
        // 获取模型数据
        size_t colonPos = blockName.find(':');
        if (colonPos != std::string::npos) blockName = blockName.substr(colonPos + 1);
        ModelData blockModel = PickShowContentModel(ns, blockName, showRng);

        // 批次加载阶段已串行预解析全局调色板。模型阶段缓存冻结后禁止
        // 惰性 ProcessBlockstate：它会写 Block/Variant/MultipartModelCache，
        // 与其它模型线程的无锁读取并发时会损坏 unordered_map 堆结构。
        if (blockModel.vertices.empty() && !blockName.empty() &&
            !blockstateCachesFrozen.load(std::memory_order_acquire)) {
            ProcessBlockstate(ns, {blockName});
            blockModel = PickShowContentModel(ns, blockName, showRng);
        }

        // 展台内嵌方块统一加 yuushya:showblock# 前缀，材质名归属到展台方块，
        // 原方块 id 保留在 # 之后供贴图查找。
        // GetRandomModelFromCache 返回拷贝，改名不会写回缓存。
        for (auto& mat : blockModel.materials) {
            if (mat.name.rfind("yuushya:showblock#", 0) != 0)
                mat.name = "yuushya:showblock#" + mat.name;
        }

        // 将所有面设置为DO_NOT_CULL,确保不会被贪心合并算法错误剔除
        for (auto& face : blockModel.faces) {
            face.faceDirection = DO_NOT_CULL;
        }

        // 应用变换顺序:缩放 -> 旋转 -> 平移
        ApplyRotationToVertices(std::span<float>(blockModel.vertices.data(), blockModel.vertices.size()), rx, ry, rz);

        ApplyDoublePositionOffset(blockModel, tx, ty, tz);
        ApplyScaleToVertices(std::span<float>(blockModel.vertices.data(), blockModel.vertices.size()), sx, sy, sz);

        // 合并模型
        if (mainModel.vertices.empty()) mainModel = blockModel;
        else MergeModelsDirectly(mainModel, blockModel);
    }
    // 展示方块自身的 facing: Yuushya ShowBlockModel 先绕方块中心按 Y 轴旋转
    // -facing.toYRot() (south=0, west=90, north=180, east=270), 再对每个内容方块做
    // 缩放/平移/旋转, 所以整体旋转放在最后。
    if (!mainModel.vertices.empty()) {
        const int selfId = GetBlockId(x, y, z);
        const float yRot = selfId < 0 ? 0.0f : ShowBlockFacingYRot(GetBlockById(selfId).name);
        if (yRot != 0.0f) {
            ApplyRotationToVertices(std::span<float>(mainModel.vertices.data(), mainModel.vertices.size()),
                0.0f, -yRot, 0.0f);
        }
    }
    ApplyPositionOffset(mainModel, x, y, z); // 最终整体偏移
    return mainModel;
}

void LittleTilesTilesEntity::PrintDetails() const {
    std::cout << "LittleTilesTilesEntity - ID: " << id
        << ", X: " << x << ", Y: " << y << ", Z: " << z
        << ", Grid: " << grid << std::endl;

    for (const auto& tile : tiles) {
        std::cout << " Tile - BlockName: " << tile.blockName << std::endl;

        // 打印颜色数组
        std::cout << "  Color: ";
        for (int c : tile.color) {
            std::cout << c << " ";
        }
        std::cout << std::endl;

        // 打印所有 boxData
        if (!tile.boxDataList.empty()) {
            std::cout << "  Boxes:" << std::endl;
            for (size_t i = 0; i < tile.boxDataList.size(); ++i) {
                std::cout << "   Box " << i << ": ";
                for (int v : tile.boxDataList[i]) {
                    std::cout << v << " ";
                }
                std::cout << std::endl;
            }
        }
        else {
            std::cout << "  (No boxes)" << std::endl;
        }
    }

    if (!children.empty()) {
        std::cout << " Children:" << std::endl;
        for (size_t i = 0; i < children.size(); ++i) {
            const auto& child = children[i];
            std::cout << "  Child " << i << " - Coord: ";
            for (int c : child.coord) std::cout << c << " ";
            std::cout << std::endl;
            for (const auto& tile : child.tiles) {
                std::cout << "    Tile - BlockName: " << tile.blockName << std::endl;
            }
        }
    }
}

// 检查面是否被完全覆盖,如果是则应被剔除
static bool IsFaceCovered(LittleFaceState state) {
    return state == LittleFaceState::INSIDE_COVERED || state == LittleFaceState::OUTSIDE_COVERED;
}

ModelData CreateCube(float minX, float minY, float minZ, float maxX, float maxY, float maxZ, const ModelData& templateModel, std::span<const int> faceStates) {
    ModelData cubeModel;

    // 1. 复制材质
    cubeModel.materials = templateModel.materials;

    // 2. 构建面-材质映射
    std::map<FaceType, int> faceMaterialMap;
    int anySideMaterial = -1;
    for (const auto& face : templateModel.faces) {
        if (faceMaterialMap.find(face.faceDirection) == faceMaterialMap.end()) {
            faceMaterialMap[face.faceDirection] = face.materialIndex;
        }
        if (anySideMaterial == -1 && (face.faceDirection == NORTH || face.faceDirection == SOUTH || face.faceDirection == EAST || face.faceDirection == WEST)) {
            anySideMaterial = face.materialIndex;
        }
    }

    auto getMaterialForFace = [&](FaceType dir) -> int {
        auto it = faceMaterialMap.find(dir);
        if (it != faceMaterialMap.end()) {
            return it->second;
        }
        // 侧面的备用方案
        if (dir == NORTH || dir == SOUTH || dir == EAST || dir == WEST) {
            if (anySideMaterial != -1) return anySideMaterial;
        }
        // 顶部/底部的备用方案
        auto it_up = faceMaterialMap.find(UP);
        if (it_up != faceMaterialMap.end()) return it_up->second;
        auto it_down = faceMaterialMap.find(DOWN);
        if (it_down != faceMaterialMap.end()) return it_down->second;

        if (anySideMaterial != -1) return anySideMaterial; // 再次尝试侧面
        if (!faceMaterialMap.empty()) return faceMaterialMap.begin()->second; // 尝试任何一个
        return 0; // 最后手段
    };

    // 3. 为6个独立的面创建顶点 (每个面4个顶点)
    cubeModel.vertices = {
        // 上 (Y max). 从顶部看逆时针: (minX,maxY,minZ), (maxX,maxY,minZ), (maxX,maxY,maxZ), (minX,maxY,maxZ)
        minX, maxY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   minX, maxY, maxZ,
        // 下 (Y min). 从底部看逆时针: (minX,minY,maxZ), (maxX,minY,maxZ), (maxX,minY,minZ), (minX,minY,minZ)
        minX, minY, maxZ,   maxX, minY, maxZ,   maxX, minY, minZ,   minX, minY, minZ,
        // 东 (X max). 从东面看逆时针: (maxX,minY,minZ), (maxX,maxY,minZ), (maxX,maxY,maxZ), (maxX,minY,maxZ)
        maxX, minY, minZ,   maxX, maxY, minZ,   maxX, maxY, maxZ,   maxX, minY, maxZ,
        // 西 (X min). 从西面看逆时針: (minX,minY,maxZ), (minX,maxY,maxZ), (minX,maxY,minZ), (minX,minY,minZ)
        minX, minY, maxZ,   minX, maxY, maxZ,   minX, maxY, minZ,   minX, minY, minZ,
        // 北 (Z min). 从北面看逆时针: (minX,minY,minZ), (maxX,minY,minZ), (maxX,maxY,minZ), (minX,maxY,minZ)
        minX, minY, minZ,   maxX, minY, minZ,   maxX, maxY, minZ,   minX, maxY, minZ,
        // 南 (Z max). 从南面看逆时针: (maxX,minY,maxZ), (minX,minY,maxZ), (minX,maxY,maxZ), (maxX,maxY,maxZ)
        maxX, minY, maxZ,   minX, minY, maxZ,   minX, maxY, maxZ,   maxX, maxY, maxZ
    };

    // 4. UV坐标 (每个顶点一对)
    cubeModel.uvCoordinates = {
        // 上 (XZ平面)
        minX, minZ,   maxX, minZ,   maxX, maxZ,   minX, maxZ,
        // 下 (XZ平面)
        minX, maxZ,   maxX, maxZ,   maxX, minZ,   minX, minZ,
        // 东 (ZY平面)
        minZ, minY,   minZ, maxY,   maxZ, maxY,   maxZ, minY,
        // 西 (ZY平面)
        maxZ, minY,   maxZ, maxY,   minZ, maxY,   minZ, minY,
        // 北 (XY平面)
        minX, minY,   maxX, minY,   maxX, maxY,   minX, maxY,
        // 南 (XY平面)
        maxX, minY,   minX, minY,   minX, maxY,   maxX, maxY
    };

    // 5. 面 - 根据faceStates决定是否生成
    struct FaceInfo {
        FaceType type;
        std::array<int, 4> vertIndices;
        std::array<int, 4> uvIndices;
    };

    const std::vector<FaceInfo> faceInfos = {
        {UP,    {0, 1, 2, 3},    {0, 1, 2, 3}},
        {DOWN,  {4, 5, 6, 7},    {4, 5, 6, 7}},
        {EAST,  {8, 9, 10, 11},  {8, 9, 10, 11}},
        {WEST,  {12, 13, 14, 15}, {12, 13, 14, 15}},
        {NORTH, {16, 17, 18, 19}, {16, 17, 18, 19}},
        {SOUTH, {20, 21, 22, 23}, {20, 21, 22, 23}}
    };

    // faceStates order after ParseLittleTilesTiles nibble split:
    //   [0]=UP [1]=DOWN [2]=SOUTH [3]=NORTH [4]=EAST [5]=WEST
    // (matches CreativeCore Facing pack DOWN,UP,NORTH,SOUTH,WEST,EAST via byte/nibble extract)
    std::map<FaceType, LittleFaceState> stateMap;
    if (faceStates.size() >= 6) {
        stateMap[FaceType::UP] = static_cast<LittleFaceState>(faceStates[0]);
        stateMap[FaceType::DOWN] = static_cast<LittleFaceState>(faceStates[1]);
        stateMap[FaceType::SOUTH] = static_cast<LittleFaceState>(faceStates[2]);
        stateMap[FaceType::NORTH] = static_cast<LittleFaceState>(faceStates[3]);
        stateMap[FaceType::EAST] = static_cast<LittleFaceState>(faceStates[4]);
        stateMap[FaceType::WEST] = static_cast<LittleFaceState>(faceStates[5]);
    }


    for (const auto& info : faceInfos) {
        LittleFaceState state = LittleFaceState::UNLOADED;
        if (stateMap.count(info.type)) {
            state = stateMap.at(info.type);
        }
        if (!IsFaceCovered(state)) {
            cubeModel.faces.push_back({ info.vertIndices, info.uvIndices, getMaterialForFace(info.type), info.type });
        }
    }

    return cubeModel;
}

// --- 新增辅助函数 ---
// 根据给定的tiles和grid生成模型,不应用最终的位置偏移
static ModelData GenerateModelFromTiles(const std::vector<LittleTilesTileEntry>& tiles, int grid) {
    ModelData model;
    float grid_f = static_cast<float>(grid);

    for (const auto& tile : tiles) {
        // 获取方块类型的模板模型
        std::string fullBlockName = tile.blockName;
        std::string ns = "minecraft"; // 默认命名空间
        std::string blockName;
        size_t colonPos = fullBlockName.find(':');
        if (colonPos != std::string::npos) {
            ns = fullBlockName.substr(0, colonPos);
            blockName = fullBlockName.substr(colonPos + 1);
        }
        else {
            blockName = fullBlockName;
        }

        ModelData templateModel = GetRandomModelFromCache(ns, blockName);
        // 同上：冻结后的模型阶段只读缓存，禁止并发惰性写入。
        if (templateModel.vertices.empty() && !blockName.empty() &&
            !blockstateCachesFrozen.load(std::memory_order_acquire)) {
            ProcessBlockstate(ns, { blockName });
            templateModel = GetRandomModelFromCache(ns, blockName);
        }

        if (templateModel.materials.empty()) {
            // 如果没有材质,创建一个虚拟材质以防止崩溃
            Material dummyMaterial;
            dummyMaterial.name = "dummy";
            dummyMaterial.texturePath = "None";
            templateModel.materials.push_back(dummyMaterial);
        }

        // LittleTiles color tint (ARGB). -1 = ColorUtils default / no tint.
        if (!tile.color.empty() && tile.color[0] != -1) {
            const uint32_t argb = static_cast<uint32_t>(tile.color[0]);
            TintResult fixed;
            fixed.kind = TintKind::Fixed;
            fixed.color = argb & 0xFFFFFFu;
            for (auto& mat : templateModel.materials) {
                if (mat.tintLocked) continue;
                mat.tint = fixed;
                mat.tintLocked = true;
                mat.name += TintSuffix(fixed);
            }
        }

        for (const auto& boxData : tile.boxDataList) {
            if (boxData.size() != 12) continue; // 现在需要12个元素

            // 从 boxData 中提取位置和大小
            int minX = boxData[6];
            int minY = boxData[7];
            int minZ = boxData[8];
            int maxX = boxData[9];
            int maxY = boxData[10];
            int maxZ = boxData[11];

            // 为该 box 创建立方体模型,并传入面状态
            ModelData cube = CreateCube(minX / grid_f, minY / grid_f, minZ / grid_f, maxX / grid_f, maxY / grid_f, maxZ / grid_f, templateModel, std::span<const int>(boxData.data(), 6));

            // 合并到最终模型中
            if (model.vertices.empty()) {
                model = cube;
            }
            else {
                MergeModelsDirectly(model, cube);  // 假设 MergeModelsDirectly 已定义
            }
        }
    }
    return model;
}

ModelData LittleTilesTilesEntity::GenerateModel() const {
    // 1. 生成父结构的本地模型
    ModelData finalModel = GenerateModelFromTiles(this->tiles, this->grid);

    // 2. 遍历子结构,生成它们的模型并合并
    for (const auto& child : this->children) {
        // 假设子结构继承父结构的 grid 设置
        ModelData childModel = GenerateModelFromTiles(child.tiles, this->grid);

        // 应用子结构的相对坐标偏移(根据grid缩放)
        if (child.coord.size() >= 3) {
            float grid_f = static_cast<float>(this->grid);
            double offsetX = static_cast<double>(child.coord[0]) / grid_f;
            double offsetY = static_cast<double>(child.coord[1]) / grid_f;
            double offsetZ = static_cast<double>(child.coord[2]) / grid_f;
            ApplyDoublePositionOffset(childModel, offsetX, offsetY, offsetZ);
        }

        // 合并到最终模型中
        if (finalModel.vertices.empty()) {
            finalModel = childModel;
        }
        else {
            MergeModelsDirectly(finalModel, childModel);
        }
    }

    // 3. 对最终合并的模型应用实体自身的绝对坐标偏移
    ApplyPositionOffset(finalModel, x, y, z);
    return finalModel;
}
