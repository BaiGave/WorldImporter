// LODManager.h
#ifndef LOD_MANAGER_H
#define LOD_MANAGER_H

#include "RegionModelExporter.h"
#include "block.h"
#include "model.h"

struct TupleHash {
    std::size_t operator()(const std::tuple<int, int, int>& t) const {
        auto h1 = std::hash<int>()(std::get<0>(t));
        auto h2 = std::hash<int>()(std::get<1>(t));
        auto h3 = std::hash<int>()(std::get<2>(t));
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};
extern std::unordered_map<std::tuple<int, int, int>, float, TupleHash> g_chunkLODs;
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