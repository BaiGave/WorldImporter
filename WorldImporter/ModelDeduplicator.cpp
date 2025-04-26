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
    
    // 查找标准UV坐标的索引 (0,0), (1,0), (1,1), (0,1)
    int uvIndex00 = -1, uvIndex10 = -1, uvIndex11 = -1, uvIndex01 = -1;
    for (size_t i = 0; i < data.uvCoordinates.size() / 2; ++i) {
        float u = data.uvCoordinates[i*2];
        float v = data.uvCoordinates[i*2+1];
        
        if (u == 0.0f && v == 0.0f) uvIndex00 = i;
        else if (u == 1.0f && v == 0.0f) uvIndex10 = i;
        else if (u == 1.0f && v == 1.0f) uvIndex11 = i;
        else if (u == 0.0f && v == 1.0f) uvIndex01 = i;
    }
    
    // 如果找不到标准UV坐标，就提前返回，不进行贪心合并
    if (uvIndex00 == -1 || uvIndex10 == -1 || uvIndex11 == -1 || uvIndex01 == -1) {
        return; // 直接结束，不进行合并
    }
    
    // 区域临时结构
    struct Region {
        FaceType dir;
        int material;
        float plane;
        int aaxis, baxis;
        float a0, a1, b0, b1;
        float u0, v0, u1, v1;
        // 添加UV旋转信息
        int uvRotation;
        std::array<int, 4> originalUVIndices; // 存储原始UV索引顺序
        bool flipX, flipY; // 增加UV翻转标记
    };
    // 提取所有区域
    std::vector<Region> regions;
    regions.reserve(data.faces.size());
    
    // 保存特殊面（DO_NOT_CULL 和 UNKNOWN）
    std::vector<Face> specialFaces;
    
    for (const auto& face : data.faces) {
        // 特殊面保存起来，不参与贪心合并
        if (face.faceDirection == DO_NOT_CULL || face.faceDirection == UNKNOWN) {
            specialFaces.push_back(face);
            continue;
        }
        
        // 直接通过UV索引判断是否是标准映射
        bool isStandardMapping = false;
        
        // 所有标准UV索引都找到了
        if (uvIndex00 != -1 && uvIndex10 != -1 && uvIndex11 != -1 && uvIndex01 != -1) {
            // 检查四个UV索引是否包含标准点的索引（不考虑顺序）
            bool has00 = false, has10 = false, has11 = false, has01 = false;
            
            for (int i = 0; i < 4; ++i) {
                int idx = face.uvIndices[i];
                if (idx == uvIndex00) has00 = true;
                else if (idx == uvIndex10) has10 = true;
                else if (idx == uvIndex11) has11 = true;
                else if (idx == uvIndex01) has01 = true;
            }
            
            // 必须包含所有四个标准UV点
            isStandardMapping = has00 && has10 && has11 && has01;
        }
        
        if (!isStandardMapping) {
            specialFaces.push_back(face);
            continue;
        }
        
        // 分析UV映射顺序，确定旋转角度
        int uvRotation = 0;
        bool flipX = false, flipY = false;
        
        // 提取UV点的位置坐标
        std::array<std::pair<float, float>, 4> uvPoints;
        for (int i = 0; i < 4; ++i) {
            int uvidx = face.uvIndices[i];
            float u = data.uvCoordinates[uvidx * 2];
            float v = data.uvCoordinates[uvidx * 2 + 1];
            uvPoints[i] = {u, v};
        }
        
        // 分析UV排列确定旋转
        // 这里使用一个更可靠的方法来检测UV旋转
        // 通过检查(0,0)点在UV序列中的位置以及相邻点的顺序
        int pos00 = -1;
        for (int i = 0; i < 4; ++i) {
            if (face.uvIndices[i] == uvIndex00) {
                pos00 = i;
                break;
            }
        }
        
        if (pos00 != -1) {
            // 检查(0,0)点后面的点是什么
            int next = (pos00 + 1) % 4;
            int uvidx = face.uvIndices[next];
            
            if (uvidx == uvIndex10) {
                uvRotation = 0; // 标准顺序
            } else if (uvidx == uvIndex01) {
                uvRotation = 90; // 逆时针旋转90度
            } else if (uvidx == uvIndex11) {
                uvRotation = 180; // 旋转180度
            } else {
                // 可能是翻转的情况，暂定旋转270度
                uvRotation = 270;
            }
            
            // 检测UV是否翻转
            // 简单的启发式方法，可能需要根据具体情况调整
            int expectedNext = -1;
            if (uvRotation == 0) expectedNext = uvIndex10;
            else if (uvRotation == 90) expectedNext = uvIndex01;
            else if (uvRotation == 180) expectedNext = uvIndex11;
            else if (uvRotation == 270) expectedNext = uvIndex10;
            
            if (uvidx != expectedNext) {
                // 可能存在翻转
                if ((uvRotation == 0 || uvRotation == 180) && face.uvIndices[next] == uvIndex01) {
                    flipX = true;
                } else if ((uvRotation == 90 || uvRotation == 270) && face.uvIndices[next] == uvIndex10) {
                    flipY = true;
                }
            }
        }
        
        std::array<std::array<float, 2>, 4> uvs;
        std::array<std::array<float,3>,4> vs;
        for (int i = 0; i < 4; ++i) {
            int vidx = face.vertexIndices[i];
            vs[i][0] = data.vertices[vidx*3];
            vs[i][1] = data.vertices[vidx*3+1];
            vs[i][2] = data.vertices[vidx*3+2];
        }
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
        
        // 添加新属性
        Region reg = {
            face.faceDirection, face.materialIndex, plane,
            aaxis, baxis, a0, a1, b0, b1, u0, v0, u1, v1,
            uvRotation, // 添加UV旋转信息
            {face.uvIndices[0], face.uvIndices[1], face.uvIndices[2], face.uvIndices[3]}, // 保存原始UV索引顺序
            flipX, flipY // 保存UV翻转标记
        };
        regions.push_back(reg);
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
        float du = 1.0f;
        float dv = 1.0f;
        for (auto& r : regs) {
            int a0i = int(r.a0), a1i = int(r.a1);
            int b0i = int(r.b0), b1i = int(r.b1);
            minA = std::fmin(minA, a0i);
            minB = std::fmin(minB, b0i);
            maxA = std::fmax(maxA, a1i);
            maxB = std::fmax(maxB, b1i);
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
        // 在每个未使用的格子上寻找最大矩形以尽可能合并
        for (int by = 0; by < height; ++by) {
            for (int ax = 0; ax < width; ++ax) {
                if (!mask[by][ax] || used[by][ax]) continue;
                int maxWidth = width - ax;
                int bestArea = 0, bestW = 0, bestH = 0;
                int minWidth = maxWidth;
                // 向下扩展行，动态维护最小宽度以计算最大面积
                for (int h = 1; by + h <= height; ++h) {
                    // 当前行可用宽度
                    int rowWidth = 0;
                    while (rowWidth < minWidth && ax + rowWidth < width
                           && mask[by + h - 1][ax + rowWidth]
                           && !used[by + h - 1][ax + rowWidth]) {
                        ++rowWidth;
                    }
                    minWidth = std::fmin(minWidth, rowWidth);
                    if (minWidth == 0) break;
                    int area = minWidth * h;
                    if (area > bestArea) {
                        bestArea = area;
                        bestW = minWidth;
                        bestH = h;
                    }
                }
                // 标记已使用区域
                for (int dy = 0; dy < bestH; ++dy) {
                    for (int dx = 0; dx < bestW; ++dx) {
                        used[by + dy][ax + dx] = true;
                    }
                }
                
                // 查找此区域包含的原始区域，确定合并区域的UV旋转
                int regionA0 = minA + ax;
                int regionA1 = regionA0 + bestW;
                int regionB0 = minB + by;
                int regionB1 = regionB0 + bestH;
                
                // 查找重叠率最高的原始区域，使用其UV旋转信息
                Region* bestMatchRegion = nullptr;
                float bestOverlap = 0.0f;
                
                for (auto& r : regs) {
                    int a0i = int(r.a0);
                    int a1i = int(r.a1);
                    int b0i = int(r.b0);
                    int b1i = int(r.b1);
                    
                    // 计算重叠区域
                    int overlapA0 = std::fmax(regionA0, a0i);
                    int overlapA1 = std::fmin(regionA1, a1i);
                    int overlapB0 = std::fmax(regionB0, b0i);
                    int overlapB1 = std::fmin(regionB1, b1i);
                    
                    if (overlapA0 < overlapA1 && overlapB0 < overlapB1) {
                        // 有重叠，计算重叠面积
                        float overlapArea = (overlapA1 - overlapA0) * (overlapB1 - overlapB0);
                        float regionArea = (a1i - a0i) * (b1i - b0i);
                        float overlapRatio = overlapArea / regionArea;
                        
                        if (overlapRatio > bestOverlap) {
                            bestOverlap = overlapRatio;
                            bestMatchRegion = &r;
                        }
                    }
                }
                
                // 生成合并区域
                Region nr = regs[0]; // 基础属性复制
                nr.a0 = float(minA + ax);
                nr.a1 = nr.a0 + bestW;
                nr.b0 = float(minB + by);
                nr.b1 = nr.b0 + bestH;
                nr.u0 = 0.0f;
                nr.u1 = du * bestW;
                nr.v0 = 0.0f;
                nr.v1 = dv * bestH;
                
                // 如果找到最佳匹配区域，使用其UV旋转信息
                if (bestMatchRegion) {
                    nr.uvRotation = bestMatchRegion->uvRotation;
                    nr.flipX = bestMatchRegion->flipX;
                    nr.flipY = bestMatchRegion->flipY;
                    // 复制原始UV索引顺序，用于后续重建UV映射
                    nr.originalUVIndices = bestMatchRegion->originalUVIndices;
                }
                
                merged.push_back(nr);
            }
        }
    }
    // 构建新网格
    ModelData newData;
    newData.materials = data.materials;
    // 用于顶点共享的哈希表
    std::unordered_map<VertexKey, int> vertexMap;
    std::unordered_map<UVKey, int> uvMap;

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
        int aw = static_cast<int>(r.a1 - r.a0);
        int bh = static_cast<int>(r.b1 - r.b0);
        
        // 基础UV映射矩形（左下、右下、右上、左上）
        std::array<std::array<float,2>,4> baseUvRect = {{
            {0.0f,      0.0f},      // 左下
            {aw * 1.0f, 0.0f},      // 右下
            {aw * 1.0f, bh * 1.0f}, // 右上
            {0.0f,      bh * 1.0f}  // 左上
        }};
        
        // 应用UV旋转和翻转
        std::array<std::array<float,2>,4> uvRect = baseUvRect;
        
        // 应用旋转
        if (r.uvRotation != 0) {
            int steps = r.uvRotation / 90;
            std::array<std::array<float,2>,4> rotatedUV = baseUvRect;
            for (int i = 0; i < 4; i++) {
                rotatedUV[i] = baseUvRect[(i + steps) % 4];
            }
            uvRect = rotatedUV;
        }
        
        // 应用翻转
        if (r.flipX) {
            std::swap(uvRect[0], uvRect[1]);
            std::swap(uvRect[3], uvRect[2]);
        }
        if (r.flipY) {
            std::swap(uvRect[0], uvRect[3]);
            std::swap(uvRect[1], uvRect[2]);
        }
        
        Face nf; nf.materialIndex = r.material; nf.faceDirection = r.dir;
        for (int i = 0; i < 4; ++i) {
            // 检查顶点是否已存在
            float x = pts[i][0], y = pts[i][1], z = pts[i][2];
            // 保留四位小数
            int rx = static_cast<int>(x * 10000 + 0.5f);
            int ry = static_cast<int>(y * 10000 + 0.5f);
            int rz = static_cast<int>(z * 10000 + 0.5f);
            VertexKey vkey{ rx, ry, rz };

            int vertexIndex;
            auto vit = vertexMap.find(vkey);
            if (vit != vertexMap.end()) {
                vertexIndex = vit->second;
            } else {
                vertexIndex = newData.vertices.size() / 3;
                vertexMap[vkey] = vertexIndex;
                newData.vertices.push_back(x);
                newData.vertices.push_back(y);
                newData.vertices.push_back(z);
            }
            nf.vertexIndices[i] = vertexIndex;

            // 检查UV是否已存在
            float u = uvRect[i][0], v = uvRect[i][1];
            // 保留六位小数
            int ru = static_cast<int>(u * 1000000 + 0.5f);
            int rv = static_cast<int>(v * 1000000 + 0.5f);
            UVKey uvkey{ ru, rv };

            int uvIndex;
            auto uvit = uvMap.find(uvkey);
            if (uvit != uvMap.end()) {
                uvIndex = uvit->second;
            } else {
                uvIndex = newData.uvCoordinates.size() / 2;
                uvMap[uvkey] = uvIndex;
                newData.uvCoordinates.push_back(u);
                newData.uvCoordinates.push_back(v);
            }
            nf.uvIndices[i] = uvIndex;
        }
        newData.faces.push_back(nf);
    }
    // 保留 DO_NOT_CULL 和 UNKNOWN 方位的面，避免丢失无法合并的植物面
    for (const auto& face : specialFaces) {
        Face nf = face;
        for (int i = 0; i < 4; ++i) {
            int vidx = face.vertexIndices[i];
            float x = data.vertices[vidx*3];
            float y = data.vertices[vidx*3+1];
            float z = data.vertices[vidx*3+2];
            // 保留四位小数
            int rx = static_cast<int>(x * 10000 + 0.5f);
            int ry = static_cast<int>(y * 10000 + 0.5f);
            int rz = static_cast<int>(z * 10000 + 0.5f);
            VertexKey vkey{ rx, ry, rz };
            
            int vertexIndex;
            auto vit = vertexMap.find(vkey);
            if (vit != vertexMap.end()) {
                vertexIndex = vit->second;
            } else {
                vertexIndex = newData.vertices.size() / 3;
                vertexMap[vkey] = vertexIndex;
                newData.vertices.push_back(x);
                newData.vertices.push_back(y);
                newData.vertices.push_back(z);
            }
            nf.vertexIndices[i] = vertexIndex;
            
            int uvidx = face.uvIndices[i];
            float u = data.uvCoordinates[uvidx*2];
            float v = data.uvCoordinates[uvidx*2+1];
            
            // 对于特殊方向面保持原始UV映射
            // 注意：这里不限制UV在0-1范围内，允许贴图重复
            
            // 保留六位小数
            int ru = static_cast<int>(u * 1000000 + 0.5f);
            int rv = static_cast<int>(v * 1000000 + 0.5f);
            UVKey uvkey{ ru, rv };
            
            int uvIndex;
            auto uvit = uvMap.find(uvkey);
            if (uvit != uvMap.end()) {
                uvIndex = uvit->second;
            } else {
                uvIndex = newData.uvCoordinates.size() / 2;
                uvMap[uvkey] = uvIndex;
                newData.uvCoordinates.push_back(u);
                newData.uvCoordinates.push_back(v);
            }
            nf.uvIndices[i] = uvIndex;
        }
        newData.faces.push_back(nf);
    }
    // 替换原数据
    data.vertices = std::move(newData.vertices);
    data.uvCoordinates = std::move(newData.uvCoordinates);
    data.faces = std::move(newData.faces);
    data.materials = std::move(newData.materials);
}