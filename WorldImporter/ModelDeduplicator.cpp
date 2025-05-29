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
    std::unordered_map<VertexKey, int> vertexMap;
    // 预先分配容量,避免多次rehash
    vertexMap.reserve(data.vertices.size() / 3);
    std::vector<float> newVertices;
    newVertices.reserve(data.vertices.size());
    std::vector<int> indexMap(data.vertices.size() / 3);

    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        float x = data.vertices[i];
        float y = data.vertices[i + 1];
        float z = data.vertices[i + 2];
        // 保留四位小数(转为整数后再比较)
        int rx = static_cast<int>(x * 10000 + 0.5f);
        int ry = static_cast<int>(y * 10000 + 0.5f);
        int rz = static_cast<int>(z * 10000 + 0.5f);
        VertexKey key{ rx, ry, rz };

        auto it = vertexMap.find(key);
        if (it != vertexMap.end()) {
            indexMap[i / 3] = it->second;
        }
        else {
            int newIndex = newVertices.size() / 3;
            vertexMap[key] = newIndex;
            newVertices.push_back(x);
            newVertices.push_back(y);
            newVertices.push_back(z);
            indexMap[i / 3] = newIndex;
        }
    }

    data.vertices = std::move(newVertices);

    // 更新面数据中的顶点索引
    for (auto& face : data.faces) {
        for (auto& idx : face.vertexIndices) {
            idx = indexMap[idx];
        }
    }
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
    // 步骤1：并行计算所有面的法向量，用于判断面方向是否一致
    // 步骤2：并行构建边到面映射，便于查找相邻面
    // 步骤3：构建顶点坐标到索引的映射，用于合并后查找原始顶点
    // 步骤4：定义UV连续性检查函数，判断面在UV贴图上是水平还是垂直
    // 步骤5：并行将所有面按材质、法线、UV连续性分组，同组面可合并
    // 步骤6：并行对每组调用 processGroup，执行贪心合并
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

    unsigned int num_threads = std::max(1u, std::thread::hardware_concurrency()); // 获取硬件线程数，至少为1

    // 1. 计算所有面的法向量 (并行化)
    std::vector<Vector3> faceNormals(faceCount);
    auto calculate_normals_task = 
        [&](size_t start_idx, size_t end_idx) {
        for (size_t i = start_idx; i < end_idx; ++i) {
            const auto& vs = data.faces[i].vertexIndices;
            Vector3 p0 = getVertex(vs[0]);
            Vector3 p1 = getVertex(vs[1]);
            Vector3 p2 = getVertex(vs[2]);
            Vector3 e1{ p1.x-p0.x, p1.y-p0.y, p1.z-p0.z };
            Vector3 e2{ p2.x-p0.x, p2.y-p0.y, p2.z-p0.z };
            faceNormals[i] = normalize(cross(e1, e2)); // 计算并存储法向量
        }
    };
    {
        std::vector<std::future<void>> futures;
        size_t chunk_size_normals = (faceCount + num_threads - 1) / num_threads;
        for (unsigned int t = 0; t < num_threads; ++t) {
            size_t start = t * chunk_size_normals;
            size_t end = std::min((t + 1) * chunk_size_normals, faceCount);
            if (start < end) {
                futures.push_back(std::async(std::launch::async, calculate_normals_task, start, end));
            }
        }
        for (auto& fut : futures) fut.get();
    }

    // 2. 构建边->面映射 (并行化 Map-Reduce)
    struct EdgeKey { int v1, v2; bool operator==(const EdgeKey& o) const { return v1==o.v1 && v2==o.v2; } };
    struct EdgeKeyHasher { size_t operator()(const EdgeKey& e) const {
            return std::hash<long long>()(((long long)std::min(e.v1,e.v2)<<32) ^ (unsigned long long)std::max(e.v1,e.v2)); // 确保哈希一致性
        }
    };
    std::unordered_map<EdgeKey, std::vector<int>, EdgeKeyHasher> edgeFaces;
    {
        std::vector<std::vector<std::pair<EdgeKey, int>>> thread_edge_pairs(num_threads);
        auto build_edge_pairs_task = 
            [&](size_t thread_id, size_t start_idx, size_t end_idx) {
            for (size_t i = start_idx; i < end_idx; ++i) {
                const auto& vs = data.faces[i].vertexIndices;
                for (int k = 0; k < 4; ++k) {
                    int a = vs[k], b = vs[(k+1)%4];
                    EdgeKey e{ std::min(a,b), std::max(a,b) }; // 规范化边，确保v1<=v2
                    thread_edge_pairs[thread_id].emplace_back(e, (int)i);
                }
            }
        };
        std::vector<std::future<void>> futures;
        size_t chunk_size_edges = (faceCount + num_threads - 1) / num_threads;
        for (unsigned int t = 0; t < num_threads; ++t) {
            size_t start = t * chunk_size_edges;
            size_t end = std::min((t + 1) * chunk_size_edges, faceCount);
            if (start < end) {
                futures.push_back(std::async(std::launch::async, build_edge_pairs_task, t, start, end));
            }
        }
        for (auto& fut : futures) fut.get();

        // Reduce step (串行汇总)
        for (const auto& pairs_vec : thread_edge_pairs) {
            for (const auto& pair : pairs_vec) {
                edgeFaces[pair.first].push_back(pair.second);
            }
        }
    }

    // 3. 构建顶点键->索引映射（用于合并后查顶点，暂时保持串行）
    std::unordered_map<VertexKey,int> vertMap;
    int vertCount = data.vertices.size()/3;
    for (int i = 0; i < vertCount; ++i) {
        int rx = static_cast<int>(data.vertices[3*i]*10000 + 0.5f);
        int ry = static_cast<int>(data.vertices[3*i+1]*10000 + 0.5f);
        int rz = static_cast<int>(data.vertices[3*i+2]*10000 + 0.5f);
        vertMap[{rx, ry, rz}] = i;
    }

    // 4. UV 连续性检查 (Lambda定义)
    enum UVAxis { NONE=0, HORIZONTAL=1, VERTICAL=2 };
    auto checkUV = [&](int fi){
        const auto& uvs_indices = data.faces[fi].uvIndices;
        int cntTop=0, cntBottom=0, cntLeft=0, cntRight=0;
        for (int j=0;j<4;++j) {
            // 检查uvIndices是否有效
            if (uvs_indices[j] < 0 || (uvs_indices[j] * 2 + 1) >= data.uvCoordinates.size()) {
                 // std::cerr << "Warning: Invalid UV index " << uvs_indices[j] << " for face " << fi << std::endl;
                 return NONE; // 无效UV索引，无法判断连续性
            }
            float u = data.uvCoordinates[2*uvs_indices[j]];
            float v = data.uvCoordinates[2*uvs_indices[j]+1];
            if (std::fabs(v-1.0f)<eps) ++cntTop;
            if (std::fabs(v)<eps) ++cntBottom;
            if (std::fabs(u)<eps) ++cntLeft;
            if (std::fabs(u-1.0f)<eps) ++cntRight;
        }
        if (cntTop==2 && cntBottom==2) return VERTICAL;   // 上下边贴合UV边界
        if (cntLeft==2 && cntRight==2) return HORIZONTAL; // 左右边贴合UV边界
        return NONE;
    };

    // 5. 分组（排除动态材质，按法线/材质/UV轴一致性） (并行化)
    std::vector<std::atomic_bool> visited_atomic(faceCount);
    for(size_t i = 0; i < faceCount; ++i) visited_atomic[i].store(false, std::memory_order_relaxed);
    
    std::vector<std::vector<int>> all_groups_collected; // 用于收集所有线程发现的组
    std::mutex all_groups_mutex;       // 保护all_groups_collected的互斥锁

    auto discover_groups_task = 
        [&](size_t start_idx, size_t end_idx) {
        std::vector<std::vector<int>> local_thread_groups; // 每个线程的局部组列表
        for (size_t i = start_idx; i < end_idx; ++i) {
            bool expected_visited = false;
            // 尝试原子地标记当前面为已访问，如果成功，则以此面为种子开始BFS建组
            if (!visited_atomic[i].compare_exchange_strong(expected_visited, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                continue; // 如果已经被其他线程标记或自身已处理，则跳过
            }

            const Face& f0 = data.faces[i];
            MaterialType mt = data.materials[f0.materialIndex].type;
            if (mt == ANIMATED) { // 动态材质不参与合并，单独成组
                local_thread_groups.push_back({(int)i});
                continue;
            }
            UVAxis axis0 = checkUV(i);
            if (axis0 == NONE) { // 不满足UV连续性条件的面，单独成组
                local_thread_groups.push_back({(int)i});
                continue;
            }
            Vector3 n0 = faceNormals[i]; //  当前组的基准法向量

            std::vector<int> current_bfs_group;
            std::queue<int> q;
            
            current_bfs_group.push_back((int)i);
            q.push((int)i);

            while (!q.empty()) {
                int cur = q.front();
                q.pop();
                const auto& vs = data.faces[cur].vertexIndices;
                for (int k_edge = 0; k_edge < 4; ++k_edge) {
                    EdgeKey ek{std::min(vs[k_edge], vs[(k_edge + 1) % 4]), std::max(vs[k_edge], vs[(k_edge + 1) % 4])};
                    auto it_edge = edgeFaces.find(ek);
                    if (it_edge == edgeFaces.end()) continue;

                    for (int nb_face_idx : it_edge->second) {
                        // 检查邻接面是否满足合并条件
                        const Face& fn_check = data.faces[nb_face_idx];
                        if (fn_check.materialIndex != f0.materialIndex) continue; // 材质必须相同
                        
                        Vector3 dn_check{faceNormals[nb_face_idx].x - n0.x, faceNormals[nb_face_idx].y - n0.y, faceNormals[nb_face_idx].z - n0.z};
                        if (std::sqrt(dn_check.x*dn_check.x + dn_check.y*dn_check.y + dn_check.z*dn_check.z) > eps) continue; // 法线必须相同
                        if (checkUV(nb_face_idx) != axis0) continue; // UV连续性类型必须相同

                        bool expected_nb_visited = false;
                        // 原子地标记合格的邻接面为已访问，并加入当前组的BFS队列
                        if (visited_atomic[nb_face_idx].compare_exchange_strong(expected_nb_visited, true, std::memory_order_acq_rel, std::memory_order_relaxed)) {
                            current_bfs_group.push_back(nb_face_idx);
                            q.push(nb_face_idx);
                        }
                    }
                }
            }
            if (!current_bfs_group.empty()) {
                local_thread_groups.push_back(std::move(current_bfs_group));
            }
        }
        // 将当前线程发现的组合并到全局组列表中（受互斥锁保护）
        if (!local_thread_groups.empty()) {
            std::lock_guard<std::mutex> lock(all_groups_mutex);
            for (auto& lg : local_thread_groups) {
                all_groups_collected.push_back(std::move(lg));
            }
        }
    };
    {
        std::vector<std::future<void>> futures_grouping;
        size_t chunk_size_grouping = (faceCount + num_threads - 1) / num_threads;
        for (unsigned int t = 0; t < num_threads; ++t) {
            size_t start = t * chunk_size_grouping;
            size_t end = std::min((t + 1) * chunk_size_grouping, faceCount);
            if (start < end) {
                futures_grouping.push_back(std::async(std::launch::async, discover_groups_task, start, end));
            }
        }
        for (auto& fut : futures_grouping) fut.get();
    }
    // 替换原有 groups 变量
    std::vector<std::vector<int>>& groups = all_groups_collected;

    // 6. 并行处理每个可合并组 (此部分逻辑来自您之前的版本，保持不变)
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
            e.originalFaceIndex = fi_original_idx; // 保存原始索引用于后续UV旋转计算

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
                     e.uMin=0; e.uMax=0; e.vMin=0; e.vMax=0; // 处理无效UV的情况
                     break; // 如果一个UV无效，整个面的UV范围可能无意义
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
            if (!uv_valid_for_rotation) { e.rotation = 0; /* 默认旋转或错误处理 */ }
            else {
                Vector2 dWx_rot{pts_proj[1].x-pts_proj[0].x,pts_proj[1].y-pts_proj[0].y}, dWy_rot{pts_proj[3].x-pts_proj[0].x,pts_proj[3].y-pts_proj[0].y};
                Vector2 dUx_rot{uvs_rot_calc[1].x-uvs_rot_calc[0].x,uvs_rot_calc[1].y-uvs_rot_calc[0].y}, dUy_rot{uvs_rot_calc[3].x-uvs_rot_calc[0].x,uvs_rot_calc[3].y-uvs_rot_calc[0].y};
                auto dot2_rot=[&](Vector2 a,Vector2 b){return a.x*b.x+a.y*b.y;};
                float t0_rot=dot2_rot(dWx_rot,dUx_rot)+dot2_rot(dWy_rot,dUy_rot), t90_rot=dot2_rot(dWx_rot,dUy_rot)-dot2_rot(dWy_rot,dUx_rot);
                float t180_rot=-t0_rot, t270_rot=-t90_rot;
                if(t0_rot>=t90_rot&&t0_rot>=t180_rot&&t0_rot>=t270_rot) e.rotation=0;
                else if(t90_rot>=t0_rot&&t90_rot>=t180_rot&&t90_rot>=t270_rot) e.rotation=90;
                else if(t180_rot>=t0_rot&&t180_rot>=t90_rot&&t180_rot>=t270_rot) e.rotation=180;
                else e.rotation=270;
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
            // 将未被消耗(processed_mask[k]==0)且未作为基础(processed_mask[k]!=1)的条目也加入next_entries_list
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
                vidx_final[k_final]=vertMap[{rx_final,ry_final,rz_final}];
                nf.vertexIndices[k_final]=vidx_final[k_final];
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

    // 7. 异步执行各组合并 (保持不变，但使用上面修改后的 groups 变量)
    std::vector<std::future<MergedResult>> futures;
    for(auto& grp_item: groups) { // 使用 all_groups_collected (现在是 groups 的引用)
        if(grp_item.size()>1) { // 只为包含多个面的组创建任务
            futures.push_back(std::async(std::launch::async, processGroup, grp_item));
        } else if (grp_item.size() == 1) { // 单面组直接加入结果，无需processGroup
            // This logic will be handled later when collecting results.
        }
    }

    // 8. 收集合并结果并更新 data
    std::vector<Face> newFaces;
    size_t current_uv_offset = data.uvCoordinates.size() / 2;
    data.uvCoordinates.reserve(data.uvCoordinates.size() + faceCount * 8); // 预估UV增长
    newFaces.reserve(faceCount); // 预估面数

    for(auto& f_future: futures){
        MergedResult mr = f_future.get();
        for(const auto& face_res : mr.faces) {
            newFaces.push_back(face_res); // 添加合并后的面
        }
        // 为新面添加对应的UV坐标，并更新其uvIndices
        size_t faces_in_mr = mr.faces.size();
        for(size_t i_mr_face=0; i_mr_face < faces_in_mr; ++i_mr_face) {
            Face& added_face = newFaces[newFaces.size() - faces_in_mr + i_mr_face];
            for(int k_uv_idx=0; k_uv_idx<4; ++k_uv_idx) {
                 added_face.uvIndices[k_uv_idx] = current_uv_offset + (i_mr_face * 4) + k_uv_idx;
            }
        }
        // 添加UV数据到总的uvCoordinates
        data.uvCoordinates.insert(data.uvCoordinates.end(), mr.uvCoords.begin(), mr.uvCoords.end());
        current_uv_offset += mr.uvCoords.size() / 2;
    }
    // 9. 保留单面组 (之前未通过 processGroup 处理的组)
    for(auto& grp_single: groups) { 
        if(grp_single.size()==1) {
            newFaces.push_back(data.faces[grp_single[0]]);
            // 注意: 单面组的UV索引不需要改变，因为它们直接来自原始data.faces
            // 并且其UV数据已经在data.uvCoordinates中。
            // 但如果后续需要所有UV都来自新的 MergedResult 结构，则这里也需要调整。
            // 当前假设：单面组的UV坐标和索引保持原样。
        }
    }
    data.faces.swap(newFaces);
}

// 综合去重和优化方法
void ModelDeduplicator::DeduplicateModel(ModelData& data) {
    auto& monitor = GetTaskMonitor();
    
    monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices");
    DeduplicateVertices(data);
    
    monitor.SetStatus(TaskStatus::DEDUPLICATING_UV, "DeduplicateUV");
    DeduplicateUV(data);
    
    monitor.SetStatus(TaskStatus::DEDUPLICATING_FACES, "DeduplicateFaces");
    DeduplicateFaces(data);
    
    if (config.useGreedyMesh) {
        monitor.SetStatus(TaskStatus::GREEDY_MESHING, "GreedyMesh");
        GreedyMesh(data);
        monitor.SetStatus(TaskStatus::DEDUPLICATING_VERTICES, "DeduplicateVertices after GreedyMesh");
        DeduplicateVertices(data);
        monitor.SetStatus(TaskStatus::DEDUPLICATING_UV, "DeduplicateUV after GreedyMesh");
        DeduplicateUV(data);
    }
}


