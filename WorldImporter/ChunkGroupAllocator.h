// ChunkGroupAllocator.h
#pragma once

#include <vector>
#include "config.h" 

struct ChunkTask {
    int chunkX;
    int sectionY;
    int chunkZ;
    float lodLevel; 
};

struct ChunkGroup {
    int startX;
    int startZ;
    std::vector<ChunkTask> tasks;
};

// 新增:批处理结构体,用于按批次管理多个区块组
struct ChunkBatch {
    // 当前批次所覆盖的区块范围(闭区间)
    int chunkXStart;
    int chunkXEnd;
    int chunkZStart;
    int chunkZEnd;

    // 包含的区块组集合
    std::vector<ChunkGroup> groups;
};

namespace ChunkGroupAllocator {

    extern std::vector<ChunkGroup> g_chunkGroups;

    // 新增:全局批处理容器
    extern std::vector<ChunkBatch> g_chunkBatches;

    void GenerateChunkGroups(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd
    );

    /*
     * 根据最大区块任务数(maxTasksPerBatch)自动将已生成的区块组拆分成多个批次。
     * 生成规则:
     *   1. 先调用 GenerateChunkGroups(...) 生成 g_chunkGroups;
     *   2. 按顺序累加区块组,当当前批次任务数量 + 组内任务数量 > maxTasksPerBatch 时开始新批次;
     *   3. 对每个批次统计其覆盖的 chunkX/chunkZ 边界,方便后续加载/卸载操作;
     */
    void GenerateChunkBatches(
        int chunkXStart, int chunkXEnd,
        int chunkZStart, int chunkZEnd,
        int sectionYStart, int sectionYEnd,
        size_t maxTasksPerBatch = 4096  // 默认值，实际使用时会被config.maxTasksPerBatch替代
    );

}