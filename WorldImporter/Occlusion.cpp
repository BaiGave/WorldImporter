#include "Occlusion.h"
#include "block.h"
#include "blockstate.h"
#include "texture.h"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace {

std::vector<BlockOcclusion> g_occlusionTable;
std::mutex g_buildMutex;
const BlockOcclusion g_emptyOcclusion;

constexpr float kEps = 1e-4f;

// 面方向索引：与 FaceType 枚举一致 UP=0, DOWN=1, NORTH=2, SOUTH=3, WEST=4, EAST=5
enum : int {
    DIR_UP = 0, DIR_DOWN = 1, DIR_NORTH = 2, DIR_SOUTH = 3, DIR_WEST = 4, DIR_EAST = 5
};

// 把材质纹理路径 "textures/<ns>/<path>.png" 解析为 (ns, path)
bool ParseMaterialTexturePath(const std::string& texturePath, std::string& ns, std::string& path) {
    const std::string prefix = "textures/";
    if (texturePath.rfind(prefix, 0) != 0) return false;
    const std::string rest = texturePath.substr(prefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return false;
    ns = rest.substr(0, slash);
    path = rest.substr(slash + 1);
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".png") == 0) {
        path.resize(path.size() - 4);
    }
    return !ns.empty() && !path.empty();
}

// 单个模型的遮挡分析结果
struct ModelOcclusion {
    bool fullGeom[6] = { false, false, false, false, false, false };   // 忽略透明度的完整 1x1 界面
    bool fullOpaque[6] = { false, false, false, false, false, false }; // 材质全不透明的完整 1x1 界面
    bool boundary = false;                                             // 是否存在贴界面的面
    bool selfFull[6] = { false, false, false, false, false, false };   // 同平面面并集完整覆盖该方向 1x1
    bool bboxFull = false;                                             // 顶点包围盒是否铺满整格（体积判定用）
    bool bboxNearlyFull = false;                                       // 顶点包围盒三轴是否都 >= 12/16（实体判定用）
    bool hasPartialBoundary[6] = { false, false, false, false, false, false }; // 存在"不完整/不透明"的界面面
};

// 按 (方向, 平面) 分组的面覆盖位图（16x16），用于计算 selfFull
struct PlaneCoverage {
    int planeKey = 0;
    uint16_t rows[16] = { 0 };
};

ModelOcclusion AnalyzeModel(const ModelData& model) {
    ModelOcclusion out;

    // ---- 顶点包围盒：用于"体积铺满整格"的判定（等价于原版的完整方块形状，
    //      不要求模型把六个面都画出来，例如堆叠柱子会故意省略上/下面）----
    {
        const size_t vertexCount = model.vertices.size() / 3;
        float vmin[3] = { 1e30f, 1e30f, 1e30f };
        float vmax[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t i = 0; i < vertexCount; ++i) {
            for (int a = 0; a < 3; ++a) {
                const float v = model.vertices[i * 3 + a];
                vmin[a] = std::min(vmin[a], v);
                vmax[a] = std::max(vmax[a], v);
            }
        }
        out.bboxFull = (vertexCount > 0) &&
            vmin[0] <= kEps && vmax[0] >= 1.0f - kEps &&
            vmin[1] <= kEps && vmax[1] >= 1.0f - kEps &&
            vmin[2] <= kEps && vmax[2] >= 1.0f - kEps;
        // "接近铺满"：用于 solidLike（≈ blocksMotion）判定，覆盖旋转/内缩的实心模型
        // （如绕轴旋转的圆柱），阈值取 12/16。
        out.bboxNearlyFull = (vertexCount > 0) &&
            (vmax[0] - vmin[0]) >= 0.75f &&
            (vmax[1] - vmin[1]) >= 0.75f &&
            (vmax[2] - vmin[2]) >= 0.75f;
    }

    std::vector<PlaneCoverage> selfGroups[6];
    for (const Face& face : model.faces) {
        float p[4][3];
        bool valid = true;
        for (int j = 0; j < 4; ++j) {
            const int vi = face.vertexIndices[j];
            if (vi < 0 || static_cast<size_t>(vi) * 3 + 2 >= model.vertices.size()) {
                valid = false;
                break;
            }
            p[j][0] = model.vertices[vi * 3];
            p[j][1] = model.vertices[vi * 3 + 1];
            p[j][2] = model.vertices[vi * 3 + 2];
        }
        if (!valid) continue;

        // 面法线（用顶点绕向判断朝向）
        const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
        const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
        const float n[3] = {
            e1[1] * e2[2] - e1[2] * e2[1],
            e1[2] * e2[0] - e1[0] * e2[2],
            e1[0] * e2[1] - e1[1] * e2[0]
        };
        int axis = 0;
        for (int a = 1; a < 3; ++a) {
            if (std::fabs(n[a]) > std::fabs(n[axis])) axis = a;
        }
        if (std::fabs(n[axis]) < kEps) continue; // 退化面
        const bool positive = n[axis] > 0.0f;

        // 面必须垂直于主轴（轴对齐）
        const float plane = p[0][axis];
        bool planar = true;
        for (int j = 1; j < 4; ++j) {
            if (std::fabs(p[j][axis] - plane) > kEps) {
                planar = false;
                break;
            }
        }
        if (!planar) continue;

        int dir = -1;
        if (axis == 0) dir = positive ? DIR_EAST : DIR_WEST;
        else if (axis == 1) dir = positive ? DIR_UP : DIR_DOWN;
        else dir = positive ? DIR_SOUTH : DIR_NORTH;

        // 另外两轴上的范围
        float mn[2] = { 1e30f, 1e30f };
        float mx[2] = { -1e30f, -1e30f };
        int k = 0;
        for (int a = 0; a < 3; ++a) {
            if (a == axis) continue;
            for (int j = 0; j < 4; ++j) {
                mn[k] = std::min(mn[k], p[j][a]);
                mx[k] = std::max(mx[k], p[j][a]);
            }
            ++k;
        }

        // ---- 自身面并集（原版 FluidRenderer#isFaceOccludedBySelf）----
        // 同一 (方向, 平面) 上的面取并集，完整覆盖 1x1 即认为该方向被自身遮挡。
        {
            const int planeKey = static_cast<int>(std::lround(plane * 10000.0f));
            PlaneCoverage* group = nullptr;
            for (auto& g : selfGroups[dir]) {
                if (g.planeKey == planeKey) {
                    group = &g;
                    break;
                }
            }
            if (group == nullptr) {
                selfGroups[dir].push_back(PlaneCoverage{});
                group = &selfGroups[dir].back();
                group->planeKey = planeKey;
            }
            const int colStart = std::max(0, static_cast<int>(std::ceil(mn[0] * 16.0f - kEps)));
            const int colEnd = std::min(15, static_cast<int>(std::floor(mx[0] * 16.0f + kEps)) - 1);
            const int rowStart = std::max(0, static_cast<int>(std::ceil(mn[1] * 16.0f - kEps)));
            const int rowEnd = std::min(15, static_cast<int>(std::floor(mx[1] * 16.0f + kEps)) - 1);
            for (int r = rowStart; r <= rowEnd; ++r) {
                for (int c = colStart; c <= colEnd; ++c) {
                    group->rows[r] |= static_cast<uint16_t>(1u << c);
                }
            }
        }

        // 必须位于方块界面且朝外（occludes/solidLike 用）
        const float boundaryCoord = positive ? 1.0f : 0.0f;
        if (std::fabs(plane - boundaryCoord) > kEps) continue;
        out.boundary = true;

        // 是否完整覆盖 1x1
        const bool full =
            mn[0] <= kEps && mx[0] >= 1.0f - kEps && mn[1] <= kEps && mx[1] >= 1.0f - kEps;
        if (full) {
            out.fullGeom[dir] = true;
        }

        // 材质是否全不透明
        bool opaque = false;
        if (face.materialIndex >= 0 &&
            static_cast<size_t>(face.materialIndex) < model.materials.size()) {
            std::string ns, path;
            if (ParseMaterialTexturePath(model.materials[face.materialIndex].texturePath, ns, path)) {
                opaque = IsTextureFullyOpaque(ns, path);
            }
        }
        if (full && opaque) {
            out.fullOpaque[dir] = true;
        }
        else {
            // 该方向存在"不完整或透明"的界面面：放宽判定时不允许视为整方块
            out.hasPartialBoundary[dir] = true;
        }
    }

    // 汇总自身面并集：任一平面被完整覆盖即算该方向被自身遮挡
    for (int d = 0; d < 6; ++d) {
        for (const auto& g : selfGroups[d]) {
            bool full = true;
            for (int r = 0; r < 16; ++r) {
                if (g.rows[r] != 0xFFFF) {
                    full = false;
                    break;
                }
            }
            if (full) {
                out.selfFull[d] = true;
                break;
            }
        }
    }
    return out;
}

} // namespace

const BlockOcclusion& GetBlockOcclusion(int blockId) {
    if (blockId < 0 || static_cast<size_t>(blockId) >= g_occlusionTable.size()) {
        return g_emptyOcclusion;
    }
    return g_occlusionTable[blockId];
}

void BuildBlockOcclusionTable(size_t fromIndex) {
    std::lock_guard<std::mutex> lock(g_buildMutex);
    const size_t total = globalBlockPalette.size();
    if (fromIndex >= total) return;
    g_occlusionTable.resize(total);

    for (size_t i = fromIndex; i < total; ++i) {
        const Block& block = globalBlockPalette[i];
        BlockOcclusion occ;
        if (block.air) {
            g_occlusionTable[i] = occ; // 空气：不遮挡、非实体
            continue;
        }

        const std::string ns = block.GetNamespace();
        const std::string blockId = block.GetModifiedName();
        const std::vector<ModelData> models = GetAllModelsFromCache(ns, blockId);
        if (models.empty()) {
            g_occlusionTable[i] = occ; // 无模型：保守视为不遮挡
            continue;
        }

        occ.hasModel = true;
        bool occludesAll = true;
        bool solidAny = false;
        bool selfFullAll[6] = { true, true, true, true, true, true };
        for (const ModelData& m : models) {
            if (m.vertices.empty() || m.faces.empty()) {
                occludesAll = false;
                for (int d = 0; d < 6; ++d) selfFullAll[d] = false;
                continue;
            }
            const ModelOcclusion mo = AnalyzeModel(m);
            bool fullOpaqueAll = true;
            bool fullGeomAll = true;
            int fullOpaqueCount = 0;
            bool anyPartialBoundary = false;
            for (int d = 0; d < 6; ++d) {
                if (!mo.fullOpaque[d]) fullOpaqueAll = false;
                if (!mo.fullGeom[d]) fullGeomAll = false;
                if (mo.fullOpaque[d]) fullOpaqueCount++;
                if (mo.hasPartialBoundary[d]) anyPartialBoundary = true;
                selfFullAll[d] = selfFullAll[d] && mo.selfFull[d];
            }
            // 放宽判定：模型体积铺满整格 + ≥4 个方向有完整不透明面 +
            // 所有已存在的边界面都完整且不透明 → 视为整方块遮挡体。
            // 用于"堆叠柱子/竖块故意省略上下面"的模型；视觉上确实铺满整格才生效，
            // 不会把视觉上小于整格的装饰品误判成遮挡体。
            const bool relaxedFullCube = mo.bboxFull && fullOpaqueCount >= 4 && !anyPartialBoundary;
            occludesAll = occludesAll && (fullOpaqueAll || relaxedFullCube);
            // solidLike（≈ 原版 blocksMotion / isSolid，用于水面高度平均）：
            // 完整立方体 / 有贴界面的面 / 接近铺满整格且面数足够的实心模型
            // （最后一条覆盖旋转、内缩的模型，如 marble_column；否则水面会在它旁边
            //   按"非实体"下凹，叠出每层约 0.09 的横向缝隙）。
            const bool volumeLikeSolid = mo.bboxNearlyFull && m.faces.size() >= 6;
            solidAny = solidAny || fullGeomAll || mo.boundary || volumeLikeSolid;
        }
        occ.occludes = occludesAll;
        occ.solidLike = solidAny;
        for (int d = 0; d < 6; ++d) occ.selfFaceFull[d] = selfFullAll[d];
        g_occlusionTable[i] = occ;
    }
}
