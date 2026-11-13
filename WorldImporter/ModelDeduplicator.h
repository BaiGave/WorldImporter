// ModelDeduplicator.h
#ifndef MODEL_DEDUPLICATOR_H
#define MODEL_DEDUPLICATOR_H

#include "model.h"
#include <unordered_map>

class ModelDeduplicator {
public:
    // 顶点去重方法
    static void DeduplicateVertices(ModelData& data);

    // 按贴图拆分顶点：相同坐标但贴图不同的面不再共用顶点，避免树叶飘动
    // 顶点组把静态几何一起带走。
    static void SplitVerticesByTexture(ModelData& data);

    // UV坐标去重方法
    static void DeduplicateUV(ModelData& model);

    // 面去重方法
    static void DeduplicateFaces(ModelData& data);

    //贪心网格算法
    static void GreedyMesh(ModelData& data);

    // 综合去重和优化方法
    static void DeduplicateModel(ModelData& data);

};

// 设置模型阶段的并发预算(外层模型线程数)。
// 去重/并行步骤会据此限制内部线程数,避免线程数量爆炸。
void SetModelThreadBudget(int threads);

#endif // MODEL_DEDUPLICATOR_H