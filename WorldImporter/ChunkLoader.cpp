// ChunkLoader.cpp
#include <mutex>
#include <thread>
#include <vector>
#include <shared_mutex>
#include "locutil.h"
#include "ChunkLoader.h"
#include "block.h"
#include "LODManager.h"
#include "RegionCache.h"

void ChunkLoader::LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd) {

    // 使用多线程加载所有相关的分块和分段
    std::vector<std::future<void>> futures;

    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            // 新增:如果 chunk 不存在于 region 文件中或为空，则跳过加载
            if (!HasChunk(chunkX, chunkZ)) {
                continue;
            }
            futures.push_back(std::async(std::launch::async, [&, chunkX, chunkZ]() {
                LoadAndCacheBlockData(chunkX, chunkZ);
                for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                    auto key = std::make_tuple(chunkX, sectionY, chunkZ);
                    // 确保条目存在（可能由RegionModelExporter预先创建以存储LOD）
                    // 如果不存在，则创建一个新的条目并设置加载状态
                    // LOD值在此处不设置，它由RegionModelExporter负责
                    {
                        std::unique_lock<std::shared_mutex> lock(g_chunkSectionInfoMapMutex);
                        g_chunkSectionInfoMap[key].isLoaded.store(true, std::memory_order_release);
                    }
                }
                }));
        }
    }

    // 等待所有线程完成
    for (auto& future : futures) {
        future.get();
    }
}

void ChunkLoader::UnloadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd,
    const std::unordered_set<std::pair<int, int>, pair_hash>& retain_expanded_chunks) {
    // 卸载指定范围的区块和分段
    std::vector<std::future<void>> futures;
    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            futures.push_back(std::async(std::launch::async, [=, &retain_expanded_chunks]() {
                // 如果区块在保留集合中，则跳过卸载
                if (retain_expanded_chunks.count({chunkX, chunkZ})) {
                    return;
                }

                // 清理 g_chunkSectionInfoMap (使用原始 sectionY)
                for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                    auto g_map_key = std::make_tuple(chunkX, sectionY, chunkZ);
                    {
                        std::unique_lock<std::shared_mutex> lock(g_chunkSectionInfoMapMutex);
                        g_chunkSectionInfoMap.erase(g_map_key);
                    }
                }

                // 清理 sectionCache 中与该 (chunkX, chunkZ) 相关的所有条目
                ClearSectionCacheForChunk(chunkX, chunkZ);

                // 卸载与区块相关的实体方块及高度图缓存
                // 确保这些操作在 sectionY 循环之外，并使用正确的互斥锁
                {
                    std::unique_lock<std::shared_mutex> lock(entityBlockCacheMutex); // 使用 entityBlockCacheMutex
                    EntityBlockCache.erase(std::make_pair(chunkX, chunkZ));
                }
                {
                    std::unique_lock<std::shared_mutex> lock(heightMapCacheMutex); // 使用 heightMapCacheMutex
                    heightMapCache.erase(std::make_pair(chunkX, chunkZ));
                }
            }));
        }
    }
    for (auto& f : futures) {
        f.get();
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
                    // 默认情况下，如果距离超过L3，我们使用一个更高的LOD值，例如8.0f
                    // 如果LOD3未激活，则尝试LOD2，然后是LOD1
                    if (config.activeLOD3) {
                        chunkLOD = 8.0f;
                    } else if (config.activeLOD2) {
                        chunkLOD = 2.0f; // 如果LOD3未激活但LOD2已激活，则回退到LOD2（或者根据需求选择一个合适的值，比如4.0f如果LOD3的替代值应该是比LOD2更高的）
                    } else {
                        chunkLOD = 1.0f; // 如果LOD3和LOD2都未激活，则回退到LOD1
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