// ChunkGenerator.cpp
#include "ChunkGenerator.h"
#include "RegionModelExporter.h"
#include "locutil.h"
#include "ObjExporter.h"
#include "include/stb_image.h"
#include "biome.h"
#include "model.h"
#include "blocktint.h"
#include "Fluid.h"
#include "LODManager.h"
#include "CTM.h"
#include "CreateCT.h"
#include "texture.h"
#include "Occlusion.h"
#include <iomanip>
#include <cmath>
#include <sstream>
#include <regex>
#include <tuple>
#include <future>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include "ModelDeduplicator.h"
#include "ChunkGroupAllocator.h"
#include <utility>
#include "hashutils.h"
#include "ChunkLoader.h"
#include "SpecialBlock.h"
#include "BbsModelSupport.h"
using namespace std;
using namespace std::chrono;

static const std::unordered_map<FaceType, int> neighborIndexMap = {
        {FaceType::DOWN, 1}, {FaceType::UP, 0}, {FaceType::NORTH, 4},
        {FaceType::SOUTH, 5}, {FaceType::WEST, 2}, {FaceType::EAST, 3}
};

static bool IsFullCubeModel(const ModelData& model) {
    constexpr float epsilon = 0.0001f;
    const std::array<FaceType, 6> directions = {
        FaceType::DOWN, FaceType::UP, FaceType::NORTH,
        FaceType::SOUTH, FaceType::WEST, FaceType::EAST
    };

    for (FaceType direction : directions) {
        bool foundBoundaryFace = false;
        for (const Face& face : model.faces) {
            if (face.faceDirection != direction) continue;

            float minA = 1.0f, maxA = 0.0f;
            float minB = 1.0f, maxB = 0.0f;
            bool onBoundary = true;
            for (int vertexIndex : face.vertexIndices) {
                size_t offset = static_cast<size_t>(vertexIndex) * 3;
                if (offset + 2 >= model.vertices.size()) {
                    onBoundary = false;
                    break;
                }

                float x = model.vertices[offset];
                float y = model.vertices[offset + 1];
                float z = model.vertices[offset + 2];
                float plane, a, b, expected;
                if (direction == FaceType::DOWN || direction == FaceType::UP) {
                    plane = y; a = x; b = z;
                    expected = direction == FaceType::DOWN ? 0.0f : 1.0f;
                }
                else if (direction == FaceType::NORTH || direction == FaceType::SOUTH) {
                    plane = z; a = x; b = y;
                    expected = direction == FaceType::NORTH ? 0.0f : 1.0f;
                }
                else {
                    plane = x; a = z; b = y;
                    expected = direction == FaceType::WEST ? 0.0f : 1.0f;
                }

                if (std::abs(plane - expected) > epsilon) {
                    onBoundary = false;
                    break;
                }
                minA = std::min(minA, a); maxA = std::max(maxA, a);
                minB = std::min(minB, b); maxB = std::max(maxB, b);
            }

            if (onBoundary && minA <= epsilon && maxA >= 1.0f - epsilon &&
                minB <= epsilon && maxB >= 1.0f - epsilon) {
                foundBoundaryFace = true;
                break;
            }
        }
        if (!foundBoundaryFace) return false;
    }
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

void ChunkGenerator::ProcessBlockForModel(ModelData& chunkModel, int x, int y, int z) {
    std::array<bool, 6> neighbors; // 邻居是否不遮挡（true = 该方向的面应渲染）

    int id = GetBlockIdWithNeighbors(x, y, z, neighbors.data());
    Block currentBlock = GetBlockById(id);
    string blockName = currentBlock.GetModifiedNameWithNamespace();
    if (blockName == "minecraft:air") return;

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

    string ns = currentBlock.GetNamespace();

    // 标准化方块名称(去掉命名空间,处理状态)
    size_t colonPos = blockName.find(':');
    if (colonPos != string::npos) {
        blockName = blockName.substr(colonPos + 1);
    }

    ModelData blockModel;
    ModelData liquidModel;
    NbtTagPtr blockEntityNbt = GetBlockEntityNbt(x, y, z);
    bool specialHandled = TryGenerateBbsModel(blockEntityNbt, x, y, z, blockModel);
    if (!specialHandled) {
        specialHandled = SpecialBlock::TryGenerateCreateBlockModel(
        currentBlock.GetModifiedNameWithNamespace(), x, y, z,
            blockEntityNbt, blockModel);
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

        ModelData waterloggedBlockModel = GetRandomModelFromCache(ns, blockName);
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
        blockModel = GetRandomModelFromCache(ns, blockName);
    }

    if (blockModel.vertices.empty()) return;

    // Preserve Yuushya's block-state geometry for partial blocks, while full
    // cubes still use their CTM rules like other blocks.
    // Create 的连接纹理是运行时 UV 重映射(自带 *_connected.png), 与 OptiFine CTM
    // 是两套系统, 单独走 CreateCT 路径。
    bool useCtm = false;
    if (!specialHandled && ns == "create") {
        useCtm = ApplyCreateCTToBlockModel(blockModel, ns, blockName, x, y, z);
    }
    if (!useCtm && !specialHandled && HasCtmRules() && ns != "create" &&
        (ns != "yuushya" || IsFullCubeModel(blockModel))) {
        ApplyCtmToBlockModel(blockModel, ns, blockName, x, y, z);
        useCtm = true;
    }

    // 解析 tint 并按需拆分材质（必须在 CTM 之后、面剔除之前）
    ApplyTintToBlockModel(blockModel, currentBlock.name);

    // 剔除被遮挡的面
    std::vector<int> validFaceIndices;
    validFaceIndices.reserve(blockModel.faces.size());

    // CTM 连接的同类方块之间应剔除内部面(玻璃等非固体方块不在 solids 表中,
    // 默认会被当作 air 而保留内部面,导致缝隙)
    std::string curBaseName = ns + ":" + blockName;
    size_t bracketPos = curBaseName.find('[');
    if (bracketPos != std::string::npos) {
        curBaseName = curBaseName.substr(0, bracketPos);
    }
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
    // 玻璃类方块（含模组命名如 stained_glass / framed_glass / *_pane）
    const bool selfGlassLike =
        curBaseName.find("glass") != std::string::npos ||
        curBaseName.find("pane") != std::string::npos;

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
                if ((useCtm || selfGlassLike) && isSameTransparentNeighbor(dir)) {
                    continue;
                }
            }
            validFaceIndices.push_back(faceIdx);
        }
    }

    // 重建面数据(使用新的Face结构体)
    ModelData filteredModel;
    filteredModel.faces.reserve(validFaceIndices.size());

    for (int faceIdx : validFaceIndices) {
        // 直接复制Face结构体
        filteredModel.faces.push_back(blockModel.faces[faceIdx]);
    }

    // 顶点和UV数据保持不变(后续合并时会去重)
    filteredModel.vertices = blockModel.vertices;
    filteredModel.uvCoordinates = blockModel.uvCoordinates;
    filteredModel.materials = blockModel.materials;

    // 使用过滤后的模型
    blockModel = std::move(filteredModel);

    ApplyPositionOffset(blockModel, x, y, z);

    // 合并到主模型
    if (chunkModel.vertices.empty()) {
        chunkModel = blockModel;
    }
    else {
        MergeModelsDirectly(chunkModel, blockModel);
    }
}

ModelData ChunkGenerator::GenerateChunkModel(int chunkX, int sectionY, int chunkZ) {
    // 从RegionModelExporter.cpp中复制GenerateChunkModel的实现
    ModelData chunkModel;
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
                ProcessBlockForModel(chunkModel, x, y, z);
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
    // 从RegionModelExporter.cpp中复制GenerateLODChunkModel的实现
    ModelData chunkModel;
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
                        ProcessBlockForModel(chunkModel, x, y, z);
                        continue;
                    }
                    
                    // 仅在LOD级别为1时启用原始模型功能
                    if (lodBlockSize == 1 && LODManager::ShouldUseOriginalModel(blockName)) {
                        ProcessBlockForModel(chunkModel, x, y, z);
                        continue; // 跳过LOD方块生成
                    }
                }
                
                std::vector<std::string> color = LODManager::GetBlockColor(x, y, z, id, type);
                level = (lodBlockSize - (level));
                // 如果块类型是固体
                if (type == SOLID) {
                    ModelData solidBox = LODManager::GenerateBox(x, y, z, lodBlockSize, level, color);
                    MergeModelsDirectly(chunkModel, solidBox);
                }
                if (type ==FLUID)
                {
                    ModelData solidBox = LODManager::GenerateBox(x, y, z, lodBlockSize, level, color);
                    MergeModelsDirectly(chunkModel, solidBox);
                }
            }
        }
    }
    return chunkModel;
}
