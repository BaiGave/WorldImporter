// ModelDeduplicator.cpp
#include "ModelDeduplicator.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <climits>
#include <sstream>
#include <queue>
#include <stack>
#include <string>
#include <set>
#include <unordered_set>
#include "TaskMonitor.h"
#include <future> // 新增: 用于 std::async, std::future
#include <mutex>  // 新增: 用于 std::mutex, std::lock_guard
#include <vector> 
#include <utility> // 新增: 用于 std::pair
#include <iostream> // 新增: 用于错误输出
#include <iterator> // 新增: 用于 std::make_move_iterator
#include <atomic> // 新增: 用于 std::atomic_bool
#include <thread> // 新增: 用于 std::thread::hardware_concurrency()
#include <chrono> // 新增: 用于性能计时
#include <functional> // 新增: 用于 std::function
#undef max
#undef min
// 顶点量化:统一使用四舍五入(std::round)。
// 历史实现中不同阶段分别采用 std::round 与 int(v*10000+0.5f)，
// 负数舍入结果不一致，会导致贪心合面反查顶点键时命中失败。
inline VertexKey MakeVertexKey(float x, float y, float z) {
    return VertexKey{
        static_cast<int>(std::round(x * 10000.0f)),
        static_cast<int>(std::round(y * 10000.0f)),
        static_cast<int>(std::round(z * 10000.0f))
    };
}

/* 并发预算与去重线程数 */
namespace {
    std::atomic<int> g_modelThreadBudget{1};
}

void SetModelThreadBudget(int threads) {
    g_modelThreadBudget.store(threads > 1 ? threads : 1, std::memory_order_relaxed);
}

// 去重内部的并行度:外层多线程时按预算分摊核数,避免线程数量爆炸。
static unsigned int DedupThreadCount() {
    unsigned int hc = std::thread::hardware_concurrency();
    if (hc == 0) hc = 1;
    int budget = g_modelThreadBudget.load(std::memory_order_relaxed);
    // 外层已有多个长期存活的模型线程时，内部去重保持串行。
    // 旧逻辑按 hc/budget 继续反复创建短命线程，4×4 线程频繁退出会在
    // MinGW/UCRT TLS 清理路径触发 0xC0000374，且线程创建开销抵消收益。
    if (budget > 1) return 1;
    return hc;
}

// 2x2矩阵结构体,用于UV坐标变换
struct Matrix2x2 {
    float m[2][2];

    Matrix2x2() {
        m[0][0] = 1.0f; m[0][1] = 0.0f;
        m[1][0] = 0.0f; m[1][1] = 1.0f;
    }

    Matrix2x2(float a, float b, float c, float d) {
        m[0][0] = a; m[0][1] = b;
        m[1][0] = c; m[1][1] = d;
    }

    // 单位矩阵
    static Matrix2x2 identity() {
        return Matrix2x2(1.0f, 0.0f, 0.0f, 1.0f);
    }

    // 旋转矩阵(角度制)
    static Matrix2x2 rotation(float angleDegrees) {
        float angleRadians = angleDegrees * 3.14159f / 180.0f;
        float c = std::cos(angleRadians);
        float s = std::sin(angleRadians);
        return Matrix2x2(c, -s, s, c);
    }

    // 缩放矩阵
    static Matrix2x2 scaling(float sx, float sy) {
        return Matrix2x2(sx, 0.0f, 0.0f, sy);
    }

    // 水平镜像矩阵
    static Matrix2x2 mirrorX() {
        return Matrix2x2(-1.0f, 0.0f, 0.0f, 1.0f);
    }

    // 垂直镜像矩阵
    static Matrix2x2 mirrorY() {
        return Matrix2x2(1.0f, 0.0f, 0.0f, -1.0f);
    }

    // 矩阵乘法
    Matrix2x2 operator*(const Matrix2x2& other) const {
        Matrix2x2 result;
        for (int i = 0; i < 2; ++i) {
            for (int j = 0; j < 2; ++j) {
                result.m[i][j] = 0;
                for (int k = 0; k < 2; ++k) {
                    result.m[i][j] += m[i][k] * other.m[k][j];
                }
            }
        }
        return result;
    }
};


void ModelDeduplicator::DeduplicateVertices(ModelData& data) {
    const size_t vertCount = data.vertices.size() / 3;
    if (vertCount == 0) return;

    using Clock = std::chrono::high_resolution_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto t0 = Clock::now();

    // 使用简单的KeyAndIndex结构进行排序和去重
    struct KeyAndIndex { VertexKey key; int oldIndex; };
    std::vector<KeyAndIndex> keys(vertCount);

    // 并行计算顶点键
    unsigned int numThreads = DedupThreadCount();
    if (numThreads == 0) numThreads = 1;
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    size_t chunk = (vertCount + numThreads - 1) / numThreads;
    
    for (unsigned int t = 0; t < numThreads; ++t) {
        size_t start = t * chunk;
        size_t end = std::min(start + chunk, vertCount);
        threads.emplace_back([&, start, end]() {
            for (size_t i = start; i < end; ++i) {
                float x = data.vertices[3*i];
                float y = data.vertices[3*i + 1];
                float z = data.vertices[3*i + 2];
                
                // 确保精确的量化，使用统一的舍入方法
                keys[i] = { MakeVertexKey(x, y, z), static_cast<int>(i) };
            }
        });
    }
    for (auto &th : threads) th.join();

    auto t1 = Clock::now();
    std::cerr << "计算顶点键: " << Ms(t1 - t0).count() << " ms\n";

    // 使用稳定排序，相同键的情况下保持原顺序
    std::stable_sort(keys.begin(), keys.end(), [](const KeyAndIndex &a, const KeyAndIndex &b) {
        if (a.key.x != b.key.x) return a.key.x < b.key.x;
        if (a.key.y != b.key.y) return a.key.y < b.key.y;
        return a.key.z < b.key.z;
    });

    auto t2 = Clock::now();
    std::cerr << "排序顶点键: " << Ms(t2 - t1).count() << " ms\n";

    // 创建新的顶点数组和索引映射
    std::vector<int> indexMap(vertCount);
    std::vector<float> newVertices;
    newVertices.reserve(vertCount * 3 / 2); // 预估去重后的顶点数

    // 去重并构建新顶点数组
    int newIndex = 0;
    for (size_t i = 0; i < vertCount; ++i) {
        // 检查是否与前一个顶点相同
        bool isNewVertex = (i == 0) || 
                          (keys[i].key.x != keys[i-1].key.x || 
                           keys[i].key.y != keys[i-1].key.y || 
                           keys[i].key.z != keys[i-1].key.z);
        
        if (isNewVertex) {
            // 这是一个新的唯一顶点
            int oldIdx = keys[i].oldIndex;
            newVertices.push_back(data.vertices[3*oldIdx]);
            newVertices.push_back(data.vertices[3*oldIdx + 1]);
            newVertices.push_back(data.vertices[3*oldIdx + 2]);
            newIndex++;
        }
        
        // 更新索引映射
        indexMap[keys[i].oldIndex] = newIndex - 1;
    }

    // 替换原始顶点数组
    data.vertices = std::move(newVertices);

    auto t3 = Clock::now();
    std::cerr << "去重顶点: " << Ms(t3 - t2).count() << " ms\n";

    // 并行更新面索引
    const size_t faceCount = data.faces.size();
    if (faceCount > 0) {
        threads.clear();
        chunk = (faceCount + numThreads - 1) / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, faceCount);
            threads.emplace_back([&, start, end]() {
                for (size_t fi = start; fi < end; ++fi) {
                    for (int &idx : data.faces[fi].vertexIndices) {
                        idx = indexMap[idx];
                    }
                }
            });
        }
        for (auto &th : threads) th.join();
    }

    auto t4 = Clock::now();
    std::cerr << "更新面索引: " << Ms(t4 - t3).count() << " ms\n";
    std::cerr << "总去重时间: " << Ms(t4 - t0).count() << " ms\n";
    std::cerr << "原始顶点数: " << vertCount << ", 去重后顶点数: " << newIndex << ", 减少率: " 
              << (1.0 - static_cast<double>(newIndex) / vertCount) * 100.0 << "%\n";
}

void ModelDeduplicator::DeduplicateUV(ModelData& model) {
    // 如果没有 UV 坐标,则直接返回
    if (model.uvCoordinates.empty()) {
        return;
    }

    // 使用哈希表记录每个唯一 UV 对应的新索引
    std::unordered_map<UVKey, int> uvMap;
    std::vector<float> newUV;  // 存储去重后的 UV 坐标(每两个元素构成一组)
    // 原始 UV 数组中组的数量(每组有2个元素:u,v)
    int uvCount = model.uvCoordinates.size() / 2;
    // 建立一个映射表,从旧的 UV 索引到新的 UV 索引
    std::vector<int> indexMapping(uvCount, -1);

    for (int i = 0; i < uvCount; i++) {
        float u = model.uvCoordinates[i * 2];
        float v = model.uvCoordinates[i * 2 + 1];
        // 将浮点数转换为整数,保留小数点后6位的精度
        int iu = static_cast<int>(std::round(u * 1000000));
        int iv = static_cast<int>(std::round(v * 1000000));
        UVKey key = { iu, iv };

        auto it = uvMap.find(key);
        if (it == uvMap.end()) {
            // 如果没有找到,则是新 UV,记录新的索引
            int newIndex = newUV.size() / 2;
            uvMap[key] = newIndex;
            newUV.push_back(u);
            newUV.push_back(v);
            indexMapping[i] = newIndex;
        }
        else {
            // 如果已存在,则记录已有的新索引
            indexMapping[i] = it->second;
        }
    }

    // 如果 uvFaces 不为空,则更新 uvFaces 中的索引
    if (!model.faces.empty()) {
        for (auto& face : model.faces) {
            for (auto& idx : face.uvIndices) {
                // 注意:这里假设 uvIndices 中的索引都在有效范围内
                if (idx >=0 && idx < indexMapping.size()){ // 添加边界检查
                idx = indexMapping[idx];
                } else {
                    // 处理无效索引，例如设置为一个特定的错误值或保持不变并记录错误
                    // std::cerr << "Warning: Invalid UV index " << idx << " encountered." << std::endl;
                }
            }
        }
    }

    // 替换掉原有的 uvCoordinates
    model.uvCoordinates = std::move(newUV);
}

void ModelDeduplicator::DeduplicateFaces(ModelData& data) {
    size_t faceCountNum = data.faces.size();
    std::vector<FaceKey> keys;
    keys.reserve(faceCountNum);

    // 第一次遍历:计算每个面的规范化键并存入数组(避免重复排序)
    // 键 = (顶点索引, UV 索引) 配对排序 + 材质。UV 必须参与: 同一方块面上
    // 的 CTM overlay 会有多个面(顶点/材质相同, 仅 tile/UV 不同), 只按顶点
    // 去重会误删第二个 tile(表现为连接纹理缺角、缺边)。
    for (size_t i = 0; i < data.faces.size(); i++) {
        const auto& face = data.faces[i];
        std::array<std::pair<int, int>, 4> pairs = {{
            { face.vertexIndices[0], face.uvIndices[0] },
            { face.vertexIndices[1], face.uvIndices[1] },
            { face.vertexIndices[2], face.uvIndices[2] },
            { face.vertexIndices[3], face.uvIndices[3] }
        }};
        std::sort(pairs.begin(), pairs.end());
        std::array<int, 4> sortedVerts;
        std::array<int, 4> sortedUVs;
        for (int k = 0; k < 4; ++k) {
            sortedVerts[k] = pairs[k].first;
            sortedUVs[k] = pairs[k].second;
        }
        int matIndex = config.strictDeduplication ? face.materialIndex : -1;
        keys.push_back(FaceKey{ sortedVerts, sortedUVs, matIndex });
    }

    // 第二次遍历:每个重合面键保留首个面。
    // 旧实现仅保留 freq==1，两个完全重合的面会被「全部删除」；玻璃、
    // 玻璃板、多元素模型及水面等经常出现这类重复，导致随机缺面甚至整块消失。
    // ChunkGenerator 已在方块邻居层剔除真正的内部面，此处只能安全地去重，
    // 不能把所有副本都删除。
    std::unordered_set<FaceKey, FaceKeyHasher> kept;
    kept.reserve(faceCountNum);
    std::vector<Face> newFaces;
    newFaces.reserve(data.faces.size());

    for (size_t i = 0; i < keys.size(); i++) {
        if (kept.insert(keys[i]).second) {
            newFaces.push_back(data.faces[i]);
        }
    }

    data.faces.swap(newFaces);
}


// Greedy mesh 算法:合并相邻同材质、相同方向的面以减少面数
void ModelDeduplicator::GreedyMesh(ModelData& data) {
    // GreedyMesh 算法：按材质/法线/UV 连续性分组并合并面以减少面数
    // 步骤1：计算所有面的法向量，用于判断面方向是否一致
    // 步骤2：构建边到面映射，便于查找相邻面
    // 步骤3：构建顶点坐标到索引的映射，用于合并后查找原始顶点
    // 步骤4：定义UV连续性检查函数，判断面在UV贴图上是水平还是垂直
    // 步骤5：将所有面按材质、法线、UV连续性分组，同组面可合并
    // 步骤6：对每组调用 processGroup，执行贪心合并
    // 步骤7：收集合并结果并更新 data.faces 和 data.uvCoordinates
    // 步骤8：保留无法合并的单面，完成最终面列表
    // 基础类型与工具函数定义
    struct Vector3 { float x, y, z; };
    struct Vector2 { float x, y; };
    const float eps = 1e-6f;
    auto getVertex = [&](int idx){ return Vector3{ data.vertices[3*idx], data.vertices[3*idx+1], data.vertices[3*idx+2] }; };
    auto normalize = [&](Vector3 v){
        float len = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (len < eps) return v;
        return Vector3{ v.x/len, v.y/len, v.z/len };
    };
    auto cross = [&](const Vector3& a, const Vector3& b){
        return Vector3{ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
    };
    auto dot3 = [&](const Vector3& a, const Vector3& b){ return a.x*b.x + a.y*b.y + a.z*b.z; };

    size_t faceCount = data.faces.size();
    if (faceCount == 0) return;

    using Clock = std::chrono::high_resolution_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto t0 = Clock::now();

    // 1. 计算所有面的法线 (并行化)
    std::vector<Vector3> faceNormals(faceCount);
    {
        unsigned int numThreads = DedupThreadCount();
        if (numThreads == 0) numThreads = 1;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        size_t chunk = (faceCount + numThreads - 1) / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, faceCount);
            if (start >= end) continue; // 面数少于线程数时跳过空线程
            threads.emplace_back([&, start, end]() {
                for (size_t i = start; i < end; ++i) {
                    const auto& vs = data.faces[i].vertexIndices;
                    Vector3 p0 = getVertex(vs[0]);
                    Vector3 p1 = getVertex(vs[1]);
                    Vector3 p2 = getVertex(vs[2]);
                    Vector3 e1{ p1.x - p0.x, p1.y - p0.y, p1.z - p0.z };
                    Vector3 e2{ p2.x - p0.x, p2.y - p0.y, p2.z - p0.z };
                    faceNormals[i] = normalize(cross(e1, e2));
                }
            });
        }
        for (auto& th : threads) th.join();
    }
    auto t1 = Clock::now();
    std::cerr << "GreedyMesh Step1 normals: " << Ms(t1 - t0).count() << " ms\n";

    // 2. 构建边->面映射 (并行化+排序)
    struct EdgeKey { int v1, v2; bool operator==(const EdgeKey& o) const { return v1==o.v1 && v2==o.v2; } };
    struct EdgeKeyHasher { size_t operator()(const EdgeKey& e) const {
        return std::hash<long long>()(((long long)e.v1<<32) ^ (unsigned long long)e.v2);
    }};
    auto t2_fill_start = Clock::now();
    
    // 并行填充所有边 - 恢复原来的实现但做更好的内存管理
    std::vector<std::pair<EdgeKey,int>> allEdges;
    allEdges.reserve(faceCount * 4); // 预分配内存
    
    // 使用批次填充以减少同步成本
    const size_t BATCH_SIZE = 1024; // 每批次处理的面数
    std::vector<std::vector<std::pair<EdgeKey,int>>> threadBatches;
    unsigned int numThreads2 = DedupThreadCount(); if (numThreads2 == 0) numThreads2 = 1;
    threadBatches.resize(numThreads2);
    
    std::vector<std::thread> fillThreads; fillThreads.reserve(numThreads2);
    size_t facesPerThread = (faceCount + numThreads2 - 1) / numThreads2;
    
    for (unsigned int t = 0; t < numThreads2; ++t) {
        size_t startF = t * facesPerThread;
        size_t endF = std::min(startF + facesPerThread, faceCount);
        if (startF >= endF) continue; // 面数少于线程数时跳过空线程，避免 size_t 下溢
        size_t batchSize = (endF - startF) * 4; // 每个面有4条边
        threadBatches[t].reserve(batchSize);
        
        fillThreads.emplace_back([&, startF, endF, t]() {
            for (size_t i = startF; i < endF; ++i) {
                const auto& vs = data.faces[i].vertexIndices;
                for (int k = 0; k < 4; ++k) {
                    int a = vs[k], b = vs[(k+1)%4];
                    EdgeKey ek{ std::min(a,b), std::max(a,b) };
                    threadBatches[t].push_back({ek, static_cast<int>(i)});
                }
            }
        });
    }
    for (auto &th : fillThreads) th.join();
    
    // 合并所有线程的结果
    for (auto& batch : threadBatches) {
        allEdges.insert(allEdges.end(), batch.begin(), batch.end());
        // 释放内存
        std::vector<std::pair<EdgeKey,int>>().swap(batch);
    }
    
    // 清理
    threadBatches.clear();
    threadBatches.shrink_to_fit();
    
    auto t2_fill_end = Clock::now();
    
    // 排序所有边
    std::sort(allEdges.begin(), allEdges.end(), [](auto &a, auto &b) {
        if (a.first.v1 != b.first.v1) return a.first.v1 < b.first.v1;
        if (a.first.v2 != b.first.v2) return a.first.v2 < b.first.v2;
        return a.second < b.second; // 确保稳定排序
    });
    auto t2_sort_end = Clock::now();
    
    // 2.1 构建面邻接表 - 使用分段并行处理
    std::vector<std::vector<int>> faceAdj(faceCount);
    
    // 预分配空间，减少重新分配
    for (size_t i = 0; i < faceCount; ++i) {
        faceAdj[i].reserve(16);  // 合理估计每个面的邻接数
    }
    
    auto t2_adj_start = Clock::now();
    
    // 按边顺序构建邻接表。旧实现按边分段并行，但同一个面拥有多条边，
    // 不同线程会同时向同一个 faceAdj[f] vector push_back，造成堆损坏。
    // 此步骤通常仅占几毫秒，串行处理比为每个面加锁更快且稳定。
    size_t edgeIndex = 0;
    while (edgeIndex < allEdges.size()) {
        const EdgeKey edgeKey = allEdges[edgeIndex].first;
        std::vector<int> edgeFaces;
        while (edgeIndex < allEdges.size() && allEdges[edgeIndex].first == edgeKey) {
            edgeFaces.push_back(allEdges[edgeIndex].second);
            ++edgeIndex;
        }
        for (size_t i = 0; i < edgeFaces.size(); ++i) {
            const int f1 = edgeFaces[i];
            for (size_t j = 0; j < edgeFaces.size(); ++j) {
                if (i != j) faceAdj[f1].push_back(edgeFaces[j]);
            }
        }
    }
    
    // 释放内存
    std::vector<std::pair<EdgeKey,int>>().swap(allEdges);
    auto t2_adj_end = Clock::now();
    std::cerr << "GreedyMesh Step2 adjacency: " << Ms(t2_adj_end - t2_adj_start).count() << " ms\n";

    // 3. 生成顶点键索引对并排序 (并行化+排序)
    int vertCount = data.vertices.size() / 3;
    std::vector<std::pair<VertexKey,int>> vertKVPairs(vertCount);
    {
        unsigned int numThreads = DedupThreadCount();
        if (numThreads == 0) numThreads = 1;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        size_t chunkV = (vertCount + numThreads - 1) / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            size_t startV = t * chunkV;
            size_t endV = std::min(startV + chunkV, (size_t)vertCount);
            threads.emplace_back([&, startV, endV]() {
                for (size_t vi = startV; vi < endV; ++vi) {
                    float x = data.vertices[3*vi];
                    float y = data.vertices[3*vi + 1];
                    float z = data.vertices[3*vi + 2];
                    vertKVPairs[vi] = { MakeVertexKey(x, y, z), (int)vi };
                }
            });
        }
        for (auto &th : threads) th.join();
    }
    auto t3_start = Clock::now();
    std::sort(vertKVPairs.begin(), vertKVPairs.end(), [](auto &a, auto &b) {
        const auto &ka = a.first, &kb = b.first;
        if (ka.x != kb.x) return ka.x < kb.x;
        if (ka.y != kb.y) return ka.y < kb.y;
        return ka.z < kb.z;
    });
    auto t3_end = Clock::now();
    std::cerr << "GreedyMesh Step3 sort vert pairs: " << Ms(t3_end - t3_start).count() << " ms\n";
    // 顶点键 -> 索引的补充映射(仅用于量化键未命中时追加的新顶点)。
    // 不能向有序数组 vertKVPairs 中间插入(O(n))，否则未命中较多时复杂度会平方放大。
    std::unordered_map<VertexKey, int> extraVertexKeys;

    // Lookup lambda
    auto lookupVertexIndex = [&](const VertexKey &vk) {
        // 先查补充映射(通常为空，仅在浮点边界差异时增长)
        auto extraIt = extraVertexKeys.find(vk);
        if (extraIt != extraVertexKeys.end()) return extraIt->second;

        auto it = std::lower_bound(vertKVPairs.begin(), vertKVPairs.end(), vk,
            [](auto &a, const VertexKey &b) {
                if (a.first.x != b.x) return a.first.x < b.x;
                if (a.first.y != b.y) return a.first.y < b.y;
                return a.first.z < b.z;
            });
        if (it != vertKVPairs.end() && it->first.x == vk.x && it->first.y == vk.y && it->first.z == vk.z)
            return it->second;
        // 量化键未命中(仅可能出现在浮点边界差异):追加为新顶点并记录到补充映射，
        // 避免静默返回 0 造成几何错位。此处为串行调用，无需加锁。
        int newIdx = static_cast<int>(data.vertices.size() / 3);
        data.vertices.push_back(static_cast<float>(vk.x) / 10000.0f);
        data.vertices.push_back(static_cast<float>(vk.y) / 10000.0f);
        data.vertices.push_back(static_cast<float>(vk.z) / 10000.0f);
        extraVertexKeys.emplace(vk, newIdx);
        return newIdx;
    };

    // 4. UV 连续性检查 (Lambda定义)
    enum UVAxis { NONE=0, HORIZONTAL=1, VERTICAL=2 };
    auto checkUV = [&](int fi){
        const auto& face_uv = data.faces[fi];
        const auto& uvs_indices = face_uv.uvIndices;
        // 周期 atlas 材质(repeat): UV 落在格子内部, 以"格子矩形"作为边界基准;
        // 仅接受恰好覆盖整格的面, 其余(非整格 UV)不参与合并。
        float left = 0.0f, right = 1.0f, bottom = 0.0f, top = 1.0f;
        if (face_uv.materialIndex >= 0 && (size_t)face_uv.materialIndex < data.materials.size()) {
            const Material& mat = data.materials[face_uv.materialIndex];
            if (mat.uvAtlas && !mat.uvPeriodic) {
                // 非周期 atlas(ctm/random/horizontal/...): 只有整图即一格(1x1)
                // 才能按普通贴图合并; 其余格子面一律不参与(跨格合并会贴错图)。
                if (mat.uvCellW < 1.0f - eps || mat.uvCellH < 1.0f - eps) return NONE;
            }
            if (mat.uvPeriodic) {
                // 面必须近似 1x1 格: 台阶侧面等半格面每格 UV 推进速率不同,
                // 合并后必然错位, 一律不参与合并。
                {
                    float mn[3] = { 1e30f, 1e30f, 1e30f };
                    float mx[3] = { -1e30f, -1e30f, -1e30f };
                    for (int j = 0; j < 4; ++j) {
                        int vi = face_uv.vertexIndices[j];
                        if (vi < 0 || (size_t)(vi * 3 + 2) >= data.vertices.size()) return NONE;
                        for (int a = 0; a < 3; ++a) {
                            float c = data.vertices[vi * 3 + a];
                            mn[a] = std::min(mn[a], c);
                            mx[a] = std::max(mx[a], c);
                        }
                    }
                    float ext[3] = { mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2] };
                    std::sort(ext, ext + 3);
                    if (std::fabs(ext[1] - 1.0f) > 1e-3f || std::fabs(ext[2] - 1.0f) > 1e-3f) return NONE;
                }
                float uMin = 0.0f, uMax = 0.0f, vMin = 0.0f, vMax = 0.0f;
                bool first = true;
                for (int j = 0; j < 4; ++j) {
                    if (uvs_indices[j] < 0 || (uvs_indices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                        return NONE;
                    }
                    float u = data.uvCoordinates[2 * uvs_indices[j]];
                    float v = data.uvCoordinates[2 * uvs_indices[j] + 1];
                    if (first) { uMin = uMax = u; vMin = vMax = v; first = false; }
                    else {
                        uMin = std::min(uMin, u); uMax = std::max(uMax, u);
                        vMin = std::min(vMin, v); vMax = std::max(vMax, v);
                    }
                }
                if (std::fabs((uMax - uMin) - mat.uvCellW) > eps) return NONE;
                if (std::fabs((vMax - vMin) - mat.uvCellH) > eps) return NONE;
                left = uMin; right = uMax; bottom = vMin; top = vMax;
            }
        }
        int cntTop=0, cntBottom=0, cntLeft=0, cntRight=0;
        for (int j=0;j<4;++j) {
            if (uvs_indices[j] < 0 || (uvs_indices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                 return NONE; 
            }
            float u = data.uvCoordinates[2*uvs_indices[j]];
            float v = data.uvCoordinates[2*uvs_indices[j]+1];
            if (std::fabs(v-top)<eps) ++cntTop;
            if (std::fabs(v-bottom)<eps) ++cntBottom;
            if (std::fabs(u-left)<eps) ++cntLeft;
            if (std::fabs(u-right)<eps) ++cntRight;
        }
        if (cntTop==2 && cntBottom==2) return VERTICAL;
        if (cntLeft==2 && cntRight==2) return HORIZONTAL;
        return NONE;
    };

    // 5. 分组：使用并查集(Union-Find)结合 faceAdj 邻接表
    auto t5_start = Clock::now();
    // 并查集初始化
    std::vector<int> parent(faceCount);
    for (int i = 0; i < (int)faceCount; ++i) parent[i] = i;
    std::function<int(int)> findp = [&](int x) {
        return parent[x] == x ? x : parent[x] = findp(parent[x]);
    };
    auto unite = [&](int a, int b) {
        int pa = findp(a), pb = findp(b);
        if (pa != pb) parent[pb] = pa;
    };
    // 标记可分组面及记录UV轴
    std::vector<bool> eligible(faceCount, false);
    std::vector<UVAxis> faceAxis(faceCount, NONE);
    for (int i = 0; i < (int)faceCount; ++i) {
        const Face& f = data.faces[i];
        if (f.materialIndex < 0 || (size_t)f.materialIndex >= data.materials.size()) continue;
        if (data.materials[f.materialIndex].type == ANIMATED) continue;
        UVAxis ax = checkUV(i);
        if (ax == NONE) continue;
        eligible[i] = true;
        faceAxis[i] = ax;
    }
    // 遍历邻接表做 union
    for (int i = 0; i < (int)faceCount; ++i) {
        if (!eligible[i]) continue;
        const Face& fi = data.faces[i];
        for (int nb : faceAdj[i]) {
            if (!eligible[nb]) continue;
            const Face& fj = data.faces[nb];
            if (fj.materialIndex != fi.materialIndex) continue;
            if (faceAxis[nb] != faceAxis[i]) continue;
            Vector3 dn{faceNormals[nb].x - faceNormals[i].x,
                       faceNormals[nb].y - faceNormals[i].y,
                       faceNormals[nb].z - faceNormals[i].z};
            if (std::sqrt(dn.x*dn.x + dn.y*dn.y + dn.z*dn.z) > eps) continue;
            unite(i, nb);
        }
    }
    // 收集分组结果
    std::unordered_map<int, int> rootToGroup;
    rootToGroup.reserve(faceCount);
    std::vector<std::vector<int>> groups;
    groups.reserve(faceCount);
    for (int i = 0; i < (int)faceCount; ++i) {
        if (!eligible[i]) {
            groups.push_back({i});
        } else {
            int r = findp(i);
            auto it = rootToGroup.find(r);
            if (it == rootToGroup.end()) {
                int idx = groups.size();
                rootToGroup[r] = idx;
                groups.emplace_back();
                groups.back().push_back(i);
            } else {
                groups[it->second].push_back(i);
            }
        }
    }
    auto t5_end = Clock::now();
    std::cerr << "GreedyMesh Step5 grouping (UF): " << Ms(t5_end - t5_start).count() << " ms\n";

    // 6. 处理每个可合并组 (串行化)
    struct MergedResult { std::vector<Face> faces; std::vector<float> uvCoords; };
    auto processGroup = [&](std::vector<int> grp_indices)->MergedResult {
        MergedResult res;
        if (grp_indices.empty()) return res;

        int i0 = grp_indices[0];
        const Face& f0_group_base = data.faces[i0];
        const Material& groupMat = data.materials[f0_group_base.materialIndex];
        Vector3 N0_group_base = faceNormals[i0];
        Vector3 arbi_group_base = std::fabs(N0_group_base.x)>std::fabs(N0_group_base.z)? Vector3{0,0,1}:Vector3{1,0,0};
        Vector3 T1_group_base = normalize(cross(arbi_group_base,N0_group_base));
        Vector3 T2_group_base = normalize(cross(N0_group_base,T1_group_base));
        Vector3 P0_group_base = getVertex(f0_group_base.vertexIndices[0]);
        
        struct Entry {
            float minW, maxW, minH, maxH;
            float uMin, uMax, vMin, vMax;
            int rotation;
            std::array<int,4> vids;
            int originalFaceIndex;
            // 周期 atlas: 锚点仿射映射 (世界投影 (W,H) -> UV), 合并后用它重算 UV
            bool  anchorValid = false;
            float anchorW = 0.0f, anchorH = 0.0f;
            float anchorU = 0.0f, anchorV = 0.0f;
            float stepW_u = 0.0f, stepW_v = 0.0f;
            float stepH_u = 0.0f, stepH_v = 0.0f;
        };
        std::vector<Entry> entries;
        entries.reserve(grp_indices.size());

        for(int fi_original_idx : grp_indices){
            Entry e;
            const auto& f_entry = data.faces[fi_original_idx];
            e.vids = f_entry.vertexIndices;
            e.originalFaceIndex = fi_original_idx;

            std::array<Vector2,4> pts_proj;
            for(int j=0;j<4;++j){
                Vector3 P_vert = getVertex(e.vids[j]);
                Vector3 d_vec{P_vert.x-P0_group_base.x,P_vert.y-P0_group_base.y,P_vert.z-P0_group_base.z};
                float w_coord = dot3(d_vec,T1_group_base), h_coord = dot3(d_vec,T2_group_base);
                pts_proj[j] = Vector2{w_coord,h_coord};
                if(j==0){ e.minW=e.maxW=w_coord; e.minH=e.maxH=h_coord; }
                else { e.minW=std::min(e.minW,w_coord); e.maxW=std::max(e.maxW,w_coord);
                       e.minH=std::min(e.minH,h_coord); e.maxH=std::max(e.maxH,h_coord); }
            }
            for(int j=0;j<4;++j){
                if (f_entry.uvIndices[j] < 0 || (f_entry.uvIndices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                     e.uMin=0; e.uMax=0; e.vMin=0; e.vMax=0; 
                     break; 
                }
                float u_coord = data.uvCoordinates[2*f_entry.uvIndices[j]];
                float v_coord = data.uvCoordinates[2*f_entry.uvIndices[j]+1];
                if(j==0){ e.uMin=e.uMax=u_coord; e.vMin=e.vMax=v_coord; }
                else { e.uMin=std::min(e.uMin,u_coord); e.uMax=std::max(e.uMax,u_coord);
                       e.vMin=std::min(e.vMin,v_coord); e.vMax=std::max(e.vMax,v_coord); }
            }

            std::array<Vector2,4> uvs_rot_calc;
            bool uv_valid_for_rotation = true;
            for(int j=0;j<4;++j){ 
                if (f_entry.uvIndices[j] < 0 || (f_entry.uvIndices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                    uv_valid_for_rotation = false; break;
                }
                uvs_rot_calc[j]={data.uvCoordinates[2*f_entry.uvIndices[j]],data.uvCoordinates[2*f_entry.uvIndices[j]+1]}; 
            }
            if (!uv_valid_for_rotation) {
                e.rotation = 0; 
            } else {
                Vector2 dWx_rot{pts_proj[1].x - pts_proj[0].x, pts_proj[1].y - pts_proj[0].y};
                Vector2 dUx_rot{uvs_rot_calc[1].x - uvs_rot_calc[0].x, uvs_rot_calc[1].y - uvs_rot_calc[0].y};
                Vector2 dUy_rot{uvs_rot_calc[3].x - uvs_rot_calc[0].x, uvs_rot_calc[3].y - uvs_rot_calc[0].y};
                float dotWx_Ux = dWx_rot.x * dUx_rot.x + dWx_rot.y * dUx_rot.y;
                float dotWx_Uy = dWx_rot.x * dUy_rot.x + dWx_rot.y * dUy_rot.y;
                if (std::fabs(dotWx_Ux) >= std::fabs(dotWx_Uy)) {
                    e.rotation = (dotWx_Ux >= 0) ? 0 : 180;
                } else {
                    e.rotation = (dotWx_Uy >= 0) ? 90 : 270;
                }
            }
            // 周期 atlas: 从该面自身的顶点/UV 对应关系推出"世界 (W,H) -> UV"
            // 的仿射映射(每格步进)。合并后按世界坐标重排 UV, 天然覆盖
            // 旋转/镜像 UV, 且与合并顺序无关。
            if (groupMat.uvPeriodic && uv_valid_for_rotation) {
                Vector2 eA{ pts_proj[1].x - pts_proj[0].x, pts_proj[1].y - pts_proj[0].y };
                Vector2 eB{ pts_proj[3].x - pts_proj[0].x, pts_proj[3].y - pts_proj[0].y };
                Vector2 dA{ uvs_rot_calc[1].x - uvs_rot_calc[0].x, uvs_rot_calc[1].y - uvs_rot_calc[0].y };
                Vector2 dB{ uvs_rot_calc[3].x - uvs_rot_calc[0].x, uvs_rot_calc[3].y - uvs_rot_calc[0].y };
                const float axisEps = 1e-3f;
                bool ok = false;
                if (std::fabs(eA.x) > std::fabs(eA.y) + axisEps) {
                    // A 沿 W, B 沿 H
                    ok = std::fabs(eA.y) < axisEps && std::fabs(eB.x) < axisEps &&
                         std::fabs(eA.x) > axisEps && std::fabs(eB.y) > axisEps;
                    if (ok) {
                        e.stepW_u = dA.x / eA.x; e.stepW_v = dA.y / eA.x;
                        e.stepH_u = dB.x / eB.y; e.stepH_v = dB.y / eB.y;
                    }
                } else if (std::fabs(eA.y) > std::fabs(eA.x) + axisEps) {
                    // A 沿 H, B 沿 W
                    ok = std::fabs(eA.x) < axisEps && std::fabs(eB.y) < axisEps &&
                         std::fabs(eA.y) > axisEps && std::fabs(eB.x) > axisEps;
                    if (ok) {
                        e.stepH_u = dA.x / eA.y; e.stepH_v = dA.y / eA.y;
                        e.stepW_u = dB.x / eB.x; e.stepW_v = dB.y / eB.x;
                    }
                }
                if (ok) {
                    e.anchorValid = true;
                    e.anchorW = pts_proj[0].x;
                    e.anchorH = pts_proj[0].y;
                    e.anchorU = uvs_rot_calc[0].x;
                    e.anchorV = uvs_rot_calc[0].y;
                }
            }
            entries.push_back(e);
        }

        // 回归使用原始但更安全的合并函数
        auto performLimitedMergePass = [&](std::vector<Entry>& current_entries_ref, bool mergeW_axis) -> bool {
            if (current_entries_ref.size() <= 1) return false;
            
            bool any_merge_overall = false;
            std::vector<char> processed_mask(current_entries_ref.size(), 0); 
            std::vector<Entry> next_entries_list;
            next_entries_list.reserve(current_entries_ref.size());
            
            // 预处理：按旋转角度分组
            std::vector<std::vector<size_t>> rotationGroups(4); // 0, 90, 180, 270度
            for (size_t i = 0; i < current_entries_ref.size(); ++i) {
                int rot = current_entries_ref[i].rotation;
                int idx = rot / 90;
                if (idx >= 0 && idx < 4) {
                    rotationGroups[idx].push_back(i);
                }
            }
            
            for (size_t i_pass = 0; i_pass < current_entries_ref.size(); ++i_pass) {
                if (processed_mask[i_pass] != 0) continue; 
                
                Entry cur_pass = current_entries_ref[i_pass];
                processed_mask[i_pass] = 1;
                
                // 仅在同一旋转组内查找匹配项
                int rotGroup = cur_pass.rotation / 90;
                if (rotGroup < 0 || rotGroup >= 4) rotGroup = 0;
                
                bool found_match = false;
                for (size_t idx : rotationGroups[rotGroup]) {
                    size_t j_pass = idx;
                    
                    if (i_pass == j_pass || processed_mask[j_pass] != 0) continue;
                    if (current_entries_ref[j_pass].rotation != cur_pass.rotation) continue;
                    
                    Entry& other_entry = current_entries_ref[j_pass];
                    bool merged_now = false;
                    
                    if (mergeW_axis) {
                        // 检查两个面是否在W轴方向相邻
                        if (std::fabs(cur_pass.maxW - other_entry.minW) < eps && 
                            std::fabs(cur_pass.minH - other_entry.minH) < eps && 
                            std::fabs(cur_pass.maxH - other_entry.maxH) < eps) {
                            
                            float deltaU = other_entry.uMax - other_entry.uMin;
                            float deltaV = other_entry.vMax - other_entry.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) 
                                cur_pass.vMax += deltaV; 
                            else 
                                cur_pass.uMax += deltaU;
                                
                            cur_pass.maxW = other_entry.maxW;
                            merged_now = true;
                        } 
                        // 检查另一个方向是否相邻
                        else if (std::fabs(cur_pass.minW - other_entry.maxW) < eps && 
                                 std::fabs(cur_pass.minH - other_entry.minH) < eps && 
                                 std::fabs(cur_pass.maxH - other_entry.maxH) < eps) {
                            
                            float deltaU = other_entry.uMax - other_entry.uMin;
                            float deltaV = other_entry.vMax - other_entry.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) 
                                cur_pass.vMin -= deltaV; 
                            else 
                                cur_pass.uMin -= deltaU;
                            
                            cur_pass.minW = other_entry.minW;
                            merged_now = true;
                        }
                    } else {
                        // 检查两个面是否在H轴方向相邻
                        if (std::fabs(cur_pass.maxH - other_entry.minH) < eps && 
                            std::fabs(cur_pass.minW - other_entry.minW) < eps && 
                            std::fabs(cur_pass.maxW - other_entry.maxW) < eps) {
                            
                            float deltaU = other_entry.uMax - other_entry.uMin;
                            float deltaV = other_entry.vMax - other_entry.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) 
                                cur_pass.uMax += deltaU; 
                            else 
                                cur_pass.vMax += deltaV;
                            
                            cur_pass.maxH = other_entry.maxH;
                            merged_now = true;
                        } 
                        // 检查另一个方向是否相邻
                        else if (std::fabs(cur_pass.minH - other_entry.maxH) < eps && 
                                 std::fabs(cur_pass.minW - other_entry.minW) < eps && 
                                 std::fabs(cur_pass.maxW - other_entry.maxW) < eps) {
                            
                            float deltaU = other_entry.uMax - other_entry.uMin;
                            float deltaV = other_entry.vMax - other_entry.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) 
                                cur_pass.uMin -= deltaU; 
                            else 
                                cur_pass.vMin -= deltaV;
                            
                            cur_pass.minH = other_entry.minH;
                            merged_now = true;
                        }
                    }
                    
                    if (merged_now) {
                        processed_mask[j_pass] = 2;
                        any_merge_overall = true;
                        found_match = true;
                        break;
                    }
                }
                
                next_entries_list.push_back(cur_pass);
            }
            
            // 添加未处理的条目
            for (size_t k_pass = 0; k_pass < current_entries_ref.size(); ++k_pass) {
                if (processed_mask[k_pass] == 0) {
                    next_entries_list.push_back(current_entries_ref[k_pass]);
                }
            }
            
            current_entries_ref.swap(next_entries_list);
            return any_merge_overall;
        };
        bool changed_in_super_iteration = true;
        while (changed_in_super_iteration) {
            changed_in_super_iteration = false;
            if (performLimitedMergePass(entries, true)) { 
                changed_in_super_iteration = true;
            }
            if (performLimitedMergePass(entries, false)) { 
                changed_in_super_iteration = true;
            }
        }

        for(auto& e_final: entries){
            Face nf; nf.materialIndex=f0_group_base.materialIndex; nf.faceDirection=UNKNOWN;
            nf.tintIndex=f0_group_base.tintIndex;
            std::array<int,4> vidx_final;
            for(int k_final=0;k_final<4;++k_final){
                float w2d = (k_final==0||k_final==3? e_final.minW : e_final.maxW);
                float h2d = (k_final==0||k_final==1? e_final.minH : e_final.maxH);
                Vector3 pos_final{P0_group_base.x + w2d*T1_group_base.x + h2d*T2_group_base.x,
                                  P0_group_base.y + w2d*T1_group_base.y + h2d*T2_group_base.y,
                                  P0_group_base.z + w2d*T1_group_base.z + h2d*T2_group_base.z};
                {
                    VertexKey vk = MakeVertexKey(pos_final.x, pos_final.y, pos_final.z);
                    int mappedIdx = lookupVertexIndex(vk);
                    vidx_final[k_final] = mappedIdx;
                    nf.vertexIndices[k_final] = mappedIdx;
                }
            }
            res.faces.push_back(nf);
            {
                float du_final = e_final.uMax - e_final.uMin;
                float dv_final = e_final.vMax - e_final.vMin;
                for(int k_uv=0; k_uv < 4; ++k_uv) {
                    float w_uv = (k_uv == 0 || k_uv == 3) ? e_final.minW : e_final.maxW;
                    float h_uv = (k_uv == 0 || k_uv == 1) ? e_final.minH : e_final.maxH;
                    float u_final, v_final;
                    if (e_final.anchorValid) {
                        // 周期 atlas: 按锚点仿射映射从世界坐标重算 UV
                        u_final = e_final.anchorU + e_final.stepW_u * (w_uv - e_final.anchorW)
                                                 + e_final.stepH_u * (h_uv - e_final.anchorH);
                        v_final = e_final.anchorV + e_final.stepW_v * (w_uv - e_final.anchorW)
                                                 + e_final.stepH_v * (h_uv - e_final.anchorH);
                    } else {
                        float fw_uv = (k_uv == 1 || k_uv == 2) ? 1.0f : 0.0f;
                        float fh_uv = (k_uv >= 2) ? 1.0f : 0.0f;
                        float lu_uv, lv_uv;
                        switch (e_final.rotation) {
                            case 0:   lu_uv = fw_uv;             lv_uv = fh_uv;             break;
                            case 90:  lu_uv = fh_uv;             lv_uv = 1.0f - fw_uv;      break;
                            case 180: lu_uv = 1.0f - fw_uv;      lv_uv = 1.0f - fh_uv;      break;
                            case 270: lu_uv = 1.0f - fh_uv;      lv_uv = fw_uv;             break;
                            default:  lu_uv = fw_uv;             lv_uv = fh_uv;             break;
                        }
                        u_final = e_final.uMin + lu_uv * du_final;
                        v_final = e_final.vMin + lv_uv * dv_final;
                    }
                    res.uvCoords.push_back(u_final);
                    res.uvCoords.push_back(v_final);
                }
            }
        }
        return res;
    };

    // 7. 串行处理所有组，保证正确性
    auto t7_start = Clock::now();
    {
        // 注意：不再按组大小排序，保持原顺序
        
        // 分配足够的空间
        std::vector<Face> final_model_faces;
        final_model_faces.reserve(faceCount); // 保守估计：不会比原始面数多
        
        size_t next_new_uv_start_index = data.uvCoordinates.size() / 2;
        data.uvCoordinates.reserve(data.uvCoordinates.size() + faceCount * 8); // 为每个面准备充足UV空间
        
        // 逐个处理每个组
        for (const auto& grp : groups) {
            if (grp.size() > 1) {
                // 处理需要合并的组
                MergedResult mr = processGroup(grp);
                
                // 给所有新生成的面设置UV索引
                for (size_t i = 0; i < mr.faces.size(); ++i) {
                    Face processed_face = mr.faces[i];
                    for (int k = 0; k < 4; ++k) {
                        processed_face.uvIndices[k] = next_new_uv_start_index + i * 4 + k;
                    }
                    final_model_faces.push_back(processed_face);
                }
                
                // 添加UV坐标
                data.uvCoordinates.insert(
                    data.uvCoordinates.end(),
                    std::make_move_iterator(mr.uvCoords.begin()),
                    std::make_move_iterator(mr.uvCoords.end()));
                    
                next_new_uv_start_index += mr.uvCoords.size() / 2;
            } else if (!grp.empty()) {
                // 直接添加单个面，保留原始面的所有属性
                final_model_faces.push_back(data.faces[grp[0]]);
            }
        }
        
        // 记录处理后的面数和UV数量
        std::cerr << "GreedyMesh: 原始面数 " << faceCount 
                  << ", 处理后面数 " << final_model_faces.size()
                  << ", UV坐标数 " << data.uvCoordinates.size() / 2
                  << std::endl;
        
        // 替换原始面数组
        data.faces.swap(final_model_faces);
    }
    auto t7_end = Clock::now();
    std::cerr << "GreedyMesh Step7 merging: " << Ms(t7_end - t7_start).count() << " ms\n";
    auto t_end = Clock::now();
    std::cerr << "GreedyMesh total: " << Ms(t_end - t0).count() << " ms\n";
}

// 综合去重和优化方法
void ModelDeduplicator::DeduplicateModel(ModelData& data) {
    using Clock = std::chrono::high_resolution_clock;
    using Ms = std::chrono::duration<double, std::milli>;
    auto dm_start = Clock::now();
    auto& monitor = GetTaskMonitor();

    monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices");
    {
        auto t0 = Clock::now();
        DeduplicateVertices(data);
        auto t1 = Clock::now();
        std::cerr << "DeduplicateVertices: " << Ms(t1 - t0).count() << " ms\n";
    }

    monitor.SetStatus(TaskStatus::DEDUPLICATING_UV, "DeduplicateUV");
    {
        auto t2 = Clock::now();
        DeduplicateUV(data);
        auto t3 = Clock::now();
        std::cerr << "DeduplicateUV: " << Ms(t3 - t2).count() << " ms\n";
    }

    monitor.SetStatus(TaskStatus::DEDUPLICATING_FACES, "DeduplicateFaces");
    {
        auto t4 = Clock::now();
        DeduplicateFaces(data);
        auto t5 = Clock::now();
        std::cerr << "DeduplicateFaces: " << Ms(t5 - t4).count() << " ms\n";
    }

    if (config.useGreedyMesh) {
        monitor.SetStatus(TaskStatus::GREEDY_MESHING, "GreedyMesh");
        {
            auto tg0 = Clock::now();
            GreedyMesh(data);
            auto tg1 = Clock::now();
            std::cerr << "GreedyMesh: " << Ms(tg1 - tg0).count() << " ms\n";
        }
        monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices after GreedyMesh");
        {
            auto t6 = Clock::now();
            DeduplicateVertices(data);
            auto t7 = Clock::now();
            std::cerr << "DeduplicateVertices after GreedyMesh: " << Ms(t7 - t6).count() << " ms\n";
        }
        monitor.SetStatus(TaskStatus::DEDUPLICATING_UV, "DeduplicateUV after GreedyMesh");
        {
            auto t8 = Clock::now();
            DeduplicateUV(data);
            auto t9 = Clock::now();
            std::cerr << "DeduplicateUV after GreedyMesh: " << Ms(t9 - t8).count() << " ms\n";
        }
    }
    auto dm_end = Clock::now();
    std::cerr << "DeduplicateModel total: " << Ms(dm_end - dm_start).count() << " ms\n";
}



