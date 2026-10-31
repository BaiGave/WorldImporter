// ChunkGenerator.cpp
#include "ChunkGenerator.h"
#include "BeaconBeam.h"
#include "RegionModelExporter.h"
#include "locutil.h"
#include "objExporter.h"
#include "CreateBlazePackager.h"
#include "include/stb_image.h"
#include "biome.h"
#include "CreateSpoutCannonPulley.h"
#include "SignText.h"
#include "model.h"
#include "CreateBellCrafter.h"
#include "blocktint.h"
#include "fluid.h"
#include "CreateTrackMarkers.h"
#include "LODManager.h"
#include "CreateSlidingDoor.h"
#include "CTM.h"
#include "CreateCT.h"
#include "CreateFlaps.h"
#include "ModCT.h"
#include "CreateFrogportPsi.h"
#include "texture.h"
#include "Occlusion.h"
#include "BannerModel.h"
#include <iomanip>
#include <cmath>
#include <sstream>
#include <regex>
#include "CreateCurvedTrack.h"
#include <tuple>
#include <future>
#include <chrono>
#include <iostream>
#include <thread>
#include "CreateBogey.h"
#include <atomic>
#include "ModelDeduplicator.h"
#include "CreateDepotItems.h"
#include "CreateFilterItems.h"
#include "ChunkGroupAllocator.h"
#include <utility>
#include "hashutils.h"
#include "PlayerSkin.h"
#include "ChunkLoader.h"
#include "CreatePostboxControls.h"
#include "SpecialBlock.h"
#include "CreateTankFluid.h"
#include "CampfireItems.h"
#include "CableMesher.h"
#include "CreateChainConveyor.h"
#include "BbsModelSupport.h"
#include "CreateBoilerGauge.h"
#include "plantoffset.h"
using namespace std;
using namespace std::chrono;

namespace {
    // 进入区块时记下它的 DataVersion，供加权变体挑选使用。
    struct ChunkVariantVersionScope {
        std::optional<int> saved;
        ChunkVariantVersionScope(int chunkX, int chunkZ) : saved(g_activeChunkDataVersion) {
            g_activeChunkDataVersion = GetChunkDataVersion(chunkX, chunkZ);
        }
        ~ChunkVariantVersionScope() { g_activeChunkDataVersion = saved; }
        ChunkVariantVersionScope(const ChunkVariantVersionScope&) = delete;
    };

    // Indexed by the frozen global palette; rebuilt between batches only.
    struct PreparedBlockName { std::string ns, local, full; };
    std::vector<PreparedBlockName> preparedBlockNames;
}

// 饰纹陶罐: 方块实体 sherds 中有图案时按图案重建侧面
static void ApplyPotSherds(ModelData& model, const std::string& ns, const std::string& blockName, const NbtTagPtr& nbt) {
    if (!nbt || model.vertices.empty() || ns != "minecraft" || blockName.rfind("decorated_pot", 0) != 0 ||
        (blockName.size() > 13 && blockName[13] != '[')) return;
    ModelData pot = SpecialBlock::GenerateDecoratedPotModel(blockName, nbt);
    if (!pot.vertices.empty()) model = std::move(pot);
}

void ChunkGenerator::PrepareBlockNames() {
    // Palette indices are append-only across export batches. Keep already
    // prepared names instead of reallocating every block-state string each time.
    if (preparedBlockNames.size() > globalBlockPalette.size()) preparedBlockNames.clear();
    preparedBlockNames.reserve(globalBlockPalette.size());
    for (size_t i = preparedBlockNames.size(); i < globalBlockPalette.size(); ++i) {
        const Block& block = globalBlockPalette[i];
        const std::string full = block.GetModifiedNameWithNamespace();
        const size_t colon = full.find(':');
        preparedBlockNames.push_back({block.GetNamespace(),
            colon == std::string::npos ? full : full.substr(colon + 1), full});
    }
}

static const std::unordered_map<FaceType, int> neighborIndexMap = {
        {FaceType::DOWN, 1}, {FaceType::UP, 0}, {FaceType::NORTH, 4},
        {FaceType::SOUTH, 5}, {FaceType::WEST, 2}, {FaceType::EAST, 3}
};

// 面朝向对应的单位法线（用于薄片贴边外推与退化面判断）
static bool FaceDirectionNormal(FaceType direction, float& nx, float& ny, float& nz) {
    switch (direction) {
    case FaceType::UP:    nx = 0.0f;  ny = 1.0f;  nz = 0.0f;  return true;
    case FaceType::DOWN:  nx = 0.0f;  ny = -1.0f; nz = 0.0f;  return true;
    case FaceType::SOUTH: nx = 0.0f;  ny = 0.0f;  nz = 1.0f;  return true;
    case FaceType::NORTH: nx = 0.0f;  ny = 0.0f;  nz = -1.0f; return true;
    case FaceType::EAST:  nx = 1.0f;  ny = 0.0f;  nz = 0.0f;  return true;
    case FaceType::WEST:  nx = -1.0f; ny = 0.0f;  nz = 0.0f;  return true;
    default:              nx = 0.0f;  ny = 0.0f;  nz = 0.0f;  return false;
    }
}

// 反方向（用于查询"邻居朝向本块"的那一面）。返回值是 Occlusion.h 的
// UP=0, DOWN=1, NORTH=2, SOUTH=3, WEST=4, EAST=5 索引，与 neighborIndexMap 不同。
static int OppositeFaceDir(FaceType direction) {
    switch (direction) {
    case FaceType::UP:    return 1;   // DOWN
    case FaceType::DOWN:  return 0;   // UP
    case FaceType::NORTH: return 3;   // SOUTH
    case FaceType::SOUTH: return 2;   // NORTH
    case FaceType::WEST:  return 5;   // EAST
    case FaceType::EAST:  return 4;   // WEST
    default:              return -1;
    }
}

// 轴向 + 正负 -> 面朝向（axis: 0=X, 1=Y, 2=Z）
static FaceType AxisToFaceType(int axis, bool positive) {
    if (axis == 0) return positive ? FaceType::EAST : FaceType::WEST;
    if (axis == 1) return positive ? FaceType::UP : FaceType::DOWN;
    return positive ? FaceType::SOUTH : FaceType::NORTH;
}

// 取面的几何法线主轴（用于没有 cullface 的面：贴片模型的 faceDirection 是 DO_NOT_CULL，
// 只能按实际几何朝向判断它面向哪个邻居）。用面的前三个顶点叉积，取绝对值最大的分量。
static bool FaceGeometryAxis(const ModelData& model, const Face& face, int& axis, bool& positive) {
    int idx[3];
    for (int k = 0; k < 3; ++k) {
        idx[k] = face.vertexIndices[k];
        if (idx[k] < 0 || static_cast<size_t>(idx[k]) * 3 + 2 >= model.vertices.size()) return false;
    }
    const float* p0 = &model.vertices[static_cast<size_t>(idx[0]) * 3];
    const float* p1 = &model.vertices[static_cast<size_t>(idx[1]) * 3];
    const float* p2 = &model.vertices[static_cast<size_t>(idx[2]) * 3];
    const float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    const float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
    const float n[3] = {
        e1[1] * e2[2] - e1[2] * e2[1],
        e1[2] * e2[0] - e1[0] * e2[2],
        e1[0] * e2[1] - e1[1] * e2[0]
    };
    const float ax = std::fabs(n[0]), ay = std::fabs(n[1]), az = std::fabs(n[2]);
    if (ax < 1e-9f && ay < 1e-9f && az < 1e-9f) return false;   // 退化面
    if (ax >= ay && ax >= az) { axis = 0; positive = n[0] > 0.0f; }
    else if (ay >= az)        { axis = 1; positive = n[1] > 0.0f; }
    else                      { axis = 2; positive = n[2] > 0.0f; }
    return true;
}

// --------------------------------------------------------------------------------
// 原版流体渲染辅助（FluidRenderer）
// --------------------------------------------------------------------------------
namespace {

// 方向索引与 FaceType 一致：UP=0, DOWN=1, NORTH=2, SOUTH=3, WEST=4, EAST=5
enum : int {
    FLUID_DIR_UP = 0, FLUID_DIR_DOWN = 1, FLUID_DIR_NORTH = 2,
    FLUID_DIR_SOUTH = 3, FLUID_DIR_WEST = 4, FLUID_DIR_EAST = 5
};

inline bool IsSameFluidName(const Block& block, const std::string& fluidName) {
    return block.HasFluid() && block.fluidName == fluidName;
}

// 原版 Vec3#normalize：长度过小视为零向量
inline void NormalizeFlow(float& flowX, float& flowZ) {
    const float len = std::sqrt(flowX * flowX + flowZ * flowZ);
    if (len < 1.0e-4f) {
        flowX = 0.0f;
        flowZ = 0.0f;
        return;
    }
    flowX /= len;
    flowZ /= len;
}

} // namespace

void ChunkGenerator::ProcessBlockForModel(ModelData& chunkModel, int x, int y, int z,
    std::unordered_map<std::string, int>* materialLookup) {
    std::array<bool, 6> neighbors; // 邻居是否不遮挡（true = 该方向的面应渲染）

    // Skip air before doing neighbor/occlusion work.
    int id = GetBlockId(x, y, z);
    Block currentBlock = GetBlockById(id);
    if (currentBlock.name == "minecraft:air") return;
    GetBlockIdWithNeighbors(x, y, z, neighbors.data());
    const PreparedBlockName* prepared = globalPaletteFrozen.load(std::memory_order_acquire) &&
        id >= 0 && static_cast<size_t>(id) < preparedBlockNames.size()
        ? &preparedBlockNames[id] : nullptr;
    string blockName = prepared ? prepared->full : currentBlock.GetModifiedNameWithNamespace();

    if (config.exportLightBlockOnly)
    {
        string processed = blockName;

        // 提取命名空间
        size_t colonPos = processed.find(':');
        string ns = "minecraft"; // 默认命名空间
        if (colonPos != string::npos) {
            ns = processed.substr(0, colonPos);
            processed = processed.substr(colonPos + 1);
        }

        // 提取方块ID和状态
        size_t bracketPos = processed.find('[');
        string LN = processed.substr(0, bracketPos);


        if (LN != "light") {
            return;
        }
    }
    if (config.cullCave)
    {
        if (GetSkyLight(x, y, z) == -1) return;
    }

    string ns = prepared ? prepared->ns : currentBlock.GetNamespace();

    // 标准化方块名称(去掉命名空间,处理状态)
    if (prepared) {
        blockName = prepared->local;
    } else {
        size_t colonPos = blockName.find(':');
        if (colonPos != string::npos) blockName = blockName.substr(colonPos + 1);
    }

    ModelData blockModel;
    ModelData liquidModel;
    NbtTagPtr blockEntityNbt = GetBlockEntityNbt(x, y, z);
    bool specialHandled = TryGenerateBbsModel(blockEntityNbt, x, y, z, blockModel);
    if (!specialHandled) {
        specialHandled = SpecialBlock::TryGenerateCreateBlockModel(
        prepared ? prepared->full : currentBlock.GetModifiedNameWithNamespace(), x, y, z,
            blockEntityNbt, blockModel);
    }
    if (!specialHandled) {
        specialHandled = TryGenerateCableModel(blockModel, ns, blockName, x, y, z, blockEntityNbt);
    }
    if (!specialHandled && ns == "minecraft") {
        // 旗帜: 实体渲染方块, 图案存放在方块实体里
        specialHandled = TryGenerateBannerModel(
            prepared ? prepared->full : currentBlock.GetModifiedNameWithNamespace(), blockEntityNbt, blockModel);
    }
    if (specialHandled) {
        for (auto& face : blockModel.faces) face.faceDirection = FaceType::DO_NOT_CULL;
    }
    else if (currentBlock.HasFluid()) {
        // ============================ 原版流体渲染路径 ============================
        // 高度/角高/UV/贴图选择与逐面剔除完全按原版 FluidRenderer 实现。
        const std::string& fluidName = currentBlock.fluidName;

        // 原版 FluidRenderer#getHeight（单位：格）：
        //   邻居同种流体 -> 上方仍是同种流体 ? 1.0 : ownHeight(液位)
        //   非流体       -> solidLike ? -1.0 : 0.0
        auto renderHeight = [&](int bx, int by, int bz) -> float {
            int nid = GetBlockId(bx, by, bz);
            Block nb = GetBlockById(nid);
            if (IsSameFluidName(nb, fluidName)) {
                int upId = GetBlockId(bx, by + 1, bz);
                Block up = GetBlockById(upId);
                if (IsSameFluidName(up, fluidName)) return 1.0f;
                return GetFluidOwnHeight(nb.level);
            }
            return GetBlockOcclusion(nid).solidLike ? -1.0f : 0.0f;
        };

        FluidModelParams params;
        params.selfHeight = renderHeight(x, y, z);
        params.sideHeight[0] = renderHeight(x, y, z - 1); // 北
        params.sideHeight[1] = renderHeight(x, y, z + 1); // 南
        params.sideHeight[2] = renderHeight(x - 1, y, z); // 西
        params.sideHeight[3] = renderHeight(x + 1, y, z); // 东
        params.cornerHeight[0] = renderHeight(x - 1, y, z - 1); // 西北
        params.cornerHeight[1] = renderHeight(x + 1, y, z - 1); // 东北
        params.cornerHeight[2] = renderHeight(x + 1, y, z + 1); // 东南
        params.cornerHeight[3] = renderHeight(x - 1, y, z + 1); // 西南
        params.falling = currentBlock.level >= 8;

        // ---- 原版 FlowingFluid#getFlow（水平分量；falling 的 -6Y 不改变角度与"是否为零"）----
        {
            const float selfOwn = GetFluidOwnHeight(currentBlock.level);
            static const int dirs[4][3] = { {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0} };
            for (const auto& d : dirs) {
                const int nx = x + d[0];
                const int nz = z + d[2];
                const int nid = GetBlockId(nx, y, nz);
                Block nb = GetBlockById(nid);
                const bool isAir = nb.air;
                const bool same = IsSameFluidName(nb, fluidName);
                if (!(isAir || same)) continue; // affectsFlow

                float calc = 0.0f;
                if (same) {
                    const float nh = GetFluidOwnHeight(nb.level);
                    if (nh != 0.0f) calc = selfOwn - nh;
                }
                else {
                    // 邻居为空气：看其下方是否仍有同种流体（原版）
                    const int bid = GetBlockId(nx, y - 1, nz);
                    Block below = GetBlockById(bid);
                    const bool belowSame = IsSameFluidName(below, fluidName);
                    const float belowH = belowSame ? GetFluidOwnHeight(below.level) : 0.0f;
                    if (!GetBlockOcclusion(nid).solidLike && (below.air || belowSame) && belowH > 0.0f) {
                        calc = 8.0f / 9.0f;
                    }
                }
                if (calc != 0.0f) {
                    params.flowX += calc * static_cast<float>(d[0]);
                    params.flowZ += calc * static_cast<float>(d[2]);
                }
            }
            NormalizeFlow(params.flowX, params.flowZ);
        }

        // ---- 原版逐面渲染判定 ----
        const int upId = GetBlockId(x, y + 1, z);
        const int downId = GetBlockId(x, y - 1, z);
        Block upBlock = GetBlockById(upId);
        Block downBlock = GetBlockById(downId);

        // 自身遮挡（原版 FluidRenderer#isFaceOccludedBySelf）：含水方块的水贴合方块形状，
        // 方块自身在同一平面上的面并集完整覆盖该方向时，水的那一面不渲染。顶面按原版不做自遮挡。
        const BlockOcclusion& selfOcc = GetBlockOcclusion(id);

        // 顶面：上方同种流体不渲染（原版 renderUp 只判同种流体）
        params.keepFace[1] = !IsSameFluidName(upBlock, fluidName);
        // 底面：自身遮挡 / 邻居同种流体 / 邻居是完整遮挡体时不渲染
        params.keepFace[0] = !IsSameFluidName(downBlock, fluidName) &&
            !selfOcc.selfFaceFull[FLUID_DIR_DOWN] &&
            !GetBlockOcclusion(downId).occludes;

        // 侧面：北、南、西、东
        static const int sideDirs[4][3] = { {0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0} };
        static const int sideDirIndices[4] = { FLUID_DIR_NORTH, FLUID_DIR_SOUTH, FLUID_DIR_WEST, FLUID_DIR_EAST };
        for (int i = 0; i < 4; ++i) {
            const int nx = x + sideDirs[i][0];
            const int nz = z + sideDirs[i][2];
            const int nid = GetBlockId(nx, y, nz);
            Block nb = GetBlockById(nid);
            params.keepFace[2 + i] = !IsSameFluidName(nb, fluidName) &&
                !selfOcc.selfFaceFull[sideDirIndices[i]] &&
                !GetBlockOcclusion(nid).occludes;
        }

        // 顶面原版优化：四个角高全满且上方是完整遮挡体时剔除
        if (params.keepFace[1]) {
            float corners[4]; // NW, NE, SE, SW
            ComputeFluidCornerHeights(params, corners);
            const float minH = std::min(std::min(corners[0], corners[1]), std::min(corners[2], corners[3]));
            if (minH >= 1.0f && GetBlockOcclusion(upId).occludes) {
                params.keepFace[1] = false;
            }
        }
        params.downCanRender = params.keepFace[0];
        params.topCanRender = params.keepFace[1];

        liquidModel = GenerateFluidModel(params, fluidName);
        AssignFluidMaterials(liquidModel, fluidName);

        ModelData waterloggedBlockModel = GetRandomModelFromCache(ns, blockName, BlockPosSeed(x, y, z));
        // 原版 OffsetType 只平移方块模型。流体在合并之前保持格子中心。
        ApplyVanillaPlantOffset(waterloggedBlockModel, ns, blockName, x, z);
        ApplyPotSherds(waterloggedBlockModel, ns, blockName, blockEntityNbt);
        if (waterloggedBlockModel.vertices.empty()) {
            blockModel = liquidModel;
        }
        else {
            // 水方块自身带模型（含水方块等）：保留原有 DO_NOT_CULL 处理，再与流体模型合并
            for (auto& face : waterloggedBlockModel.faces)
            {
                FaceType dir = face.faceDirection;
                if (dir != FaceType::DO_NOT_CULL) {
                    auto it = neighborIndexMap.find(dir);
                    if (it != neighborIndexMap.end()) {
                        int nx = x, ny = y, nz = z;
                        if (dir == FaceType::DOWN) ny--;
                        else if (dir == FaceType::UP) ny++;
                        else if (dir == FaceType::NORTH) nz--;
                        else if (dir == FaceType::SOUTH) nz++;
                        else if (dir == FaceType::WEST) nx--;
                        else if (dir == FaceType::EAST) nx++;

                        int neighborId = GetBlockId(nx, ny, nz);
                        Block neighborBlock = GetBlockById(neighborId);
                        if (neighborBlock.HasFluid()) {
                            face.faceDirection = FaceType::DO_NOT_CULL;
                        }
                    }
                }
            }

            blockModel = MergeFluidModelData(waterloggedBlockModel, liquidModel);
        }
    }
    else
    {
        // 处理其他方块
        blockModel = GetRandomModelFromCache(ns, blockName, BlockPosSeed(x, y, z));
        ApplyPotSherds(blockModel, ns, blockName, blockEntityNbt);
        // 钟: 方块模型只有支架, 钟体由 BellRenderer 绘制
        if (ns == "minecraft" && blockName.rfind("bell", 0) == 0 && (blockName.size() == 4 || blockName[4] == '[') &&
            !blockModel.vertices.empty()) {
            static const ModelData bellBody = SpecialBlock::GenerateBlockEntityModel("minecraft:bell");
            if (!bellBody.faces.empty()) MergeModelsDirectly(blockModel, bellBody);
        }
        // 原版 OffsetType 随机位移(草/花/蕨等)。含水方块的模型在流体分支里平移过,
        // 这里只处理非流体分支, 免得位移叠两层。
        ApplyVanillaPlantOffset(blockModel, ns, blockName, x, z);
    }

    // Player heads: swap the shared Steve material for the profile skin.
    // All faces of the skull model use that one material.
    if (ns == "minecraft" && blockEntityNbt) {
        std::string headId = blockName;
        size_t br = headId.find('[');
        if (br != std::string::npos) headId.resize(br);
        if (headId == "player_head" || headId == "player_wall_head") {
            const std::string skin = ResolvePlayerSkinTexture(blockEntityNbt);
            if (!skin.empty() && !blockModel.materials.empty()) {
                const std::string marker = "entity/player_skins/";
                std::string key;
                size_t p = skin.find(marker);
                if (p != std::string::npos) {
                    key = skin.substr(p + marker.size());
                    size_t dot = key.find('.');
                    if (dot != std::string::npos) key.resize(dot);
                } else {
                    key = skin;
                    const std::string prefix = "textures/";
                    if (key.rfind(prefix, 0) == 0) key = key.substr(prefix.size());
                    if (key.size() >= 4 && key.compare(key.size() - 4, 4, ".png") == 0)
                        key.resize(key.size() - 4);
                    for (char& c : key) {
                        if (c == '/' || c == '\\' || c == ':') c = '_';
                    }
                }
                const std::string matName = "minecraft:player_head#skin_" + key;
                for (auto& mat : blockModel.materials) {
                    mat.name = matName;
                    mat.texturePath = skin;
                    mat.type = NORMAL;
                    mat.aspectRatio = 1.0f;
                }
            }
        }
    }

    // Create depot / ejector / belt: held and transported items appended to the block model.
    if (ns == "create" && blockEntityNbt)
        AppendCreateDepotItems("create:" + blockName, blockEntityNbt, blockModel);

    // Create filter-slot items (FilteringRenderer.renderOnBlockEntity).
    if (ns == "create" && blockEntityNbt)
        AppendCreateFilterItems("create:" + blockName, blockEntityNbt, blockModel);

    // Copycats / Create copycat: material camo may rebuild empty air placeholders.
    if (blockEntityNbt && (ns == "copycats" ||
        (ns == "create" && blockName.rfind("copycat_", 0) == 0))) {
        ApplyCopycatCamoToBlockModel(blockModel, blockEntityNbt, ns, blockName, x, y, z);
    }
    // Create sliding doors with visible=false are drawn by SlidingDoorRenderer.
    if (ns == "create") ApplyCreateSlidingDoor(ns, blockName, blockModel);

    // Sign boards on 1.21 and earlier are particle-only on this branch, so the
    // block model is empty and the early returns below would drop the text.
    // Boards that do exist are not fully culled; this still covers that path.
    // The lambda is also invoked after the board merge at the end of this function.
    auto appendSignText = [&]() {
        if (ns != "minecraft" || blockName.find("sign") == std::string::npos) return;
        ModelData textModel;
        if (!BuildSignTextModel(ns, blockName, blockEntityNbt, x, y, z, textModel)) return;
        if (chunkModel.vertices.empty()) chunkModel = std::move(textModel);
        else MergeModelsDirectly(chunkModel, textModel, materialLookup);
    };

    if (blockModel.vertices.empty()) {
        appendSignText();
        return;
    }

    // Create frogport / portable interfaces: BE-rendered partials appended to the block model.
    if (ns == "create")
        AppendCreateFrogportPsi("create:" + blockName, x, y, z, blockEntityNbt, blockModel);

    // FramedBlocks: retexture frame materials from BE camo (shape stays).
    // Double/fluid camo + position camo alias for Mek CT connect groups.
    if (ns == "framedblocks" && blockEntityNbt) {
        ApplyFramedCamoToBlockModel(blockModel, blockEntityNbt, blockName, x, y, z);
    }

    // Create 的连接纹理是运行时 UV 重映射(自带 *_connected.png), 与 OptiFine CTM
    // 是两套系统, 单独走 CreateCT 路径。
    bool useCtm = false;
    // Gearbox / clutch / gearshift are SpecialBlock-handled only to append
    // SHAFT_HALF partials; their casing still needs CreateCT (EncasedCTBehaviour).
    bool createSpecialNeedsCT = false;
    if (specialHandled && ns == "create") {
        std::string base = "create:" + blockName;
        size_t br = base.find('[');
        if (br != std::string::npos) base = base.substr(0, br);
        createSpecialNeedsCT =
            base == "create:gearbox" || base == "create:clutch" ||
            base == "create:gearshift" || base == "create:sequenced_gearshift" ||
            base == "create:encased_fluid_pipe";
    }
    // Create spout / schematicannon / pulleys: BE-rendered partials appended to the block model.
    if (ns == "create")
        AppendCreateSpoutCannonPulley("create:" + blockName, blockEntityNbt, blockModel);
    if ((!specialHandled || createSpecialNeedsCT) && ns == "create") {
        useCtm = ApplyCreateCTToBlockModel(blockModel, ns, blockName, x, y, z);
    }
    // OptiFine/Continuity 对任意形状的面都按贴图匹配 CTM(台阶、半砖等非满方块也一样)。
    if (!useCtm && !specialHandled && HasCtmRules() && ns != "create") {
        ApplyCtmToBlockModel(blockModel, ns, blockName, x, y, z);
        useCtm = true;
    }

    if (ns == "create" && blockEntityNbt)
        AppendCreateTankFluid("create:" + blockName, blockEntityNbt, blockModel);

    // 解析 tint 并按需拆分材质（必须在 CTM 之后、面剔除之前）
    ApplyTintToBlockModel(blockModel, currentBlock.name);
    if (ns == "create" && blockEntityNbt)
        AppendCreateBoilerGauge("create:" + blockName, x, y, z, blockEntityNbt, blockModel);

    // 剔除被遮挡的面
    std::vector<int> validFaceIndices;
    // Create haunted/peculiar bell bodies and mechanical crafter cog/lid/belt/arrow (DO_NOT_CULL).
    if (ns == "create")
        AppendCreateBellCrafter("create:" + blockName, x, y, z, blockEntityNbt, blockModel);
    validFaceIndices.reserve(blockModel.faces.size());
    // Create blaze burner and packager/repackager partials (DO_NOT_CULL).
    if (ns == "create")
        AppendCreateBlazePackager("create:" + blockName, x, y, z, blockEntityNbt, blockModel);

    // CTM 连接的同类方块之间应剔除内部面(玻璃等非固体方块不在 solids 表中,
    // 默认会被当作 air 而保留内部面,导致缝隙)
    std::string curBaseName = ns + ":" + blockName;
    size_t bracketPos = curBaseName.find('[');
    if (bracketPos != std::string::npos) {
        curBaseName = curBaseName.substr(0, bracketPos);
    }
    // Create funnel and belt-tunnel flaps (static flapness 0, DO_NOT_CULL).
    if (ns == "create")
        AppendCreateFlaps("create:" + blockName, blockEntityNbt, blockModel);
    auto isSameTransparentNeighbor = [&](FaceType dir) -> bool {
        // 仅用于「玻璃类方块」的同类内部面剔除（玻璃/染色玻璃/玻璃板等）。
        // 注意：不能对半砖、楼梯、栅栏等部分方块生效，否则会错误剔除侧面。
        int nx = x, ny = y, nz = z;
        if (dir == FaceType::DOWN) ny--;
        else if (dir == FaceType::UP) ny++;
        else if (dir == FaceType::NORTH) nz--;
        else if (dir == FaceType::SOUTH) nz++;
        else if (dir == FaceType::WEST) nx--;
        else if (dir == FaceType::EAST) nx++;
        else return false;
        int nid = GetBlockId(nx, ny, nz);
        Block nb = GetBlockById(nid);
        return nb.GetNameAndNameSpaceWithoutState() == curBaseName;
    };
    // Create station flag, signal lamp and track overlays (static, DO_NOT_CULL).
    if (ns == "create")
        AppendCreateTrackMarkers("create:" + blockName, x, y, z, blockEntityNbt, blockModel);
    // 玻璃类方块（含模组命名如 stained_glass / framed_glass / *_pane）
    const bool selfGlassLike =
        curBaseName.find("glass") != std::string::npos ||
        curBaseName.find("pane") != std::string::npos;
    // 原版 HalfTransparentBlock（冰/霜冰/黏液块/蜂蜜块）与相同方块相邻时不渲染该面
    const bool selfHalfTransparent =
        curBaseName == "minecraft:ice" || curBaseName == "minecraft:frosted_ice" ||
        curBaseName == "minecraft:slime_block" || curBaseName == "minecraft:honey_block";
    // 原版 MangroveRootsBlock 只在竖直方向与红树根相邻时不渲染该面
    const bool selfMangroveRoots = curBaseName == "minecraft:mangrove_roots";

    // Create chain conveyor shaft, wheel, guards and chains (static, DO_NOT_CULL).
    if (ns == "create")
        AppendCreateChainConveyor("create:" + blockName, x, y, z, blockEntityNbt, blockModel);

    // 遍历所有面
    for (size_t faceIdx = 0; faceIdx < blockModel.faces.size(); ++faceIdx) {
        // 检查faceIdx是否超出范围
        if (faceIdx >= blockModel.faces.size()) {
            throw std::runtime_error("faceIdx out of range");
        }

        FaceType dir = blockModel.faces[faceIdx].faceDirection; // 获取面的方向
        // 如果是DO_NOT_CULL,保留该面
        if (dir == FaceType::DO_NOT_CULL) {
            validFaceIndices.push_back(faceIdx);
        }
        else {
            auto it = neighborIndexMap.find(dir);
            if (it != neighborIndexMap.end()) {
                int neighborIdx = it->second;
                if (!neighbors[neighborIdx]) { // 邻居为完整遮挡体,跳过该面
                    continue;
                }
                // 邻居被当作 air（透明/非实心）时，只有「同类连接方块」的
                // 内部面才可剔除。旧代码把 useCtm 当成直接剔除条件，命中任意
                // CTM 规则后会删掉该块全部 cullface（玻璃/水整块消失）。
                if ((selfGlassLike || selfHalfTransparent) && isSameTransparentNeighbor(dir)) {
                    continue;
                }
                if (selfMangroveRoots && (dir == FaceType::UP || dir == FaceType::DOWN) &&
                    isSameTransparentNeighbor(dir)) {
                    continue;
                }
            }
            validFaceIndices.push_back(faceIdx);
        }
    }

    // Create postbox flag and contraption-controls button/indicator (static, DO_NOT_CULL).
    // Appended after the cull walk, so record the new face indices or the compact drops them.
    if (ns == "create") {
        const size_t postboxFaceBegin = blockModel.faces.size();
        AppendCreatePostboxControls("create:" + blockName, blockModel);
        for (size_t faceIdx = postboxFaceBegin; faceIdx < blockModel.faces.size(); ++faceIdx)
            validFaceIndices.push_back(static_cast<int>(faceIdx));
    }

    // ---- 薄片贴边外推: 极薄元素(如装饰贴片)贴在方块界面上的那一面，与相邻方块
    // 朝向本块的接触面严格共面且朝向相反。单模型内的重合面去重管不到它(顶点索引
    // 不同)，邻居遮挡也删不掉它(贴片自身在该方向没有 cullface)，在 Blender 里会
    // z-fighting。这里沿该面法线把它外推 overlayLayerStep，把两个面分开。
    //
    // 邻居判定用 hasModel + selfFaceFull[对面方向] 而不是 occludes —— occludes 要求
    // 邻居六个方向都完整覆盖(完整不透明立方体)，半砖/台阶/玻璃这类邻居会被漏掉，
    // 而它们同样会和贴片共面。selfFaceFull 的索引约定见 Occlusion.cpp:
    // UP=0, DOWN=1, NORTH=2, SOUTH=3, WEST=4, EAST=5，与 neighborIndexMap 不同。
    if (config.overlayLayerStep > 0.0f) {
        const float pushStep = config.overlayLayerStep;
        for (int faceIdx : validFaceIndices) {
            Face& face = blockModel.faces[faceIdx];
            if (!face.thinEdge) continue;
            // 贴片模型通常不声明 cullface，faceDirection 是 DO_NOT_CULL，查不到邻居索引；
            // 这种情况按面的几何法线定方向（贴片背面就是朝向邻居的那一面）
            FaceType pushDir = face.faceDirection;
            if (pushDir == FaceType::DO_NOT_CULL || pushDir == FaceType::UNKNOWN) {
                int axis = -1;
                bool positive = true;
                if (!FaceGeometryAxis(blockModel, face, axis, positive)) continue;
                pushDir = AxisToFaceType(axis, positive);
            }
            auto neighborIt = neighborIndexMap.find(pushDir);
            if (neighborIt == neighborIndexMap.end()) continue;
            int nx = x, ny = y, nz = z;
            switch (pushDir) {
            case FaceType::DOWN:  ny--; break;
            case FaceType::UP:    ny++; break;
            case FaceType::NORTH: nz--; break;
            case FaceType::SOUTH: nz++; break;
            case FaceType::WEST:  nx--; break;
            case FaceType::EAST:  nx++; break;
            default: break;
            }
            const BlockOcclusion& neighborOcc = GetBlockOcclusion(GetBlockId(nx, ny, nz));
            // 邻居必须有模型，且在该方向确实有一张贴向本块的完整实心面(即对面方向的那一面)，
            // 才存在共面冲突
            if (!neighborOcc.hasModel || !neighborOcc.selfFaceFull[OppositeFaceDir(pushDir)]) continue;

            float nxv = 0.0f, nyv = 0.0f, nzv = 0.0f;
            if (!FaceDirectionNormal(pushDir, nxv, nyv, nzv)) continue;
            // 复制一份顶点再外推: 贴片其余面(如正面/侧面)共用原顶点，不能跟着一起动
            for (int k = 0; k < 4; ++k) {
                const int index = face.vertexIndices[k];
                if (index < 0 || static_cast<size_t>(index) * 3 + 2 >= blockModel.vertices.size()) continue;
                const size_t base = static_cast<size_t>(index) * 3;
                const float px = blockModel.vertices[base];
                const float py = blockModel.vertices[base + 1];
                const float pz = blockModel.vertices[base + 2];
                const int newIndex = static_cast<int>(blockModel.vertices.size() / 3);
                blockModel.vertices.push_back(px + nxv * pushStep);
                blockModel.vertices.push_back(py + nyv * pushStep);
                blockModel.vertices.push_back(pz + nzv * pushStep);
                face.vertexIndices[k] = newIndex;
            }
        }
    }

    // Create bogey frame, wheels, shafts and drive (wheelAngle 0, DO_NOT_CULL).
    // Appended after the cull walk, so the new face indices must be recorded.
    if (ns == "create") {
        const size_t bogeyMeshBegin = blockModel.faces.size();
        AppendCreateBogey("create:" + blockName, blockEntityNbt, blockModel);
        for (size_t bogeyFace = bogeyMeshBegin; bogeyFace < blockModel.faces.size(); ++bogeyFace)
            validFaceIndices.push_back(static_cast<int>(bogeyFace));
    }

    // ---- 共面叠加层(草侧这类)解析: 底层存活时删除叠加面, 并把
    // base 材质 -> 叠加层(全名/贴图/tint) 登记给 overlay.json,
    // 由 Blender 侧材质节点实现叠加; 底层被剔除时保留叠加面作几何回退。
    if (!blockModel.overlayPairs.empty()) {
        std::unordered_set<int> validFaceSet(validFaceIndices.begin(), validFaceIndices.end());
        std::unordered_set<int> overlayFacesToDrop;
        std::unordered_map<std::string, std::vector<OverlayLayerInfo>> blockOverlays;
        const int faceCount = static_cast<int>(blockModel.faces.size());
        const int materialCount = static_cast<int>(blockModel.materials.size());

        for (const auto& pair : blockModel.overlayPairs) {
            if (pair.baseFace < 0 || pair.overlayFace < 0 ||
                pair.baseFace >= faceCount || pair.overlayFace >= faceCount) {
                continue;
            }
            const bool baseValid = validFaceSet.count(pair.baseFace) != 0;
            const bool overlayValid = validFaceSet.count(pair.overlayFace) != 0;
            if (!baseValid || !overlayValid) continue; // 底层不在时叠加面保留为几何

            overlayFacesToDrop.insert(pair.overlayFace);
            const int baseMatIdx = blockModel.faces[pair.baseFace].materialIndex;
            const int overlayMatIdx = blockModel.faces[pair.overlayFace].materialIndex;
            if (baseMatIdx < 0 || baseMatIdx >= materialCount ||
                overlayMatIdx < 0 || overlayMatIdx >= materialCount) {
                continue;
            }
            const Material& baseMat = blockModel.materials[baseMatIdx];
            const Material& overlayMat = blockModel.materials[overlayMatIdx];
            if (baseMat.name == overlayMat.name) continue;
            blockOverlays[baseMat.name].push_back(
                { overlayMat.name, overlayMat.texturePath, overlayMat.tint });
        }

        if (!overlayFacesToDrop.empty()) {
            std::vector<int> remaining;
            remaining.reserve(validFaceIndices.size());
            for (int faceIdx : validFaceIndices) {
                if (!overlayFacesToDrop.count(faceIdx)) remaining.push_back(faceIdx);
            }
            validFaceIndices.swap(remaining);
        }
        for (const auto& entry : blockOverlays) {
            RegisterOverlaySequence(entry.first, entry.second);
        }
        blockModel.overlayPairs.clear();
    }

    // Bezier track curves are baked from the track block entity and extend
    // outside this block. Append them after neighbor culling and keep every
    // new face: they are DO_NOT_CULL and must survive the compact below.
    if (ns == "create") {
        const size_t curveFacesBegin = blockModel.faces.size();
        AppendCreateCurvedTrack("create:" + blockName, blockEntityNbt, blockModel);
        for (size_t faceIndex = curveFacesBegin; faceIndex < blockModel.faces.size(); ++faceIndex)
            validFaceIndices.push_back(static_cast<int>(faceIndex));
    }

    // Hidden blocks contribute no geometry. Previously their unused vertices,
    // UVs and materials still reached the final mesh and global sort.
    if (validFaceIndices.empty()) {
        appendSignText();
        return;
    }
    // Indices are ascending, so compact faces in place without copying the
    // already-owned vertex/UV/material arrays a second time.
    for (size_t i = 0; i < validFaceIndices.size(); ++i) {
        blockModel.faces[i] = blockModel.faces[validFaceIndices[i]];
    }
    blockModel.faces.resize(validFaceIndices.size());

    // Beacon beam is extra geometry on the finished block model. Faces are
    // already culled and compacted; offset/merge below moves the beam with
    // the beacon. specialHandled stays unset.
    if (ns == "minecraft") {
        std::string beaconName = blockName;
        const size_t beaconBracket = beaconName.find('[');
        if (beaconBracket != std::string::npos) beaconName.resize(beaconBracket);
        if (beaconName == "beacon") {
            AppendBeaconBeam(blockModel, blockEntityNbt, x, y, z);
        }
    }
    // Campfire / soul_campfire cooking items (CampfireRenderer).
    if (blockEntityNbt && ns == "minecraft")
        AppendCampfireItems(blockModel, blockEntityNbt, ns + ":" + blockName, x, y, z);

    ApplyPositionOffset(blockModel, x, y, z);

    // 合并到主模型
    if (chunkModel.vertices.empty()) {
        chunkModel = blockModel;
    }
    else {
        MergeModelsDirectly(chunkModel, blockModel, materialLookup);
    }
    appendSignText();
}

ModelData ChunkGenerator::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    // Missing/air-only sections used to visit every voxel and repeatedly probe
    // seven section hashes. Caches are frozen while model workers are running.
    auto section = sectionCache.find({chunkX, chunkZ, AdjustSectionY(sectionY)});
    if (section == sectionCache.end() || std::all_of(section->second.blockData.begin(),
        section->second.blockData.end(), [](int id) { return id == 0; })) {
        bool hasEntity = false;
        std::shared_lock<std::shared_mutex> lock(entityBlockCacheMutex);
        auto entities = EntityBlockCache.find({chunkX, chunkZ});
        if (entities != EntityBlockCache.end()) {
            for (const auto& entity : entities->second)
                if (entity && (entity->y >> 4) == sectionY) { hasEntity = true; break; }
        }
        if (!hasEntity) return ModelData{};
    }
    // 本区块的 DataVersion 决定加权变体走 1.21.5+ 还是旧算法。
    ChunkVariantVersionScope variantVersion(chunkX, chunkZ);
    // Reuse the material index across thousands of block appends.
    ModelData chunkModel;
    std::unordered_map<std::string, int> materialLookup;
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;
    
    // 计算区块内的方块范围
    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;
   
    
    // 遍历区块内的每个方块
    for (int x = blockXStart; x < blockXStart + 16; ++x) {
        for (int z = blockZStart; z < blockZStart + 16; ++z) {
            for (int y = blockYStart; y < blockYStart + 16; ++y) {
                
                // 检查当前方块是否在导出区域内
                if (x < xStart || x > xEnd || y < yStart || y > yEnd || z < zStart || z > zEnd) {
                    continue; // 跳过不在导出区域内的方块
                }
                ProcessBlockForModel(chunkModel, x, y, z, &materialLookup);
            }
        }
    }

    
    // 方块实体只在其基准坐标所属的 Section 中生成，并严格遵守导出范围。
    // 旧逻辑会由当前区块第一个完成的 Section 导出全部实体，导致范围外的
    // 模组方块泄漏到结果中，同时让导出结果受线程调度影响。
    auto chunkKey = std::make_pair(chunkX, chunkZ);
    // 复制 shared_ptr 列表后再生成模型，避免持锁执行昂贵操作；即使未来恢复
    // 异步加载/卸载，也不会与 EntityBlockCache 的写入发生竞态。
    std::vector<std::shared_ptr<EntityBlock>> entityBlocks;
    {
        std::shared_lock<std::shared_mutex> entityLock(entityBlockCacheMutex);
        auto entityIt = EntityBlockCache.find(chunkKey);
        if (entityIt != EntityBlockCache.end()) entityBlocks = entityIt->second;
    }
    if (!entityBlocks.empty()) {
        for (const auto& entity : entityBlocks) {
            if (entity == nullptr) continue;
            if (entity->x < xStart || entity->x > xEnd ||
                entity->y < yStart || entity->y > yEnd ||
                entity->z < zStart || entity->z > zEnd) {
                continue;
            }

            int entitySectionY;
            blockYToSectionY(entity->y, entitySectionY);
            if (entitySectionY != sectionY) continue;

            ModelData entityModel = entity->GenerateModel();
            if (entityModel.vertices.empty()) continue;
            if (chunkModel.vertices.empty()) {
                chunkModel = std::move(entityModel);
            }
            else {
                MergeModelsDirectly(chunkModel, entityModel);
            }
        }
    }

    return chunkModel;
}

ModelData ChunkGenerator::GenerateLODChunkModel(int chunkX, int sectionY, int chunkZ, float lodSize) {
    ChunkVariantVersionScope variantVersion(chunkX, chunkZ);
    ModelData chunkModel;
    std::unordered_map<std::string, int> materialLookup;
    int xStart = config.minX;
    int xEnd = config.maxX;
    int yStart = config.minY;
    int yEnd = config.maxY;
    int zStart = config.minZ;
    int zEnd = config.maxZ;

    int blockXStart = chunkX * 16;
    int blockZStart = chunkZ * 16;
    int blockYStart = sectionY * 16;

    int lodBlockSize = static_cast<int>(lodSize);

    for (int x = blockXStart; x < blockXStart + 16; x += lodBlockSize) {
        for (int z = blockZStart; z < blockZStart + 16; z += lodBlockSize) {
            for (int y = blockYStart; y < blockYStart + 16; y += lodBlockSize) {
                // 边界检查
                if (x < xStart || x + lodBlockSize - 1 > xEnd ||
                    z < zStart || z + lodBlockSize - 1 > zEnd ||
                    y < yStart || y + lodBlockSize - 1 > yEnd)
                    continue;

                if (config.cullCave) {
                    if (GetSkyLight(x, y, z) == -1)
                        continue;
                }
                int id = -1;
                int level=0;
                BlockType type = LODManager::DetermineLODBlockTypeWithUpperCheck(x, y, z, lodBlockSize, &id, &level);
                
                // 检查是否应该使用原始模型
                if (id != -1) {
                    Block currentBlock = GetBlockById(id);
                    std::string blockName = currentBlock.GetModifiedNameWithNamespace();

                    if (lodBlockSize == 1 && currentBlock.HasFluid() && !currentBlock.IsPureFluid()) {
                        ProcessBlockForModel(chunkModel, x, y, z, &materialLookup);
                        continue;
                    }
                    
                    // 仅在LOD级别为1时启用原始模型功能
                    if (lodBlockSize == 1 && LODManager::ShouldUseOriginalModel(blockName)) {
                        ProcessBlockForModel(chunkModel, x, y, z, &materialLookup);
                        continue; // 跳过LOD方块生成
                    }
                }
                
                if (type != SOLID && type != FLUID) continue;
                std::vector<std::string> color = LODManager::GetBlockColor(x, y, z, id, type);
                level = (lodBlockSize - (level));
                ModelData solidBox = LODManager::GenerateBox(x, y, z, lodBlockSize, level, color);
                MergeModelsDirectly(chunkModel, solidBox, &materialLookup);
            }
        }
    }
    return chunkModel;
}
