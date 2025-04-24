// ModelDeduplicator.cpp
#include "ModelDeduplicator.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <climits>

void ModelDeduplicator::DeduplicateVertices(ModelData& data) {
    std::unordered_map<VertexKey, int> vertexMap;
    // 预先分配容量，避免多次rehash
    vertexMap.reserve(data.vertices.size() / 3);
    std::vector<float> newVertices;
    newVertices.reserve(data.vertices.size());
    std::vector<int> indexMap(data.vertices.size() / 3);

    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        float x = data.vertices[i];
        float y = data.vertices[i + 1];
        float z = data.vertices[i + 2];
        // 保留四位小数（转为整数后再比较）
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
    // 如果没有 UV 坐标，则直接返回
    if (model.uvCoordinates.empty()) {
        return;
    }

    // 使用哈希表记录每个唯一 UV 对应的新索引
    std::unordered_map<UVKey, int> uvMap;
    std::vector<float> newUV;  // 存储去重后的 UV 坐标（每两个元素构成一组）
    // 原始 UV 数组中组的数量（每组有2个元素：u,v）
    int uvCount = model.uvCoordinates.size() / 2;
    // 建立一个映射表，从旧的 UV 索引到新的 UV 索引
    std::vector<int> indexMapping(uvCount, -1);

    for (int i = 0; i < uvCount; i++) {
        float u = model.uvCoordinates[i * 2];
        float v = model.uvCoordinates[i * 2 + 1];
        // 将浮点数转换为整数，保留小数点后6位的精度
        int iu = static_cast<int>(std::round(u * 1000000));
        int iv = static_cast<int>(std::round(v * 1000000));
        UVKey key = { iu, iv };

        auto it = uvMap.find(key);
        if (it == uvMap.end()) {
            // 如果没有找到，则是新 UV，记录新的索引
            int newIndex = newUV.size() / 2;
            uvMap[key] = newIndex;
            newUV.push_back(u);
            newUV.push_back(v);
            indexMapping[i] = newIndex;
        }
        else {
            // 如果已存在，则记录已有的新索引
            indexMapping[i] = it->second;
        }
    }

    // 如果 uvFaces 不为空，则更新 uvFaces 中的索引
    if (!model.faces.empty()) {
        for (auto& face : model.faces) {
            for (auto& idx : face.uvIndices) {
                // 注意：这里假设 uvIndices 中的索引都在有效范围内
                idx = indexMapping[idx];
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

    // 第一次遍历：计算每个面的规范化键并存入数组（避免重复排序）
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

    // 第二次遍历：过滤只出现一次的面
    std::vector<Face> newFaces;
    newFaces.reserve(data.faces.size());

    for (size_t i = 0; i < keys.size(); i++) {
        if (freq[keys[i]] == 1) {
            newFaces.push_back(data.faces[i]);
        }
    }

    data.faces.swap(newFaces);
}

// Greedy mesh 算法：合并相邻同材质、相同方向的面以减少面数
void ModelDeduplicator::GreedyMesh(ModelData& data) {
    if (data.faces.empty()) return;
    // 区域临时结构
    struct Region {
        FaceType dir;
        int material;
        float plane;
        int aaxis, baxis;
        float a0, a1, b0, b1;
        float u0, v0, u1, v1;
    };
    // 提取所有区域
    std::vector<Region> regions;
    regions.reserve(data.faces.size());
    for (const auto& face : data.faces) {
        std::array<std::array<float,3>,4> vs;
        for (int i = 0; i < 4; ++i) {
            int vidx = face.vertexIndices[i];
            vs[i][0] = data.vertices[vidx*3];
            vs[i][1] = data.vertices[vidx*3+1];
            vs[i][2] = data.vertices[vidx*3+2];
        }
        std::array<std::array<float,2>,4> uvs;
        for (int i = 0; i < 4; ++i) {
            int uvidx = face.uvIndices[i];
            uvs[i][0] = data.uvCoordinates[uvidx*2];
            uvs[i][1] = data.uvCoordinates[uvidx*2+1];
        }
        int daxis, aaxis, baxis;
        switch(face.faceDirection) {
            case UP:    daxis=1; aaxis=0; baxis=2; break;
            case DOWN:  daxis=1; aaxis=0; baxis=2; break;
            case NORTH: daxis=2; aaxis=0; baxis=1; break;
            case SOUTH: daxis=2; aaxis=0; baxis=1; break;
            case WEST:  daxis=0; aaxis=2; baxis=1; break;
            case EAST:  daxis=0; aaxis=2; baxis=1; break;
            default: continue;
        }
        float plane = vs[0][daxis];
        float a0 = vs[0][aaxis], a1 = a0;
        float b0 = vs[0][baxis], b1 = b0;
        for (int i = 1; i < 4; ++i) {
            a0 = std::fmin(a0, vs[i][aaxis]);
            a1 = std::fmax(a1, vs[i][aaxis]);
            b0 = std::fmin(b0, vs[i][baxis]);
            b1 = std::fmax(b1, vs[i][baxis]);
        }
        float u0 = uvs[0][0], u1 = u0;
        float v0 = uvs[0][1], v1 = v0;
        for (int i = 1; i < 4; ++i) {
            u0 = std::fmin(u0, uvs[i][0]);
            u1 = std::fmax(u1, uvs[i][0]);
            v0 = std::fmin(v0, uvs[i][1]);
            v1 = std::fmax(v1, uvs[i][1]);
        }
        regions.push_back({face.faceDirection, face.materialIndex, plane,
                           aaxis, baxis, a0, a1, b0, b1, u0, v0, u1, v1});
    }
    // 分组
    std::map<std::tuple<FaceType, int, float>, std::vector<Region>> groups;
    for (auto& r : regions) {
        groups[{r.dir, r.material, r.plane}].push_back(r);
    }
    // 合并
    std::vector<Region> merged;
    // 二维贪心合并：最大化沿 a 轴和 b 轴方向的矩形合并
    for (auto& kv : groups) {
        auto& regs = kv.second;
        if (regs.empty()) continue;
        // 计算 a、b 轴范围
        int minA = INT_MAX, minB = INT_MAX, maxA = INT_MIN, maxB = INT_MIN;
        float du = regs[0].u1 - regs[0].u0;
        float dv = regs[0].v1 - regs[0].v0;
        for (auto& r : regs) {
            int a0i = int(r.a0), a1i = int(r.a1);
            int b0i = int(r.b0), b1i = int(r.b1);
            minA = std::min(minA, a0i);
            minB = std::min(minB, b0i);
            maxA = std::max(maxA, a1i);
            maxB = std::max(maxB, b1i);
        }
        int width = maxA - minA;
        int height = maxB - minB;
        // 构建占位图
        std::vector<std::vector<bool>> mask(height, std::vector<bool>(width, false));
        for (auto& r : regs) {
            int a0i = int(r.a0) - minA;
            int b0i = int(r.b0) - minB;
            int w = int(r.a1 - r.a0);
            int h = int(r.b1 - r.b0);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    mask[b0i + y][a0i + x] = true;
        }
        std::vector<std::vector<bool>> used(height, std::vector<bool>(width, false));
        // 扫描出最大矩形
        for (int by = 0; by < height; ++by) {
            for (int ax = 0; ax < width; ++ax) {
                if (!mask[by][ax] || used[by][ax]) continue;
                // 横向扩展宽度
                int w = 1;
                while (ax + w < width && mask[by][ax + w] && !used[by][ax + w]) ++w;
                // 纵向扩展高度
                int h = 1;
                bool ok = true;
                while (by + h < height && ok) {
                    for (int xx = 0; xx < w; ++xx) {
                        if (!mask[by + h][ax + xx] || used[by + h][ax + xx]) { ok = false; break; }
                    }
                    if (ok) ++h;
                }
                // 标记已使用
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                        used[by + y][ax + x] = true;
                // 生成合并区域
                Region nr = regs[0];
                nr.a0 = float(minA + ax);
                nr.a1 = nr.a0 + w;
                nr.b0 = float(minB + by);
                nr.b1 = nr.b0 + h;
                nr.u0 = regs[0].u0 + du * ax;
                nr.u1 = nr.u0 + du * w;
                nr.v0 = regs[0].v0 + dv * by;
                nr.v1 = nr.v0 + dv * h;
                merged.push_back(nr);
            }
        }
    }
    // 构建新网格
    ModelData newData;
    newData.materials = data.materials;
    for (auto& r : merged) {
        int daxis, aaxis, baxis;
        switch (r.dir) {
            case UP:    daxis=1; aaxis=0; baxis=2; break;
            case DOWN:  daxis=1; aaxis=0; baxis=2; break;
            case NORTH: daxis=2; aaxis=0; baxis=1; break;
            case SOUTH: daxis=2; aaxis=0; baxis=1; break;
            case WEST:  daxis=0; aaxis=2; baxis=1; break;
            case EAST:  daxis=0; aaxis=2; baxis=1; break;
            default: continue;
        }
        std::array<std::array<float,3>,4> pts;
        // 按下左、下右、上右、上左顺序设置顶点
        auto setPt = [&](int idx, float a, float b) {
            pts[idx][daxis] = r.plane;
            pts[idx][aaxis] = a;
            pts[idx][baxis] = b;
        };
        setPt(0, r.a0, r.b0);
        setPt(1, r.a1, r.b0);
        setPt(2, r.a1, r.b1);
        setPt(3, r.a0, r.b1);
        // 计算合并区域的 UV 重复范围，避免贴图拉伸
        float du = r.u1 - r.u0;
        float dv = r.v1 - r.v0;
        float aw = r.a1 - r.a0; // 区域宽度（世界单位）
        float bh = r.b1 - r.b0; // 区域高度（世界单位）
        std::array<std::array<float,2>,4> uvRect = {{
            {r.u0,                  r.v0},
            {r.u0 + du * aw,        r.v0},
            {r.u0 + du * aw,        r.v0 + dv * bh},
            {r.u0,                  r.v0 + dv * bh}
        }};
        int baseV = newData.vertices.size() / 3;
        int baseUV = newData.uvCoordinates.size() / 2;
        Face nf; nf.materialIndex = r.material; nf.faceDirection = r.dir;
        for (int i = 0; i < 4; ++i) {
            newData.vertices.push_back(pts[i][0]);
            newData.vertices.push_back(pts[i][1]);
            newData.vertices.push_back(pts[i][2]);
            nf.vertexIndices[i] = baseV + i;
            newData.uvCoordinates.push_back(uvRect[i][0]);
            newData.uvCoordinates.push_back(uvRect[i][1]);
            nf.uvIndices[i] = baseUV + i;
        }
        newData.faces.push_back(nf);
    }
    // 保留 DO_NOT_CULL 和 UNKNOWN 方位的面，避免丢失无法合并的植物面
    for (const auto& face : data.faces) {
        if (face.faceDirection == DO_NOT_CULL || face.faceDirection == UNKNOWN) {
            Face nf = face;
            int baseV = newData.vertices.size() / 3;
            int baseUV = newData.uvCoordinates.size() / 2;
            for (int i = 0; i < 4; ++i) {
                int vidx = face.vertexIndices[i];
                newData.vertices.push_back(data.vertices[vidx*3]);
                newData.vertices.push_back(data.vertices[vidx*3+1]);
                newData.vertices.push_back(data.vertices[vidx*3+2]);
                nf.vertexIndices[i] = baseV + i;
                int uvidx = face.uvIndices[i];
                newData.uvCoordinates.push_back(data.uvCoordinates[uvidx*2]);
                newData.uvCoordinates.push_back(data.uvCoordinates[uvidx*2+1]);
                nf.uvIndices[i] = baseUV + i;
            }
            newData.faces.push_back(nf);
        }
    }
    // 替换原数据
    data.vertices = std::move(newData.vertices);
    data.uvCoordinates = std::move(newData.uvCoordinates);
    data.faces = std::move(newData.faces);
    data.materials = std::move(newData.materials);
}