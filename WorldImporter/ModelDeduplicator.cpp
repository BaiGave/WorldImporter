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
    unsigned int numThreads = std::thread::hardware_concurrency();
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
                
                // 确保精确的量化，使用相同的舍入方法
                int rx = static_cast<int>(std::round(x * 10000.0f));
                int ry = static_cast<int>(std::round(y * 10000.0f));
                int rz = static_cast<int>(std::round(z * 10000.0f));
                
                keys[i] = { VertexKey{rx, ry, rz}, static_cast<int>(i) };
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
    for (size_t i = 0; i < data.faces.size(); i++) {
        const auto& face = data.faces[i];
        std::array<int, 4> faceArray = {
            face.vertexIndices[0], face.vertexIndices[1],
            face.vertexIndices[2], face.vertexIndices[3]
        };
        std::array<int, 4> sorted = faceArray;
        std::sort(sorted.begin(), sorted.end());
        int matIndex = config.strictDeduplication ? face.materialIndex : -1;
        keys.push_back(FaceKey{ sorted, matIndex });
    }

    // 使用预分配容量的 unordered_map 来统计每个 FaceKey 的出现次数
    std::unordered_map<FaceKey, int, FaceKeyHasher> freq;
    freq.reserve(faceCountNum);
    for (const auto& key : keys) {
        freq[key]++;
    }

    // 第二次遍历:过滤只出现一次的面
    std::vector<Face> newFaces;
    newFaces.reserve(data.faces.size());

    for (size_t i = 0; i < keys.size(); i++) {
        if (freq[keys[i]] == 1) {
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
        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 1;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        size_t chunk = (faceCount + numThreads - 1) / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            size_t start = t * chunk;
            size_t end = std::min(start + chunk, faceCount);
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
    // 并行填充所有边
    std::vector<std::pair<EdgeKey,int>> allEdges(faceCount * 4);
    unsigned int numThreads2 = std::thread::hardware_concurrency(); if (numThreads2 == 0) numThreads2 = 1;
    std::vector<std::thread> fillThreads; fillThreads.reserve(numThreads2);
    size_t facesPerThread = (faceCount + numThreads2 - 1) / numThreads2;
    for (unsigned int t = 0; t < numThreads2; ++t) {
        size_t startF = t * facesPerThread;
        size_t endF = std::min(startF + facesPerThread, faceCount);
        fillThreads.emplace_back([&, startF, endF]() {
            for (size_t i = startF; i < endF; ++i) {
                const auto& vs = data.faces[i].vertexIndices;
                for (int k = 0; k < 4; ++k) {
                    int a = vs[k], b = vs[(k+1)%4];
                    EdgeKey ek{ std::min(a,b), std::max(a,b) };
                    allEdges[i * 4 + k] = {ek, (int)i};
                }
            }
        });
    }
    for (auto &th : fillThreads) th.join();
    auto t2_fill_end = Clock::now();
    // 排序所有边
    std::sort(allEdges.begin(), allEdges.end(), [](auto &a, auto &b) {
        if (a.first.v1 != b.first.v1) return a.first.v1 < b.first.v1;
        return a.first.v2 < b.first.v2;
    });
    auto t2_sort_end = Clock::now();
    // 2.1 构建面邻接表 (并行化+排序完成后)
    std::vector<std::vector<int>> faceAdj(faceCount);
    auto t2_adj_start = Clock::now();
    size_t idxEdge = 0;
    while (idxEdge < allEdges.size()) {
        EdgeKey ek = allEdges[idxEdge].first;
        size_t j = idxEdge + 1;
        while (j < allEdges.size() && allEdges[j].first == ek) ++j;
        for (size_t p = idxEdge; p < j; ++p) {
            int f1 = allEdges[p].second;
            for (size_t q = idxEdge; q < j; ++q) {
                if (p == q) continue;
                int f2 = allEdges[q].second;
                faceAdj[f1].push_back(f2);
            }
        }
        idxEdge = j;
    }
    auto t2_adj_end = Clock::now();
    std::cerr << "GreedyMesh Step2 adjacency: " << Ms(t2_adj_end - t2_sort_end).count() << " ms\n";

    // 3. 生成顶点键索引对并排序 (并行化+排序)
    int vertCount = data.vertices.size() / 3;
    std::vector<std::pair<VertexKey,int>> vertKVPairs(vertCount);
    {
        unsigned int numThreads = std::thread::hardware_concurrency();
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
                    int rx = static_cast<int>(x * 10000 + 0.5f);
                    int ry = static_cast<int>(y * 10000 + 0.5f);
                    int rz = static_cast<int>(z * 10000 + 0.5f);
                    vertKVPairs[vi] = { VertexKey{rx, ry, rz}, (int)vi };
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
    // Lookup lambda
    auto lookupVertexIndex = [&](const VertexKey &vk) {
        auto it = std::lower_bound(vertKVPairs.begin(), vertKVPairs.end(), vk,
            [](auto &a, const VertexKey &b) {
                if (a.first.x != b.x) return a.first.x < b.x;
                if (a.first.y != b.y) return a.first.y < b.y;
                return a.first.z < b.z;
            });
        if (it != vertKVPairs.end() && it->first.x == vk.x && it->first.y == vk.y && it->first.z == vk.z)
            return it->second;
        return 0;
    };

    // 4. UV 连续性检查 (Lambda定义)
    enum UVAxis { NONE=0, HORIZONTAL=1, VERTICAL=2 };
    auto checkUV = [&](int fi){
        const auto& uvs_indices = data.faces[fi].uvIndices;
        int cntTop=0, cntBottom=0, cntLeft=0, cntRight=0;
        for (int j=0;j<4;++j) {
            if (uvs_indices[j] < 0 || (uvs_indices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                 return NONE; 
            }
            float u = data.uvCoordinates[2*uvs_indices[j]];
            float v = data.uvCoordinates[2*uvs_indices[j]+1];
            if (std::fabs(v-1.0f)<eps) ++cntTop;
            if (std::fabs(v)<eps) ++cntBottom;
            if (std::fabs(u)<eps) ++cntLeft;
            if (std::fabs(u-1.0f)<eps) ++cntRight;
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
        Vector3 N0_group_base = faceNormals[i0];
        Vector3 arbi_group_base = std::fabs(N0_group_base.x)>std::fabs(N0_group_base.z)? Vector3{0,0,1}:Vector3{1,0,0};
        Vector3 T1_group_base = normalize(cross(arbi_group_base,N0_group_base));
        Vector3 T2_group_base = normalize(cross(N0_group_base,T1_group_base));
        Vector3 P0_group_base = getVertex(f0_group_base.vertexIndices[0]);
        
        struct Entry { float minW, maxW, minH, maxH; float uMin, uMax, vMin, vMax; int rotation; std::array<int,4> vids; int originalFaceIndex; };
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
            entries.push_back(e);
        }

        auto performLimitedMergePass = [&](std::vector<Entry>& current_entries_ref, bool mergeW_axis) -> bool {
            bool any_merge_overall = false;
            std::vector<char> processed_mask(current_entries_ref.size(), 0); 
            std::vector<Entry> next_entries_list;
            for (size_t i_pass = 0; i_pass < current_entries_ref.size(); ++i_pass) {
                if (processed_mask[i_pass] != 0) continue; 
                Entry cur_pass = current_entries_ref[i_pass];
                processed_mask[i_pass] = 1; 
                for (size_t j_pass = 0; j_pass < current_entries_ref.size(); ++j_pass) {
                    if (i_pass == j_pass || processed_mask[j_pass] != 0) continue; 
                    if (current_entries_ref[j_pass].rotation != cur_pass.rotation) continue;
                    bool merged_now = false;
                    Entry other_entry_copy_pass = current_entries_ref[j_pass]; 
                    if (mergeW_axis) {
                        if (std::fabs(cur_pass.maxW - other_entry_copy_pass.minW) < eps && std::fabs(cur_pass.minH - other_entry_copy_pass.minH) < eps && std::fabs(cur_pass.maxH - other_entry_copy_pass.maxH) < eps) {
                            float deltaU = other_entry_copy_pass.uMax - other_entry_copy_pass.uMin;
                            float deltaV = other_entry_copy_pass.vMax - other_entry_copy_pass.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) cur_pass.vMax += deltaV; else cur_pass.uMax += deltaU;
                            cur_pass.maxW = other_entry_copy_pass.maxW;
                            merged_now = true;
                        } else if (std::fabs(cur_pass.minW - other_entry_copy_pass.maxW) < eps && std::fabs(cur_pass.minH - other_entry_copy_pass.minH) < eps && std::fabs(cur_pass.maxH - other_entry_copy_pass.maxH) < eps) {
                            float deltaU = other_entry_copy_pass.uMax - other_entry_copy_pass.uMin;
                            float deltaV = other_entry_copy_pass.vMax - other_entry_copy_pass.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) cur_pass.vMin -= deltaV; else cur_pass.uMin -= deltaU;
                            cur_pass.minW = other_entry_copy_pass.minW;
                            merged_now = true;
                        }
                    } else { 
                        if (std::fabs(cur_pass.maxH - other_entry_copy_pass.minH) < eps && std::fabs(cur_pass.minW - other_entry_copy_pass.minW) < eps && std::fabs(cur_pass.maxW - other_entry_copy_pass.maxW) < eps) {
                            float deltaU = other_entry_copy_pass.uMax - other_entry_copy_pass.uMin;
                            float deltaV = other_entry_copy_pass.vMax - other_entry_copy_pass.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) cur_pass.uMax += deltaU; else cur_pass.vMax += deltaV;
                            cur_pass.maxH = other_entry_copy_pass.maxH;
                            merged_now = true;
                        } else if (std::fabs(cur_pass.minH - other_entry_copy_pass.maxH) < eps && std::fabs(cur_pass.minW - other_entry_copy_pass.minW) < eps && std::fabs(cur_pass.maxW - other_entry_copy_pass.maxW) < eps) {
                            float deltaU = other_entry_copy_pass.uMax - other_entry_copy_pass.uMin;
                            float deltaV = other_entry_copy_pass.vMax - other_entry_copy_pass.vMin;
                            if (cur_pass.rotation == 90 || cur_pass.rotation == 270) cur_pass.uMin -= deltaU; else cur_pass.vMin -= deltaV;
                            cur_pass.minH = other_entry_copy_pass.minH;
                            merged_now = true;
                        }
                    }
                    if (merged_now) {
                        processed_mask[j_pass] = 2; 
                        any_merge_overall = true;
                        break; 
                    }
                }
                next_entries_list.push_back(cur_pass); 
            }
            for(size_t k_pass=0; k_pass < current_entries_ref.size(); ++k_pass) {
                if(processed_mask[k_pass] == 0) next_entries_list.push_back(current_entries_ref[k_pass]);
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
            std::array<int,4> vidx_final;
            for(int k_final=0;k_final<4;++k_final){
                float w2d = (k_final==0||k_final==3? e_final.minW : e_final.maxW);
                float h2d = (k_final==0||k_final==1? e_final.minH : e_final.maxH);
                Vector3 pos_final{P0_group_base.x + w2d*T1_group_base.x + h2d*T2_group_base.x,
                                  P0_group_base.y + w2d*T1_group_base.y + h2d*T2_group_base.y,
                                  P0_group_base.z + w2d*T1_group_base.z + h2d*T2_group_base.z};
                int rx_final=int(pos_final.x*10000+0.5f), ry_final=int(pos_final.y*10000+0.5f), rz_final=int(pos_final.z*10000+0.5f);
                {
                    VertexKey vk{rx_final, ry_final, rz_final};
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
                    float u_final = e_final.uMin + lu_uv * du_final;
                    float v_final = e_final.vMin + lv_uv * dv_final;
                    res.uvCoords.push_back(u_final);
                    res.uvCoords.push_back(v_final);
                }
            }
        }
        return res;
    };

    // 7. 并行处理各可合并组，并收集结果（即时整合以降低峰值内存）
    auto t7_start = Clock::now();
    {
        std::vector<Face> final_model_faces;
        final_model_faces.reserve(faceCount);
        size_t next_new_uv_start_index = data.uvCoordinates.size() / 2;
        data.uvCoordinates.reserve(data.uvCoordinates.size() + faceCount * 8);
        std::mutex resultMutex;
        std::atomic<size_t> nextGroupIdx(0);
        unsigned int numThreads = std::thread::hardware_concurrency();
        if (numThreads == 0) numThreads = 1;
        std::vector<std::thread> workers;
        workers.reserve(numThreads);
        for (unsigned int t = 0; t < numThreads; ++t) {
            workers.emplace_back([&]() {
                size_t idx;
                while ((idx = nextGroupIdx.fetch_add(1)) < groups.size()) {
                    const auto &grp = groups[idx];
                    if (grp.size() > 1) {
                        MergedResult mr = processGroup(grp);
                        std::lock_guard<std::mutex> lock(resultMutex);
                        for (size_t i = 0; i < mr.faces.size(); ++i) {
                            Face processed_face = mr.faces[i];
                            for (int k = 0; k < 4; ++k) {
                                processed_face.uvIndices[k] = next_new_uv_start_index + i * 4 + k;
                            }
                            final_model_faces.push_back(processed_face);
                        }
                        data.uvCoordinates.insert(
                            data.uvCoordinates.end(),
                            std::make_move_iterator(mr.uvCoords.begin()),
                            std::make_move_iterator(mr.uvCoords.end()));
                        next_new_uv_start_index += mr.uvCoords.size() / 2;
                    } else {
                        std::lock_guard<std::mutex> lock(resultMutex);
                        final_model_faces.push_back(data.faces[grp[0]]);
                    }
                }
            });
        }
        for (auto &w : workers) w.join();
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



