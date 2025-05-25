// LODManager.h
#ifndef LOD_MANAGER_H
#define LOD_MANAGER_H

#include "RegionModelExporter.h"
#include "block.h"
#include "model.h"
#include "hashutils.h"
#include <shared_mutex>

// 结构体：包含LOD等级和加载状态
struct ChunkSectionInfo {
    float lodLevel = 0.0f; // 默认为最高精度LOD
    std::atomic<bool> isLoaded{false};

    // 需要显式定义默认构造函数，因为std::atomic<bool>不可复制
    ChunkSectionInfo() : lodLevel(0.0f), isLoaded(false) {}
    // 如果需要，可以添加带参数的构造函数
    ChunkSectionInfo(float lod, bool loaded) : lodLevel(lod) {
        isLoaded.store(loaded, std::memory_order_relaxed);
    }
    // 显式定义复制构造函数
    ChunkSectionInfo(const ChunkSectionInfo& other) : lodLevel(other.lodLevel) {
        isLoaded.store(other.isLoaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    // 显式定义复制赋值运算符
    ChunkSectionInfo& operator=(const ChunkSectionInfo& other) {
        if (this != &other) {
            lodLevel = other.lodLevel;
            isLoaded.store(other.isLoaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }
    // 显式定义移动构造函数
    ChunkSectionInfo(ChunkSectionInfo&& other) noexcept : lodLevel(other.lodLevel), isLoaded(other.isLoaded.load(std::memory_order_relaxed)) {
        // no need to modify other.isLoaded as it's atomic and its state is moved
    }
    // 显式定义移动赋值运算符
    ChunkSectionInfo& operator=(ChunkSectionInfo&& other) noexcept {
        if (this != &other) {
            lodLevel = other.lodLevel;
            isLoaded.store(other.isLoaded.load(std::memory_order_relaxed), std::memory_order_relaxed);
            // no need to modify other.isLoaded
        }
        return *this;
    }
};

// 全局区块信息缓存 (LOD 和加载状态)
extern std::unordered_map<std::tuple<int, int, int>, ChunkSectionInfo, TupleHash> g_chunkSectionInfoMap;

// 线程安全:保护 g_chunkSectionInfoMap 的读写
extern std::shared_mutex g_chunkSectionInfoMapMutex;

enum BlockType {
    AIR,
    FLUID,
    SOLID
};
class LODManager {
public:
    // 获取指定块的 LOD 值
    static float GetChunkLODAtBlock(int x, int y, int z);

    // 带上方检查的 LOD 块类型确定
    static BlockType DetermineLODBlockTypeWithUpperCheck(int x, int y, int z, int lodBlockSize, int* id = nullptr, int* level = nullptr);

    // 获取块颜色
    static std::vector<std::string> GetBlockColor(int x, int y, int z, int id, BlockType blockType);


    // 生成包围盒模型并剔除不需要的面
    static ModelData GenerateBox(int x, int y, int z, int baseSize, float boxHeight, const std::vector<std::string>& colors);

};

#endif // LOD_MANAGER_H