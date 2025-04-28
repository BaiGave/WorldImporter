// ModelDeduplicator.h
#ifndef MODEL_DEDUPLICATOR_H
#define MODEL_DEDUPLICATOR_H

#include "model.h"
#include <unordered_map>



class ModelDeduplicator {
public:
    // 顶点去重方法
    static void DeduplicateVertices(ModelData& data);

    // UV坐标去重方法
    static void DeduplicateUV(ModelData& model);

    // 面去重方法
    static void DeduplicateFaces(ModelData& data);

    //贪心网格算法
    static void GreedyMesh(ModelData& data);

};

#endif // MODEL_DEDUPLICATOR_H