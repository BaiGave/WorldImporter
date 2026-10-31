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
#include <unordered_map>
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
#include <span>
#include <stdexcept>
#undef max
#undef min
/* 并发预算与去重线程数 */
namespace {
    std::atomic<int> g_modelThreadBudget{1};

    // Append-only lookup for nonnegative vertex indices. Inline slots avoid
    // per-key nodes and pointer chasing; no caller observes hash-table order.
    class VertexIndexMap {
        struct Slot {
            VertexKey key;
            int index = -1;
        };
        std::vector<Slot> slots;
        size_t count = 0;

        static size_t capacityFor(size_t expected) {
            size_t capacity = 16;
            // Keep an empty slot, including when GreedyMesh appends new keys.
            // Rounding to a power of two permits masking instead of division.
            while (expected >= capacity - capacity / 8) {
                if (capacity > std::vector<Slot>().max_size() / 2)
                    throw std::length_error("vertex index table too large");
                capacity *= 2;
            }
            return capacity;
        }

        static size_t hashKey(const VertexKey& key) {
            // Pack x/y without overlap and spread z across all bits before
            // one avalanche. Grid keys have many low zero bits; masking an
            // unfinalized spatial hash would cluster them in a few slots.
            const uint64_t xy = (static_cast<uint64_t>(static_cast<uint32_t>(key.x)) << 32) |
                static_cast<uint32_t>(key.y);
            uint64_t hash = xy ^ (static_cast<uint32_t>(key.z) * 0x9e3779b97f4a7c15ULL);
            hash ^= hash >> 30;
            hash *= 0xbf58476d1ce4e5b9ULL;
            hash ^= hash >> 27;
            hash *= 0x94d049bb133111ebULL;
            hash ^= hash >> 31;
            return static_cast<size_t>(hash);
        }

        size_t findSlot(const VertexKey& key, size_t hash) const {
            const size_t mask = slots.size() - 1;
            size_t pos = hash & mask;
            while (slots[pos].index != -1 && slots[pos].key != key)
                pos = (pos + 1) & mask;
            return pos;
        }

    public:
        explicit VertexIndexMap(size_t expected) : slots(capacityFor(expected)) {}

        // Prepare independent lookups ahead of the ordered insert pass. A
        // growth between preparation and insertion only makes this cache hint
        // stale; insertion always probes the current table with the full hash.
        size_t prepare(const VertexKey& key) const {
            const size_t hash = hashKey(key);
#if defined(__GNUC__) || defined(__clang__)
            __builtin_prefetch(&slots[hash & (slots.size() - 1)], 0, 1);
#endif
            return hash;
        }

        std::pair<int, bool> tryEmplace(const VertexKey& key, int index) {
            return tryEmplace(key, index, hashKey(key));
        }

        std::pair<int, bool> tryEmplace(const VertexKey& key, int index, size_t hash) {
            size_t pos = findSlot(key, hash);
            if (slots[pos].index != -1) return {slots[pos].index, false};
            if (count + 1 >= slots.size() - slots.size() / 8) {
                // Only new keys can grow the table. Rehashing never changes
                // the assigned indices or the original coordinates they name.
                std::vector<Slot> oldSlots(capacityFor(count + 1));
                oldSlots.swap(slots);
                for (const auto& slot : oldSlots) {
                    if (slot.index != -1) slots[findSlot(slot.key, hashKey(slot.key))] = slot;
                }
                pos = findSlot(key, hash);
            }
            slots[pos] = Slot{key, index};
            ++count;
            return {index, true};
        }
    };

    // Membership only: callers publish faces in input order, never slot order.
    class FaceKeySet {
        struct Slot {
            FaceKey key;
            bool occupied = false;
        };
        std::vector<Slot> slots;

        static size_t capacityFor(size_t expected) {
            size_t capacity = 16;
            // The input face count bounds insertions, so no rehash is needed.
            // Leave empty slots for terminating probes, even for unique faces.
            while (expected >= capacity - capacity / 8) {
                if (capacity > std::vector<Slot>().max_size() / 2)
                    throw std::length_error("face key table too large");
                capacity *= 2;
            }
            return capacity;
        }

    public:
        explicit FaceKeySet(size_t expected) : slots(capacityFor(expected)) {}

        bool insert(const FaceKey& key) {
            const size_t mask = slots.size() - 1;
            size_t pos = FaceKeyHasher{}(key) & mask;
            while (slots[pos].occupied) {
                if (slots[pos].key == key) return false;
                pos = (pos + 1) & mask;
            }
            slots[pos].key = key;
            slots[pos].occupied = true;
            return true;
        }
    };
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

    // Imported faces usually repeat corners. Start below the input bound to
    // keep probes in a smaller table; unique-heavy inputs grow as needed.
    VertexIndexMap vertexIndices(vertCount / 2);
    std::vector<int> indexMap(vertCount);
    std::vector<float> newVertices;
    newVertices.reserve(data.vertices.size());

    // Visit original indices in order: the first vertex for each quantized key
    // supplies the original float coordinates, just like the old stable sort.
    int newIndex = 0;
    // Batching exposes independent quantization and cache misses. Only the
    // final loop inserts, so duplicates within/across batches stay first-wins.
    constexpr size_t batchSize = 64;
    std::array<VertexKey, batchSize> keys;
    std::array<size_t, batchSize> hashes;
    for (size_t base = 0; base < vertCount; base += batchSize) {
        const size_t size = std::min(batchSize, vertCount - base);
        for (size_t j = 0; j < size; ++j) {
            const size_t i = base + j;
            keys[j] = MakeVertexKey(data.vertices[3*i], data.vertices[3*i + 1], data.vertices[3*i + 2]);
        }
        for (size_t j = 0; j < size; ++j)
            hashes[j] = vertexIndices.prepare(keys[j]);
        for (size_t j = 0; j < size; ++j) {
            const size_t i = base + j;
            const auto [index, inserted] = vertexIndices.tryEmplace(keys[j], newIndex, hashes[j]);
            if (inserted) {
                newVertices.push_back(data.vertices[3*i]);
                newVertices.push_back(data.vertices[3*i + 1]);
                newVertices.push_back(data.vertices[3*i + 2]);
                ++newIndex;
            }
            indexMap[i] = index;
        }
    }
    data.vertices = std::move(newVertices);

    auto t3 = Clock::now();
    std::cerr << "顶点键与哈希构建: " << Ms(t3 - t0).count() << " ms\n";

    // Keep face order and winding; each worker remaps a disjoint face range.
    const size_t faceCount = data.faces.size();
    auto remapFaces = [&](size_t start, size_t end) {
        for (size_t fi = start; fi < end; ++fi) {
            for (int &idx : data.faces[fi].vertexIndices) {
                idx = indexMap[idx];
            }
        }
    };
    const unsigned int numThreads = DedupThreadCount();
    if (faceCount > 0 && numThreads > 1) {
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        const size_t chunk = (faceCount + numThreads - 1) / numThreads;
        for (unsigned int t = 0; t < numThreads; ++t) {
            const size_t start = t * chunk;
            const size_t end = std::min(start + chunk, faceCount);
            threads.emplace_back(remapFaces, start, end);
        }
        for (auto &th : threads) th.join();
    } else {
        remapFaces(0, faceCount);
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
    // Inline keys avoid per-face nodes while preserving first-face publication.
    FaceKeySet kept(data.faces.size());
    std::vector<Face> newFaces;
    newFaces.reserve(data.faces.size());

    // 键 = (顶点索引, UV 索引) 循环规范化 + 材质。UV 必须参与: 同一方块面上
    // 的 CTM overlay 会有多个面(顶点/材质相同, 仅 tile/UV 不同), 只按顶点
    // 去重会误删第二个 tile(表现为连接纹理缺角、缺边)。
    for (const auto& face : data.faces) {
        std::array<std::pair<int, int>, 4> pairs = {{
            { face.vertexIndices[0], face.uvIndices[0] },
            { face.vertexIndices[1], face.uvIndices[1] },
            { face.vertexIndices[2], face.uvIndices[2] },
            { face.vertexIndices[3], face.uvIndices[3] }
        }};
        // Canonicalize cyclic starts, but never reverse winding. Sorting all
        // four pairs erased front/back identity of zero-thickness foliage.
        auto canonical = pairs;
        for (int start = 1; start < 4; ++start) {
            std::array<std::pair<int, int>, 4> candidate;
            for (int k = 0; k < 4; ++k) candidate[k] = pairs[(start + k) % 4];
            if (candidate < canonical) canonical = candidate;
        }
        std::array<int, 4> sortedVerts;
        std::array<int, 4> sortedUVs;
        for (int k = 0; k < 4; ++k) {
            sortedVerts[k] = canonical[k].first;
            sortedUVs[k] = canonical[k].second;
        }
        int matIndex = config.strictDeduplication ? face.materialIndex : -1;
        if (kept.insert(FaceKey{ sortedVerts, sortedUVs, matIndex })) {
            newFaces.push_back(face);
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

    // 2. Build edge chains in contiguous face chunks, then replay occurrences
    // in face/edge order. Keep duplicates and the full non-manifold clique.
    struct EdgeKey { int v1, v2; bool operator==(const EdgeKey& o) const { return v1==o.v1 && v2==o.v2; } };
    auto t2_start = Clock::now();
    std::vector<size_t> faceAdjOffsets(faceCount + 1, 0);
    std::vector<int> faceAdj;
    {
        std::vector<std::pair<int, int>> adjacentFaces;
        adjacentFaces.reserve(faceCount * 4);
        {
            const size_t noEdge = static_cast<size_t>(-1);
            struct EdgeChain { size_t head, tail; };
            struct PartialEdges {
                struct Slot {
                    EdgeKey edge;
                    EdgeChain chain;
                    // Only head is read in an empty slot. Initialize the key
                    // and tail on insertion instead of clearing the full table.
                    Slot() noexcept { chain.head = static_cast<size_t>(-1); }
                };
                std::vector<Slot> slots;
                // Append only, in local first-occurrence order. Cross-chunk
                // linking visits each key once without scanning empty slots.
                std::vector<size_t> occupied;

                void reserve(size_t expected) {
                    size_t capacity = 16;
                    // Four occurrences per input face bound all insertions;
                    // no rehash can invalidate the occupied slot indices.
                    while (expected >= capacity - capacity / 8) {
                        if (capacity > slots.max_size() / 2)
                            throw std::length_error("edge chain table too large");
                        capacity *= 2;
                    }
                    slots.resize(capacity);
                    occupied.reserve(expected);
                }

                size_t findSlot(const EdgeKey& edge) const {
                    uint64_t hash = (static_cast<uint64_t>(static_cast<uint32_t>(edge.v1)) << 32)
                                  | static_cast<uint32_t>(edge.v2);
                    // Vertex indices have structured low bits. Avalanche both
                    // endpoints before masking into the power-of-two table.
                    hash ^= hash >> 30;
                    hash *= 0xbf58476d1ce4e5b9ULL;
                    hash ^= hash >> 27;
                    hash *= 0x94d049bb133111ebULL;
                    hash ^= hash >> 31;
                    const size_t mask = slots.size() - 1;
                    size_t pos = static_cast<size_t>(hash) & mask;
                    while (slots[pos].chain.head != static_cast<size_t>(-1) &&
                           !(slots[pos].edge == edge))
                        pos = (pos + 1) & mask;
                    return pos;
                }

                size_t prepend(const EdgeKey& edge, size_t occurrence) {
                    const size_t pos = findSlot(edge);
                    auto& slot = slots[pos];
                    const size_t previous = slot.chain.head;
                    if (previous == static_cast<size_t>(-1)) {
                        slot.edge = edge;
                        slot.chain.tail = occurrence;
                        occupied.push_back(pos);
                    }
                    slot.chain.head = occurrence;
                    return previous;
                }

                const EdgeChain* find(const EdgeKey& edge) const {
                    if (slots.empty()) return nullptr;
                    const auto& chain = slots[findSlot(edge)].chain;
                    return chain.head == static_cast<size_t>(-1) ? nullptr : &chain;
                }
            };
            const unsigned numThreads = static_cast<unsigned>(std::min<size_t>(DedupThreadCount(), faceCount));
            const size_t chunk = (faceCount + numThreads - 1) / numThreads;
            std::vector<PartialEdges> partials(numThreads);
            // Occurrence 4*i+k belongs to edge k of face i. Workers write only
            // their own chunk; the face index need not be stored separately.
            std::vector<size_t> previousOccurrence(faceCount * 4);
            auto buildPartial = [&](unsigned t) {
                const size_t start = t * chunk;
                const size_t end = std::min(start + chunk, faceCount);
                auto& chains = partials[t];
                if (start >= end) return;
                chains.reserve((end - start) * 4);
                for (size_t i = start; i < end; ++i) {
                    const auto& vs = data.faces[i].vertexIndices;
                    for (int k = 0; k < 4; ++k) {
                        const int a = vs[k], b = vs[(k + 1) % 4];
                        const EdgeKey edge{std::min(a, b), std::max(a, b)};
                        const size_t occurrence = i * 4 + k;
                        previousOccurrence[occurrence] = chains.prepend(edge, occurrence);
                    }
                }
            };
            auto runChunks = [&](auto work) {
                if (numThreads == 1) {
                    work(0);
                    return;
                }
                std::vector<std::thread> workers;
                workers.reserve(numThreads);
                for (unsigned t = 0; t < numThreads; ++t) {
                    if (t * chunk >= faceCount) break;
                    workers.emplace_back(work, t);
                }
                for (auto& worker : workers) worker.join();
            };
            runChunks(buildPartial);

            // Concatenate chains in ascending chunk (hence face) order: the
            // earliest local occurrence links to the latest earlier chunk's
            // head. Tables are immutable after build, and each worker patches
            // only its own tails, so these deterministic links can be parallel.
            // No hash-table iteration order reaches adjacency emission.
            if (numThreads > 1) runChunks([&](unsigned t) {
                for (const size_t pos : partials[t].occupied) {
                    const auto& [edge, chain] = partials[t].slots[pos];
                    for (unsigned earlier = t; earlier > 0; --earlier) {
                        if (const auto* earlierChain = partials[earlier - 1].find(edge)) {
                            previousOccurrence[chain.tail] = earlierChain->head;
                            break;
                        }
                    }
                }
            });

            // Serial replay preserves the old left-to-right face/edge walk
            // and newest-to-oldest neighbor order exactly, including repeated
            // edges on one face. Emit both directions via the offsets below.
            for (size_t occurrence = 0; occurrence < previousOccurrence.size(); ++occurrence) {
                const int face = static_cast<int>(occurrence / 4);
                for (size_t prev = previousOccurrence[occurrence]; prev != noEdge; prev = previousOccurrence[prev]) {
                    const int nb = static_cast<int>(prev / 4);
                    adjacentFaces.emplace_back(face, nb);
                    ++faceAdjOffsets[face + 1];
                    ++faceAdjOffsets[nb + 1];
                }
            }
        }
        auto t2_build_end = Clock::now();
        std::cerr << "GreedyMesh Step2 edge build: " << Ms(t2_build_end - t2_start).count() << " ms\n";

        // Compact adjacency avoids one heap allocation per face and makes the
        // serial union pass read each face's neighbors from a contiguous span.
        for (size_t i = 1; i <= faceCount; ++i) faceAdjOffsets[i] += faceAdjOffsets[i - 1];
        faceAdj.resize(faceAdjOffsets.back());
        auto nextOffset = faceAdjOffsets;
        for (const auto& [a, b] : adjacentFaces) {
            faceAdj[nextOffset[a]++] = b;
            faceAdj[nextOffset[b]++] = a;
        }
        auto t2_adj_end = Clock::now();
        std::cerr << "GreedyMesh Step2 adjacency: " << Ms(t2_adj_end - t2_build_end).count() << " ms\n";
    }
    auto t2_end = Clock::now();
    std::cerr << "GreedyMesh Step2 total: " << Ms(t2_end - t2_start).count() << " ms\n";

    // 3. DeduplicateVertices has made quantized keys unique: retain the exact
    // existing index, without sorting or binary searches during publication.
    auto t3_start = Clock::now();
    const size_t vertCount = data.vertices.size() / 3;
    VertexIndexMap vertexIndices(vertCount);
    // Prepare independent keys and cache hints before ordered insertion, as in
    // DeduplicateVertices. Publish the original vi, preserving index identity.
    constexpr size_t batchSize = 64;
    std::array<VertexKey, batchSize> keys;
    std::array<size_t, batchSize> hashes;
    for (size_t base = 0; base < vertCount; base += batchSize) {
        const size_t size = std::min(batchSize, vertCount - base);
        for (size_t j = 0; j < size; ++j) {
            const size_t vi = base + j;
            keys[j] = MakeVertexKey(data.vertices[3*vi],
                data.vertices[3*vi + 1], data.vertices[3*vi + 2]);
        }
        for (size_t j = 0; j < size; ++j)
            hashes[j] = vertexIndices.prepare(keys[j]);
        for (size_t j = 0; j < size; ++j)
            vertexIndices.tryEmplace(keys[j], static_cast<int>(base + j), hashes[j]);
    }
    auto t3_end = Clock::now();
    std::cerr << "GreedyMesh Step3 vertex hash: " << Ms(t3_end - t3_start).count() << " ms\n";

    auto lookupVertexIndex = [&](const VertexKey& vk) {
        const int newIdx = static_cast<int>(data.vertices.size() / 3);
        auto [index, inserted] = vertexIndices.tryEmplace(vk, newIdx);
        if (!inserted) return index;
        // Quantized-key misses still append reconstructed coordinates. This
        // cache is only mutated during serial, ordered result publication.
        data.vertices.push_back(static_cast<float>(vk.x) / 10000.0f);
        data.vertices.push_back(static_cast<float>(vk.y) / 10000.0f);
        data.vertices.push_back(static_cast<float>(vk.z) / 10000.0f);
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
    auto findp = [&](int x) {
        int root = x;
        while (parent[root] != root) root = parent[root];
        while (parent[x] != x) {
            const int next = parent[x];
            parent[x] = root;
            x = next;
        }
        return root;
    };
    auto unite = [&](int a, int b) {
        int pa = findp(a), pb = findp(b);
        if (pa != pb) parent[pb] = pa;
    };
    // 标记可分组面及记录UV轴
    // Byte elements permit independent writes from eligibility workers.
    std::vector<unsigned char> eligible(faceCount, false);
    std::vector<UVAxis> faceAxis(faceCount, NONE);
    // processGroup 用 (W,H) 投影的 min/max 重建矩形, 只对边平行于该法线
    // 所导出的 T1/T2 基的矩形面成立。被旋转(如 showblock ShowRotation、
    // 模型 element rotation)后在自身平面内转过一个角度的面(法线仍可能轴对齐),
    // 会被重建成包围盒 -> 拉伸的"碎片"。这类面不参与合并, 原样保留。
    auto isGridAlignedRect = [&](int fi) -> bool {
        const auto& vs = data.faces[fi].vertexIndices;
        Vector3 N = faceNormals[fi];
        Vector3 arbi = std::fabs(N.x) > std::fabs(N.z) ? Vector3{0,0,1} : Vector3{1,0,0};
        Vector3 T1 = normalize(cross(arbi, N));
        Vector3 T2 = normalize(cross(N, T1));
        for (int k = 0; k < 4; ++k) {
            Vector3 a = getVertex(vs[k]), b = getVertex(vs[(k+1)%4]);
            Vector3 d{ b.x-a.x, b.y-a.y, b.z-a.z };
            float len = std::sqrt(dot3(d,d));
            if (len < 1e-5f) return false; // 退化面
            float c1 = std::fabs(dot3(d,T1)) / len, c2 = std::fabs(dot3(d,T2)) / len;
            if (std::min(c1, c2) > 1e-3f) return false; // 边不沿 T1/T2
        }
        return true;
    };
    auto markEligible = [&](size_t start, size_t end) {
        size_t skippedRotated = 0;
        for (size_t i = start; i < end; ++i) {
            const Face& f = data.faces[i];
            if (f.materialIndex < 0 || (size_t)f.materialIndex >= data.materials.size()) continue;
            if (data.materials[f.materialIndex].type == ANIMATED) continue;
            if (!isGridAlignedRect(static_cast<int>(i))) { ++skippedRotated; continue; }
            UVAxis ax = checkUV(static_cast<int>(i));
            if (ax == NONE) continue;
            eligible[i] = true;
            faceAxis[i] = ax;
        }
        return skippedRotated;
    };
    size_t skippedRotated = 0;
    const unsigned eligibilityThreads = faceCount >= 10000 ? DedupThreadCount() : 1;
    if (eligibilityThreads > 1) {
        std::vector<size_t> skipped(eligibilityThreads);
        std::vector<std::thread> workers;
        workers.reserve(eligibilityThreads);
        const size_t chunk = (faceCount + eligibilityThreads - 1) / eligibilityThreads;
        for (unsigned t = 0; t < eligibilityThreads; ++t) {
            const size_t start = t * chunk;
            const size_t end = std::min(start + chunk, faceCount);
            if (start >= end) continue;
            workers.emplace_back([&, t, start, end] { skipped[t] = markEligible(start, end); });
        }
        for (auto& worker : workers) worker.join();
        for (size_t count : skipped) skippedRotated += count;
    } else {
        skippedRotated = markEligible(0, faceCount);
    }
    auto t5_eligible_end = Clock::now();
    // Keep adjacency traversal and parent[pb] = pa unchanged. Collection below
    // assigns groups on first encounter and keeps members in face-index order.
    // 遍历邻接表做 union
    for (int i = 0; i < (int)faceCount; ++i) {
        if (!eligible[i]) continue;
        const Face& fi = data.faces[i];
        for (size_t adj = faceAdjOffsets[i]; adj < faceAdjOffsets[i + 1]; ++adj) {
            const int nb = faceAdj[adj];
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
    auto t5_union_end = Clock::now();
    // Roots are face indices: direct lookup avoids hashing and node allocations.
    // Count in first-seen group order, then fill flat spans in face-index order.
    // Ineligible faces still create individual groups at their original turn.
    std::vector<size_t> groupOffsets;
    groupOffsets.reserve(faceCount + 1);
    groupOffsets.push_back(0);
    std::vector<int> groupMembers(faceCount);
    {
        std::vector<int> rootToGroup(faceCount, -1);
        std::vector<int> faceGroup(faceCount);
        for (int i = 0; i < (int)faceCount; ++i) {
            int group;
            if (!eligible[i]) {
                group = static_cast<int>(groupOffsets.size() - 1);
                groupOffsets.push_back(0);
            } else {
                const int root = findp(i);
                int& slot = rootToGroup[root];
                if (slot == -1) {
                    slot = static_cast<int>(groupOffsets.size() - 1);
                    groupOffsets.push_back(0);
                }
                group = slot;
            }
            faceGroup[i] = group;
            ++groupOffsets[group + 1];
        }
        for (size_t i = 1; i < groupOffsets.size(); ++i)
            groupOffsets[i] += groupOffsets[i - 1];
        auto nextOffset = groupOffsets;
        for (int i = 0; i < (int)faceCount; ++i) {
            groupMembers[nextOffset[faceGroup[i]]++] = i;
        }
    }
    const size_t groupCount = groupOffsets.size() - 1;
    auto groupAt = [&](size_t group) {
        return std::span<const int>(groupMembers.data() + groupOffsets[group],
                                    groupOffsets[group + 1] - groupOffsets[group]);
    };
    auto t5_end = Clock::now();
    std::cerr << "GreedyMesh: non-grid-aligned (rotated) faces kept unmerged: " << skippedRotated << "\n";
    std::cerr << "GreedyMesh Step5 eligibility: " << Ms(t5_eligible_end - t5_start).count() << " ms\n";
    std::cerr << "GreedyMesh Step5 union: " << Ms(t5_union_end - t5_eligible_end).count() << " ms\n";
    std::cerr << "GreedyMesh Step5 collect: " << Ms(t5_end - t5_union_end).count() << " ms\n";
    std::cerr << "GreedyMesh Step5 grouping (UF): " << Ms(t5_end - t5_start).count() << " ms\n";

    // 6. 处理每个可合并组 (串行化)
    struct MergedResult {
        std::vector<Face> faces;
        std::vector<float> uvCoords;
        std::vector<Vector3> positions;
    };
    auto processGroup = [&](std::span<const int> grp_indices)->MergedResult {
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
            // 从该面自身的顶点/UV 对应关系推出"世界 (W,H) -> UV" 的仿射映射
            // (每格步进)。合并后按世界坐标重排 UV, 天然覆盖旋转/镜像 UV,
            // 且与合并顺序无关。对所有材质启用 (不只 uvPeriodic):
            //   e.rotation 只区分 {0,90,180,270}, 镜像 UV (down 面、显式反写 uv、
            //   镜像模型如 stone_mirrored/iron_trapdoor) 会被重建成非镜像版本;
            //   仿射步进带符号, 旋转与镜像都能精确表达 (同 PR #3 f3329c6)。
            // 推导失败的退化面保持 anchorValid=false, 走下面的 rotation 回退路径。
            if (uv_valid_for_rotation) {
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

        struct BandEdgeKey {
            int rot;
            long long a, b, edge; // non-merge axis bounds and merge-axis edge
            bool operator==(const BandEdgeKey& o) const {
                return rot == o.rot && a == o.a && b == o.b && edge == o.edge;
            }
        };
        struct BandEdgeKeyHash {
            size_t operator()(const BandEdgeKey& k) const {
                size_t h = std::hash<int>{}(k.rot);
                h ^= std::hash<long long>{}(k.a) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
                h ^= std::hash<long long>{}(k.b) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
                h ^= std::hash<long long>{}(k.edge) + 0x9e3779b97f4a7c15ULL + (h<<6) + (h>>2);
                // Quantized edges have many common low bits; avalanche before
                // masking into the power-of-two inline table.
                h ^= h >> 30;
                h *= 0xbf58476d1ce4e5b9ULL;
                h ^= h >> 27;
                h *= 0x94d049bb133111ebULL;
                return h ^ (h >> 31);
            }
        };
        const size_t noNeighbor = static_cast<size_t>(-1);
        struct EdgeHeads {
            size_t min, max;
        };
        struct EdgeBucket {
            BandEdgeKey key;
            EdgeHeads heads{static_cast<size_t>(-1), static_cast<size_t>(-1)};
        };
        // At most two distinct edge keys per entry, at less than 75% load.
        // No deletion or growth is needed within a pass. Both empty heads mark
        // an unused slot, and every insertion immediately sets one of them.
        size_t edgeCapacity = 8;
        const size_t maxEdgeKeys = entries.size() * 2;
        while (maxEdgeKeys >= edgeCapacity - edgeCapacity / 4) {
            if (edgeCapacity > std::vector<EdgeBucket>().max_size() / 2)
                throw std::length_error("merge edge table too large");
            edgeCapacity *= 2;
        }
        std::vector<EdgeBucket> edgeBuckets(edgeCapacity);
        auto edgeBucketFor = [&](const BandEdgeKey& key) -> EdgeBucket& {
            size_t pos = BandEdgeKeyHash{}(key) & (edgeCapacity - 1);
            while ((edgeBuckets[pos].heads.min != noNeighbor || edgeBuckets[pos].heads.max != noNeighbor) &&
                   !(edgeBuckets[pos].key == key))
                pos = (pos + 1) & (edgeCapacity - 1);
            return edgeBuckets[pos];
        };
        // Index chains replace per-edge vectors. Retain all scratch capacity
        // across W/H passes as the entry count shrinks.
        std::vector<EdgeHeads> nextAtEdge(entries.size());
        std::vector<char> processed_mask;
        std::vector<Entry> next_entries_list;
        next_entries_list.reserve(entries.size());

        // Spatial-hash neighbor lookup for rectangle merge.
        // Same adjacency + UV rules as before, but avoid O(n^2) scans inside
        // large coplanar groups (vanilla stone/dirt/grass floors blow up there).
        auto performLimitedMergePass = [&](std::vector<Entry>& current_entries_ref, bool mergeW_axis) -> bool {
            if (current_entries_ref.size() <= 1) return false;

            bool any_merge_overall = false;
            processed_mask.assign(current_entries_ref.size(), 0);
            next_entries_list.clear();
            for (auto& bucket : edgeBuckets) bucket.heads = {noNeighbor, noNeighbor};

            // Quantize shared-axis bounds so adjacency is an exact key match.
            // eps is 1e-6; use a slightly coarser bin so float noise still hits.
            const float qScale = 1e4f; // 0.0001 world units
            auto qf = [&](float v) -> long long {
                return static_cast<long long>(std::llround(static_cast<double>(v) * qScale));
            };

            auto bandOf = [&](const Entry& e) -> BandEdgeKey {
                if (mergeW_axis) return BandEdgeKey{e.rotation, qf(e.minH), qf(e.maxH), 0};
                return BandEdgeKey{e.rotation, qf(e.minW), qf(e.maxW), 0};
            };
            auto minEdge = [&](const Entry& e) -> long long { return mergeW_axis ? qf(e.minW) : qf(e.minH); };
            auto maxEdge = [&](const Entry& e) -> long long { return mergeW_axis ? qf(e.maxW) : qf(e.maxH); };

            // Prepend in reverse order to visit each bucket in ascending entry
            // order, exactly as the previous push_back vectors did.
            for (size_t i = current_entries_ref.size(); i-- > 0;) {
                const Entry& e = current_entries_ref[i];
                BandEdgeKey key = bandOf(e);
                key.edge = minEdge(e);
                auto& minBucket = edgeBucketFor(key);
                minBucket.key = key;
                nextAtEdge[i].min = minBucket.heads.min;
                minBucket.heads.min = i;
                key.edge = maxEdge(e);
                auto& maxBucket = edgeBucketFor(key);
                maxBucket.key = key;
                nextAtEdge[i].max = maxBucket.heads.max;
                maxBucket.heads.max = i;
            }

            // 两个面合并后只用 cur 的 UV 映射重建, 所以要求 other 的 UV 在世界上与
            // cur 连续: 仿射步进相同, 且 cur 的映射在 other 锚点处给出的 UV 与
            // other 实际 UV 只差整数 (贴图 REPEAT)。显式 uv 的模型面 (铁栏杆边、
            // 活板门、Yuushya 框架等) 常常相邻但 UV 不连续, 合并会贴错纹理。
            auto uvCompatible = [&](const Entry& a, const Entry& b) -> bool {
                if (!a.anchorValid || !b.anchorValid) return true; // 退化面: 维持原行为
                const float stepEps = 1e-4f, uvEps = 2e-3f;
                if (std::fabs(a.stepW_u - b.stepW_u) > stepEps || std::fabs(a.stepW_v - b.stepW_v) > stepEps ||
                    std::fabs(a.stepH_u - b.stepH_u) > stepEps || std::fabs(a.stepH_v - b.stepH_v) > stepEps)
                    return false;
                float pu = a.anchorU + a.stepW_u * (b.anchorW - a.anchorW) + a.stepH_u * (b.anchorH - a.anchorH);
                float pv = a.anchorV + a.stepW_v * (b.anchorW - a.anchorW) + a.stepH_v * (b.anchorH - a.anchorH);
                float du = pu - b.anchorU, dv = pv - b.anchorV;
                return std::fabs(du - std::round(du)) < uvEps && std::fabs(dv - std::round(dv)) < uvEps;
            };

            // Classify adjacency without mutating: 1 = +axis (cur.max↔other.min),
            // -1 = -axis (cur.min↔other.max), 0 = no geometric match.
            auto classifyAdj = [&](const Entry& cur, const Entry& other) -> int {
                if (mergeW_axis) {
                    if (std::fabs(cur.maxW - other.minW) < eps &&
                        std::fabs(cur.minH - other.minH) < eps &&
                        std::fabs(cur.maxH - other.maxH) < eps) return 1;
                    if (std::fabs(cur.minW - other.maxW) < eps &&
                        std::fabs(cur.minH - other.minH) < eps &&
                        std::fabs(cur.maxH - other.maxH) < eps) return -1;
                } else {
                    if (std::fabs(cur.maxH - other.minH) < eps &&
                        std::fabs(cur.minW - other.minW) < eps &&
                        std::fabs(cur.maxW - other.maxW) < eps) return 1;
                    if (std::fabs(cur.minH - other.maxH) < eps &&
                        std::fabs(cur.minW - other.minW) < eps &&
                        std::fabs(cur.maxW - other.maxW) < eps) return -1;
                }
                return 0;
            };

            auto applyMerge = [&](Entry& cur, const Entry& other, int dir) {
                float deltaU = other.uMax - other.uMin;
                float deltaV = other.vMax - other.vMin;
                if (mergeW_axis) {
                    if (dir > 0) {
                        if (cur.rotation == 90 || cur.rotation == 270) cur.vMax += deltaV;
                        else cur.uMax += deltaU;
                        cur.maxW = other.maxW;
                    } else {
                        if (cur.rotation == 90 || cur.rotation == 270) cur.vMin -= deltaV;
                        else cur.uMin -= deltaU;
                        cur.minW = other.minW;
                    }
                } else {
                    if (dir > 0) {
                        if (cur.rotation == 90 || cur.rotation == 270) cur.uMax += deltaU;
                        else cur.vMax += deltaV;
                        cur.maxH = other.maxH;
                    } else {
                        if (cur.rotation == 90 || cur.rotation == 270) cur.uMin -= deltaU;
                        else cur.vMin -= deltaV;
                        cur.minH = other.minH;
                    }
                }
            };

            // Pick the lowest-index spatial neighbor (same as old rotation-group scan
            // order). Among ties at the same index path, +axis wins via if/else-if.
            auto findFirstNeighbor = [&](Entry& cur, size_t i_pass) -> size_t {
                BandEdgeKey key = bandOf(cur);
                size_t best = static_cast<size_t>(-1);
                int bestDir = 0;
                auto consider = [&](size_t head, bool minEdges) {
                    for (size_t j = head; j != noNeighbor;
                         j = minEdges ? nextAtEdge[j].min : nextAtEdge[j].max) {
                        if (j == i_pass || processed_mask[j] != 0) continue;
                        if (best != static_cast<size_t>(-1) && j >= best) continue;
                        const Entry& other = current_entries_ref[j];
                        int dir = classifyAdj(cur, other);
                        if (dir == 0) continue;
                        if (!uvCompatible(cur, other)) continue;
                        best = j;
                        bestDir = dir;
                    }
                };
                auto lookup = [&](long long edge, bool minEdges) -> size_t {
                    key.edge = edge;
                    const auto& heads = edgeBucketFor(key).heads;
                    return minEdges ? heads.min : heads.max;
                };
                consider(lookup(maxEdge(cur), true), true);
                consider(lookup(minEdge(cur), false), false);
                if (best != static_cast<size_t>(-1)) applyMerge(cur, current_entries_ref[best], bestDir);
                return best;
            };

            for (size_t i_pass = 0; i_pass < current_entries_ref.size(); ++i_pass) {
                if (processed_mask[i_pass] != 0) continue;

                Entry cur_pass = current_entries_ref[i_pass];
                processed_mask[i_pass] = 1;

                size_t j_pass = findFirstNeighbor(cur_pass, i_pass);
                if (j_pass != static_cast<size_t>(-1)) {
                    processed_mask[j_pass] = 2;
                    any_merge_overall = true;
                }

                next_entries_list.push_back(cur_pass);
            }

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

        res.faces.reserve(entries.size());
        res.positions.reserve(entries.size() * 4);
        res.uvCoords.reserve(entries.size() * 8);
        for(auto& e_final: entries){
            Face nf; nf.materialIndex=f0_group_base.materialIndex; nf.faceDirection=UNKNOWN;
            nf.tintIndex=f0_group_base.tintIndex;
            // 贪心合并出的面继承该组基底面的双面几何标记，否则标记会在合并时丢失
            // （零厚度贴片常被合并成大面，比如大块路面的 up/down）
            nf.bothSided=f0_group_base.bothSided;
            // 薄片贴边标记同理：它决定导出时是否沿法线外推，合并后丢了就会重新出现共面 z-fighting
            nf.thinEdge=f0_group_base.thinEdge;
            std::array<int,4> vidx_final;
            for(int k_final=0;k_final<4;++k_final){
                float w2d = (k_final==0||k_final==3? e_final.minW : e_final.maxW);
                float h2d = (k_final==0||k_final==1? e_final.minH : e_final.maxH);
                Vector3 pos_final{P0_group_base.x + w2d*T1_group_base.x + h2d*T2_group_base.x,
                                  P0_group_base.y + w2d*T1_group_base.y + h2d*T2_group_base.y,
                                  P0_group_base.z + w2d*T1_group_base.z + h2d*T2_group_base.z};
                {
                    // Workers only calculate geometry. Vertex cache mutation is
                    // deferred to ordered publication after all workers join.
                    res.positions.push_back(pos_final);
                    nf.vertexIndices[k_final] = 0;
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

    // Independent groups have immutable inputs. Parallelize expensive merging,
    // then publish vertices/faces/UVs in the same order as the serial algorithm.
    auto t7_start = Clock::now();
    const unsigned mergeThreads = faceCount >= 10000
        ? std::min<unsigned>(DedupThreadCount(), static_cast<unsigned>(groupCount)) : 1;
    std::vector<MergedResult> mergedGroups(mergeThreads > 1 ? groupCount : 0);
    if (mergeThreads > 1) {
        std::atomic<size_t> nextGroup{0};
        std::exception_ptr error;
        std::mutex errorMutex;
        std::vector<std::thread> workers;
        for (unsigned t = 0; t < mergeThreads; ++t) {
            workers.emplace_back([&] {
                try {
                    for (;;) {
                        const size_t i = nextGroup.fetch_add(1, std::memory_order_relaxed);
                        if (i >= groupCount) break;
                        const auto group = groupAt(i);
                        if (group.size() > 1) mergedGroups[i] = processGroup(group);
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(errorMutex);
                    if (!error) error = std::current_exception();
                }
            });
        }
        for (auto& worker : workers) worker.join();
        if (error) std::rethrow_exception(error);
    }
    {
        // 注意：不再按组大小排序，保持原顺序
        
        // 分配足够的空间
        std::vector<Face> final_model_faces;
        final_model_faces.reserve(faceCount); // 保守估计：不会比原始面数多
        
        size_t next_new_uv_start_index = data.uvCoordinates.size() / 2;
        data.uvCoordinates.reserve(data.uvCoordinates.size() + faceCount * 8); // 为每个面准备充足UV空间
        
        // 逐个处理每个组
        for (size_t groupNo = 0; groupNo < groupCount; ++groupNo) {
            const auto grp = groupAt(groupNo);
            if (grp.size() > 1) {
                MergedResult mr = mergeThreads > 1
                    ? std::move(mergedGroups[groupNo]) : processGroup(grp);
                for (size_t i = 0; i < mr.faces.size(); ++i) {
                    for (int k = 0; k < 4; ++k) {
                        const auto& pos = mr.positions[i * 4 + k];
                        mr.faces[i].vertexIndices[k] = lookupVertexIndex(MakeVertexKey(pos.x, pos.y, pos.z));
                    }
                }
                
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
        const size_t verticesBeforeGreedy = data.vertices.size();
        monitor.SetStatus(TaskStatus::GREEDY_MESHING, "GreedyMesh");
        {
            auto tg0 = Clock::now();
            GreedyMesh(data);
            auto tg1 = Clock::now();
            std::cerr << "GreedyMesh: " << Ms(tg1 - tg0).count() << " ms\n";
        }
        // Vertices are already unique here. GreedyMesh only appends vertices;
        // with no additions, another vertex deduplication would be redundant.
        if (data.vertices.size() != verticesBeforeGreedy) {
            monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices after GreedyMesh");
            auto t6 = Clock::now();
            DeduplicateVertices(data);
            auto t7 = Clock::now();
            std::cerr << "DeduplicateVertices after GreedyMesh: " << Ms(t7 - t6).count() << " ms\n";
        } else {
            std::cerr << "DeduplicateVertices after GreedyMesh: skipped (vertex count unchanged)\n";
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



