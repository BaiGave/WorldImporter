// ChunkLoader.cpp
#include <mutex>
#include <thread>
#include <vector>
#include "ChunkLoader.h"
#include "block.h"
#include "LODManager.h"

void ChunkLoader::LoadChunks(int chunkXStart, int chunkXEnd, int chunkZStart, int chunkZEnd,
    int sectionYStart, int sectionYEnd) {

    // 使用多线程加载所有相关的分块和分段
    std::vector<std::future<void>> futures;

    for (int chunkX = chunkXStart; chunkX <= chunkXEnd; ++chunkX) {
        for (int chunkZ = chunkZStart; chunkZ <= chunkZEnd; ++chunkZ) {
            futures.push_back(std::async(std::launch::async, [&, chunkX, chunkZ]() {
                LoadAndCacheBlockData(chunkX, chunkZ);
                for (int sectionY = sectionYStart; sectionY <= sectionYEnd; ++sectionY) {
                    auto key = std::make_tuple(chunkX, sectionY, chunkZ);
                    // 确保条目存在（可能由RegionModelExporter预先创建以存储LOD）
                    // 如果不存在，则创建一个新的条目并设置加载状态
                    // LOD值在此处不设置，它由RegionModelExporter负责
                    g_chunkSectionInfoMap[key].isLoaded.store(true, std::memory_order_release);
                }
                }));
        }
    }

    // 等待所有线程完成
    for (auto& future : futures) {
        future.get();
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
                    chunkLOD = 2.0f;
                } else if (dist2 <= L3d2) {
                    chunkLOD = 4.0f;
                } else {
                    chunkLOD = 8.0f;
                }
            }
            for (int sy = sectionYStart; sy <= sectionYEnd; ++sy) {
                // isLoaded 状态将由 ChunkLoader::LoadChunks 设置
                g_chunkSectionInfoMap[std::make_tuple(cx, sy, cz)].lodLevel = chunkLOD;
            }
        }
    }
}