// ChunkLoader.cpp
#include <mutex>
#include <thread>
#include <vector>
#include <shared_mutex>
#include "locutil.h"
#include "ChunkLoader.h"
#include "block.h"
#include "blockstate.h"
#include "LODManager.h"
#include "RegionCache.h"

void ChunkLoader::LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd) {

    // Only newly appended palette entries need blockstate processing this batch.
    const size_t paletteStart = globalBlockPalette.size();
    // 区块加载会注册全局方块/模型缓存，并且模型解析会查询 sectionCache。
    // 原先每个区块使用 std::async，会造成 sectionCache 与模型缓存的锁顺序死锁。
    // 保持模型导出阶段并行，加载阶段串行以保证缓存一致性。
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            if (!HasChunk(chunkX, chunkZ)) {
                continue;
            }
            LoadAndCacheBlockData(chunkX, chunkZ);
            for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                auto key = std::make_tuple(chunkX, sectionY, chunkZ);
                std::unique_lock<std::shared_mutex> lock(g_chunkSectionInfoMapMutex);
                g_chunkSectionInfoMap[key].isLoaded.store(true, std::memory_order_release);
            }
        }
    }

    // sectionCache 已完整可读后，再统一解析新方块模型。
    // 不在 LoadAndCacheBlockData 的写锁内调用，避免模型/CTM 查询 sectionCache 时自锁。
    std::vector<Block> blocksToProcess;
    {
        std::lock_guard<std::mutex> lock(globalPaletteMutex);
        blocksToProcess.assign(globalBlockPalette.begin() + paletteStart, globalBlockPalette.end());
    }
    if (!blocksToProcess.empty()) {
        ProcessBlockstateForBlocks(blocksToProcess);
    }
}

void ChunkLoader::UnloadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd,
    const std::unordered_set<std::pair<int, int>, pair_hash>& retain_expanded_chunks) {
    // Model workers have joined. One pass per map replaces one short-lived
    // thread per chunk and O(chunks * cachedSections) scans under a shared lock.
    auto removeChunk = [&](int x, int z) {
        return x >= chunkXStart && x <= chunkXEnd &&
            z >= chunkZStart && z <= chunkZEnd &&
            !retain_expanded_chunks.count({x, z});
    };
    {
        std::unique_lock<std::shared_mutex> lock(g_chunkSectionInfoMapMutex);
        for (auto it = g_chunkSectionInfoMap.begin(); it != g_chunkSectionInfoMap.end();) {
            const auto& [x, y, z] = it->first;
            if (removeChunk(x, z) && y >= sectionYStart && y <= sectionYEnd)
                it = g_chunkSectionInfoMap.erase(it);
            else ++it;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(sectionCacheMutex);
        for (auto it = sectionCache.begin(); it != sectionCache.end();) {
            if (removeChunk(std::get<0>(it->first), std::get<1>(it->first)))
                it = sectionCache.erase(it);
            else ++it;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(entityBlockCacheMutex);
        for (auto it = EntityBlockCache.begin(); it != EntityBlockCache.end();) {
            if (removeChunk(it->first.first, it->first.second)) it = EntityBlockCache.erase(it);
            else ++it;
        }
    }
    {
        std::unique_lock<std::shared_mutex> lock(heightMapCacheMutex);
        for (auto it = heightMapCache.begin(); it != heightMapCache.end();) {
            if (removeChunk(it->first.first, it->first.second)) it = heightMapCache.erase(it);
            else ++it;
        }
    }
}

void ChunkLoader::CalculateChunkLODs(int expandedChunkXStart, int expandedChunkXEnd, int expandedChunkZStart, int expandedChunkZEnd,
    int sectionYStart, int sectionYEnd) {
    // 计算LOD范围
    const int L0 = config.LOD0renderDistance;
    const int L1 = L0 + config.LOD1renderDistance;
    const int L2 = L1 + config.LOD2renderDistance;
    const int L3 = L2 + config.LOD3renderDistance;

    int L0d2 = L0 * L0;
    int L1d2 = L1 * L1;
    int L2d2 = L2 * L2;
    int L3d2 = L3 * L3;

    // 预先计算所有区块的LOD等级
    {
        size_t effectiveXCount = (expandedChunkXEnd - expandedChunkXStart + 1);
        size_t effectiveZCount = (expandedChunkZEnd - expandedChunkZStart + 1);
        size_t secCount = sectionYEnd - sectionYStart + 1;
        g_chunkSectionInfoMap.reserve(effectiveXCount * effectiveZCount * secCount);
    }

    for (int cx = expandedChunkXStart; cx <= expandedChunkXEnd; ++cx) {
        for (int cz = expandedChunkZStart; cz <= expandedChunkZEnd; ++cz) {
            int dx = cx - config.LODCenterX;
            int dz = cz - config.LODCenterZ;
            int dist2 = dx * dx + dz * dz;
            float chunkLOD = 0.0f;
            if (config.activeLOD) {
                if (dist2 <= L0d2) {
                    chunkLOD = 0.0f;
                } else if (dist2 <= L1d2) {
                    chunkLOD = 1.0f;
                } else if (dist2 <= L2d2) {
                    if (config.activeLOD2) {
                        chunkLOD = 2.0f;
                    } else {
                        chunkLOD = 1.0f; // 如果LOD2未激活，则回退到LOD1
                    }
                } else if (dist2 <= L3d2) {
                    if (config.activeLOD3) {
                        chunkLOD = 4.0f;
                    } else if (config.activeLOD2) {
                        chunkLOD = 2.0f; // 如果LOD3未激活但LOD2已激活，则回退到LOD2
                    } else {
                        chunkLOD = 1.0f; // 如果LOD3和LOD2都未激活，则回退到LOD1
                    }
                } else {
                    // 对于超出L3范围的区块，应用LOD4或回退
                    if (config.activeLOD4) {
                        chunkLOD = 8.0f;
                    } else if (config.activeLOD3) {
                        chunkLOD = 4.0f; // 如果LOD4未激活但LOD3已激活，则回退到LOD3
                    } else if (config.activeLOD2) {
                        chunkLOD = 2.0f; // 如果LOD4和LOD3都未激活但LOD2已激活，则回退到LOD2
                    } else {
                        chunkLOD = 1.0f; // 如果LOD4, LOD3和LOD2都未激活，则回退到LOD1
                    }
                }
            }
            for (int sy = sectionYStart; sy <= sectionYEnd; ++sy) {
                // isLoaded 状态将由 ChunkLoader::LoadChunks 设置
                {
                    std::unique_lock<std::shared_mutex> lock(g_chunkSectionInfoMapMutex);
                    g_chunkSectionInfoMap[std::make_tuple(cx, sy, cz)].lodLevel = chunkLOD;
                }
            }
        }
    }
}