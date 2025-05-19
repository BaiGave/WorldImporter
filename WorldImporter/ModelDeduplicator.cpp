// ModelDeduplicator.cpp
#include "ModelDeduplicator.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>
#include <climits>
#include <sstream>
#include <stack>
#include <string>
#include <set>

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

// 变换一个UV坐标点
std::pair<float, float> transformPoint(const Matrix2x2& matrix, float u, float v, float centerU, float centerV) {
    // 移到原点
    float relU = u - centerU;
    float relV = v - centerV;
    
    // 应用矩阵变换
    float newU = matrix.m[0][0] * relU + matrix.m[0][1] * relV;
    float newV = matrix.m[1][0] * relU + matrix.m[1][1] * relV;
    
    // 移回中心点
    return {centerU + newU, centerV + newV};
}

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
    if (data.faces.empty()) return;

    //==========================================================================
    // 第1步:初始化和标准UV坐标查找
    //==========================================================================
    // 查找标准UV坐标的索引 (0,0), (1,0), (1,1), (0,1),这些是合并的基础
    int uvIndex00 = -1, uvIndex10 = -1, uvIndex11 = -1, uvIndex01 = -1;
    for (size_t i = 0; i < data.uvCoordinates.size() / 2; ++i) {
        float u = data.uvCoordinates[i * 2];
        float v = data.uvCoordinates[i * 2 + 1];

        if (u == 0.0f && v == 0.0f) uvIndex00 = i;
        else if (u == 1.0f && v == 0.0f) uvIndex10 = i;
        else if (u == 1.0f && v == 1.0f) uvIndex11 = i;
        else if (u == 0.0f && v == 1.0f) uvIndex01 = i;
    }

    // 如果找不到标准UV坐标,就提前返回,不进行贪心合并
    if (uvIndex00 == -1 || uvIndex10 == -1 || uvIndex11 == -1 || uvIndex01 == -1) {
        return; // 直接结束,不进行合并
    }

    //==========================================================================
    // 第2步:定义数据结构
    //==========================================================================
    // 区域临时结构:用于表示可合并的矩形区域
    struct Region {
        FaceType dir;          // 面的方向
        int material;          // 面的材质索引
        float plane;           // 面所在平面的位置
        int aaxis, baxis;      // 面在平面上的两个轴
        float a0, a1, b0, b1;  // 面在这两个轴上的范围
        float u0, v0, u1, v1;  // UV坐标范围
        std::array<int, 4> vertexOrder;  // 顶点索引顺序
        int originalFaceIndex; // 添加原始面的索引以便追踪
    };
    
    // 用于统计顶点顺序的映射
    std::map<std::array<int, 4>, int> vertexOrderCount;
    
    // 用于存储提取的区域和特殊面
    std::vector<Region> regions;
    regions.reserve(data.faces.size());
    std::vector<Face> specialFaces;
    
    // 跟踪哪些面被合并了(使用索引,而不是指针)
    std::vector<bool> faceMerged(data.faces.size(), false);

    //==========================================================================
    // 第3步:面分类与区域提取
    //==========================================================================
    // 遍历所有面,提取可合并的区域,将特殊面单独保存
    for (size_t faceIdx = 0; faceIdx < data.faces.size(); ++faceIdx) {
        const auto& face = data.faces[faceIdx];
        // 特殊面保存起来,不参与贪心合并
        if (face.faceDirection == DO_NOT_CULL || face.faceDirection == UNKNOWN) {
            specialFaces.push_back(face);
            continue;
        }
        
        // 检查是否为动态材质或特殊材质,如果是则跳过合并
        if (face.materialIndex >= 0 && face.materialIndex < data.materials.size()) {
            const Material& material = data.materials[face.materialIndex];
            if (material.type == ANIMATED) {
                specialFaces.push_back(face);
                continue;
            }
        }

        // 判断是否使用标准UV映射(必须包含所有四个标准点)
        bool isStandardMapping = false;
        if (uvIndex00 != -1 && uvIndex10 != -1 && uvIndex11 != -1 && uvIndex01 != -1) {
            bool has00 = false, has10 = false, has11 = false, has01 = false;
            for (int i = 0; i < 4; ++i) {
                int idx = face.uvIndices[i];
                if (idx == uvIndex00) has00 = true;
                else if (idx == uvIndex10) has10 = true;
                else if (idx == uvIndex11) has11 = true;
                else if (idx == uvIndex01) has01 = true;
            }
            isStandardMapping = has00 && has10 && has11 && has01;
        }

        // 非标准UV映射的面放入特殊面列表
        if (!isStandardMapping) {
            specialFaces.push_back(face);
            continue;
        }

        //----------------------------------------------------------------------
        // 提取顶点和UV坐标
        //----------------------------------------------------------------------
        // 获取面的四个顶点坐标
        std::array<std::array<float, 3>, 4> vs;
        for (int i = 0; i < 4; ++i) {
            int vidx = face.vertexIndices[i];
            vs[i][0] = data.vertices[vidx * 3];
            vs[i][1] = data.vertices[vidx * 3 + 1];
            vs[i][2] = data.vertices[vidx * 3 + 2];
        }
        
        // 获取面的四个UV坐标
        std::array<std::array<float, 2>, 4> uvs;
        for (int i = 0; i < 4; ++i) {
            int uvidx = face.uvIndices[i];
            uvs[i][0] = data.uvCoordinates[uvidx * 2];
            uvs[i][1] = data.uvCoordinates[uvidx * 2 + 1];
        }

        //----------------------------------------------------------------------
        // 确定面的坐标轴和边界
        //----------------------------------------------------------------------
        // 根据面的方向确定对应的坐标轴
        int daxis, aaxis, baxis;
        switch (face.faceDirection) {
        case UP:    daxis = 1; aaxis = 0; baxis = 2; break; // Y是深度,X和Z是面的轴
        case DOWN:  daxis = 1; aaxis = 0; baxis = 2; break;
        case NORTH: daxis = 2; aaxis = 0; baxis = 1; break; // Z是深度,X和Y是面的轴
        case SOUTH: daxis = 2; aaxis = 0; baxis = 1; break;
        case WEST:  daxis = 0; aaxis = 2; baxis = 1; break; // X是深度,Z和Y是面的轴
        case EAST:  daxis = 0; aaxis = 2; baxis = 1; break;
        default: continue; // 其他方向的面跳过
        }

        // 确定面在3D空间中的平面位置
        float plane = vs[0][daxis];
        
        //----------------------------------------------------------------------
        // 计算顶点顺序
        //----------------------------------------------------------------------
        // 找到最小坐标点作为参考点(0, 0)
        int minIdx = 0;
        float minA = vs[0][aaxis];
        float minB = vs[0][baxis];

        for (int i = 1; i < 4; ++i) {
            if (vs[i][aaxis] < minA || (vs[i][aaxis] == minA && vs[i][baxis] < minB)) {
                minA = vs[i][aaxis];
                minB = vs[i][baxis];
                minIdx = i;
            }
        }

        // 计算顶点相对于最小点的位置,这样每个1x1的面都有标准化的顶点位置
        std::array<std::pair<float, float>, 4> relativePos;
        for (int i = 0; i < 4; ++i) {
            relativePos[i] = std::make_pair(
                vs[i][aaxis] - minA,
                vs[i][baxis] - minB
            );
        }

        // 确定顶点的顺序索引,用于统计分析
        std::array<int, 4> vertexOrder;
        for (int i = 0; i < 4; ++i) {
            if (std::abs(relativePos[i].first) < 0.01f && std::abs(relativePos[i].second) < 0.01f) {
                vertexOrder[0] = i; // 左下角 (0, 0)
            } else if (std::abs(relativePos[i].first - 1.0f) < 0.01f && std::abs(relativePos[i].second) < 0.01f) {
                vertexOrder[1] = i; // 右下角 (1, 0)
            } else if (std::abs(relativePos[i].first - 1.0f) < 0.01f && std::abs(relativePos[i].second - 1.0f) < 0.01f) {
                vertexOrder[2] = i; // 右上角 (1, 1)
            } else if (std::abs(relativePos[i].first) < 0.01f && std::abs(relativePos[i].second - 1.0f) < 0.01f) {
                vertexOrder[3] = i; // 左上角 (0, 1)
            }
        }

        // 记录并统计顶点顺序
        vertexOrderCount[vertexOrder]++;
        
        // 计算面在两个轴上的边界范围
        float a0 = vs[0][aaxis], a1 = a0;
        float b0 = vs[0][baxis], b1 = b0;
        for (int i = 1; i < 4; ++i) {
            a0 = std::fmin(a0, vs[i][aaxis]);
            a1 = std::fmax(a1, vs[i][aaxis]);
            b0 = std::fmin(b0, vs[i][baxis]);
            b1 = std::fmax(b1, vs[i][baxis]);
        }
        
        // 计算UV坐标的边界范围
        float u0 = uvs[0][0], u1 = u0;
        float v0 = uvs[0][1], v1 = v0;
        for (int i = 1; i < 4; ++i) {
            u0 = std::fmin(u0, uvs[i][0]);
            u1 = std::fmax(u1, uvs[i][0]);
            v0 = std::fmin(v0, uvs[i][1]);
            v1 = std::fmax(v1, uvs[i][1]);
        }

        // 创建区域对象并添加到列表
        Region reg = {
            face.faceDirection, face.materialIndex, plane,
            aaxis, baxis, a0, a1, b0, b1, u0, v0, u1, v1,
            vertexOrder,
            static_cast<int>(faceIdx)  // 保存原始面的索引
        };
        regions.push_back(reg);
    }

    //==========================================================================
    // 第4步:区域分组
    //==========================================================================
    // 按照面的方向、材质、平面位置和顶点顺序进行分组
    std::map<std::tuple<FaceType, int, float, std::array<int, 4>>, std::vector<Region>> groups;
    
    // 使用哈希集合跟踪已处理的原始面索引,提高查找效率
    std::unordered_set<int> processedFaces;
    processedFaces.reserve(data.faces.size());
    
    for (auto& r : regions) {
        // 使用方向、材质、平面和顶点顺序作为键
        groups[{r.dir, r.material, r.plane, r.vertexOrder}].push_back(r);
    }

    // 处理所有孤立的单个面:即一个分组中只有一个面的情况
    for (auto it = groups.begin(); it != groups.end();) {
        if (it->second.size() == 1) {
            // 将这个孤立面添加到特殊面列表
            int originalFaceIdx = it->second[0].originalFaceIndex;
            specialFaces.push_back(data.faces[originalFaceIdx]);
            
            // 标记为已处理
            processedFaces.insert(originalFaceIdx);
            
            // 从groups中移除并更新迭代器
            it = groups.erase(it);
        } else {
            ++it;
        }
    }

    //==========================================================================
    // 第5步:贪心合并
    //==========================================================================
    std::vector<Region> merged;
    merged.reserve(groups.size()); // 预分配内存减少重新分配
    
    // 对每个分组执行贪心合并算法
    for (auto& kv : groups) {
        auto& regs = kv.second;
        if (regs.empty()) continue;
        
        //----------------------------------------------------------------------
        // 计算分组的边界框
        //----------------------------------------------------------------------
        int minA = INT_MAX, minB = INT_MAX;
        int maxA = INT_MIN, maxB = INT_MIN;
        float du = 1.0f;  // UV坐标映射比例
        float dv = 1.0f;
        
        // 计算所有区域的最大边界
        for (auto& r : regs) {
            int a0i = int(r.a0), a1i = int(r.a1);
            int b0i = int(r.b0), b1i = int(r.b1);
            minA = std::fmin(minA, a0i);
            minB = std::fmin(minB, b0i);
            maxA = std::fmax(maxA, a1i);
            maxB = std::fmax(maxB, b1i);
        }
        
        int width = maxA - minA;   // 分组在A轴上的总宽度
        int height = maxB - minB;  // 分组在B轴上的总高度
        
        // 如果区域太大,可能导致内存问题,跳过处理
        if (width > 1000 || height > 1000) {
            for (auto& r : regs) {
                specialFaces.push_back(data.faces[r.originalFaceIndex]);
                processedFaces.insert(r.originalFaceIndex);
            }
            continue;
        }
        
        //----------------------------------------------------------------------
        // 构建占位图和使用标记图
        //----------------------------------------------------------------------
        // 创建占位图,标记每个格子是否被区域占用
        std::vector<std::vector<bool>> mask(height, std::vector<bool>(width, false));
        // 创建占位图对应的面索引
        std::vector<std::vector<int>> cellToRegionIdx(height, std::vector<int>(width, -1));
        
        for (size_t regIdx = 0; regIdx < regs.size(); ++regIdx) {
            auto& r = regs[regIdx];
            int a0i = int(r.a0) - minA;
            int b0i = int(r.b0) - minB;
            int w = int(r.a1 - r.a0);
            int h = int(r.b1 - r.b0);
            // 标记此区域占用的所有格子,并记录对应的原始区域索引
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    mask[b0i + y][a0i + x] = true;
                    cellToRegionIdx[b0i + y][a0i + x] = regIdx;
                }
            }
        }
        
        // 创建标记图,记录哪些格子已被合并处理
        std::vector<std::vector<bool>> used(height, std::vector<bool>(width, false));
        
        //----------------------------------------------------------------------
        // 优化的贪心合并算法:寻找最大矩形
        //----------------------------------------------------------------------
        
        // 结构体用于存储可能的合并矩形
        struct PotentialRectangle {
            int x, y;       // 左上角坐标
            int width, height; // 宽度和高度
            int area;          // 面积
            
            bool operator<(const PotentialRectangle& other) const {
                return area > other.area; // 按面积降序排列
            }
        };
        
        // 计算每个位置可以向右扩展的宽度
        std::vector<std::vector<int>> rightExtension(height, std::vector<int>(width, 0));
        for (int y = 0; y < height; ++y) {
            for (int x = width - 1; x >= 0; --x) {
                if (mask[y][x] && !used[y][x]) {
                    if (x == width - 1) {
                        rightExtension[y][x] = 1;
                    } else {
                        rightExtension[y][x] = rightExtension[y][x + 1] + 1;
                    }
                }
            }
        }
        
        // 当还有未使用的格子时,继续寻找最大矩形
        while (true) {
            std::vector<PotentialRectangle> potentialRects;
            
            // 尝试从每个可用格子开始,找出可能的最大矩形
            for (int startY = 0; startY < height; ++startY) {
                for (int startX = 0; startX < width; ++startX) {
                    if (!mask[startY][startX] || used[startY][startX]) continue;
                    
                    // 该位置可以向右扩展的最大宽度
                    int maxWidth = rightExtension[startY][startX];
                    if (maxWidth == 0) continue;
                    
                    // 向下扩展寻找最大矩形
                    int currentHeight = 1;
                    int currentWidth = maxWidth;
                    
                    for (int h = 1; startY + h < height; ++h) {
                        // 检查下一行的可用宽度
                        if (!mask[startY + h][startX] || used[startY + h][startX]) break;
                        
                        // 更新当前行可以扩展的最大宽度
                        currentWidth = std::fmin(currentWidth, rightExtension[startY + h][startX]);
                        if (currentWidth == 0) break;
                        
                        // 计算当前矩形的面积
                        int area = currentWidth * (h + 1);
                        currentHeight = h + 1;
                        
                        // 添加到潜在矩形列表
                        potentialRects.push_back({startX, startY, currentWidth, currentHeight, area});
                    }
                    
                    // 如果只有一行,也计算面积
                    if (currentHeight == 1) {
                        potentialRects.push_back({startX, startY, maxWidth, 1, maxWidth});
                    }
                }
            }
            
            // 如果没有找到可合并的矩形,则结束循环
            if (potentialRects.empty()) break;
            
            // 按面积降序排序,选择最大面积的矩形
            std::sort(potentialRects.begin(), potentialRects.end());
            const auto& bestRect = potentialRects[0];
            
            // 如果最大矩形只有1x1大小,检查是否是孤立单元格
            if (bestRect.width == 1 && bestRect.height == 1) {
                // 将这个孤立的单元格标记为已使用
                used[bestRect.y][bestRect.x] = true;
                
                // 获取原始面索引并添加到特殊面列表
                int regIdx = cellToRegionIdx[bestRect.y][bestRect.x];
                if (regIdx >= 0) {
                    int originalFaceIdx = regs[regIdx].originalFaceIndex;
                    if (processedFaces.find(originalFaceIdx) == processedFaces.end()) {
                        specialFaces.push_back(data.faces[originalFaceIdx]);
                        processedFaces.insert(originalFaceIdx);
                    }
                }
                continue;
            }
            
            // 标记矩形内的所有格子为已使用
            for (int dy = 0; dy < bestRect.height; ++dy) {
                for (int dx = 0; dx < bestRect.width; ++dx) {
                    int y = bestRect.y + dy;
                    int x = bestRect.x + dx;
                    used[y][x] = true;
                    
                    // 记录参与合并的原始区域
                    int regIdx = cellToRegionIdx[y][x];
                    if (regIdx >= 0) {
                        int originalFaceIdx = regs[regIdx].originalFaceIndex;
                        processedFaces.insert(originalFaceIdx);
                    }
                }
            }
            
            // 计算合并后矩形的世界坐标
            int regionA0 = minA + bestRect.x;
            int regionA1 = regionA0 + bestRect.width;
            int regionB0 = minB + bestRect.y;
            int regionB1 = regionB0 + bestRect.height;
            
            // 查找与合并区域重叠最多的原始区域,继承其UV属性
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
                
                // 检查是否有重叠
                if (overlapA0 < overlapA1 && overlapB0 < overlapB1) {
                    // 计算重叠比例
                    float overlapArea = (overlapA1 - overlapA0) * (overlapB1 - overlapB0);
                    float regionArea = (a1i - a0i) * (b1i - b0i);
                    float overlapRatio = overlapArea / regionArea;
                    
                    // 更新最佳匹配
                    if (overlapRatio > bestOverlap) {
                        bestOverlap = overlapRatio;
                        bestMatchRegion = &r;
                    }
                }
            }
            
            // 创建合并区域
            Region nr = regs[0];  // 基础属性复制
            nr.a0 = float(regionA0);
            nr.a1 = float(regionA1);
            nr.b0 = float(regionB0);
            nr.b1 = float(regionB1);
            nr.u0 = 0.0f;
            nr.u1 = du * bestRect.width;
            nr.v0 = 0.0f;
            nr.v1 = dv * bestRect.height;
            nr.originalFaceIndex = -1; // 合并区域不对应原始面
            
            // 如果有最佳匹配区域,继承其顶点顺序
            if (bestMatchRegion) {
                nr.vertexOrder = bestMatchRegion->vertexOrder;
            }
            
            merged.push_back(nr);
            
            // 更新右扩展数组,因为有些格子已经被使用了
            for (int y = 0; y < height; ++y) {
                for (int x = width - 1; x >= 0; --x) {
                    if (mask[y][x] && !used[y][x]) {
                        if (x == width - 1) {
                            rightExtension[y][x] = 1;
                        } else if (used[y][x + 1]) {
                            rightExtension[y][x] = 1;
                        } else {
                            rightExtension[y][x] = rightExtension[y][x + 1] + 1;
                        }
                    } else {
                        rightExtension[y][x] = 0;
                    }
                }
            }
        }
        
        // 处理没有参与合并的区域
        for (size_t i = 0; i < regs.size(); ++i) {
            int originalFaceIdx = regs[i].originalFaceIndex;
            if (processedFaces.find(originalFaceIdx) == processedFaces.end()) {
                specialFaces.push_back(data.faces[originalFaceIdx]);
                processedFaces.insert(originalFaceIdx);
            }
        }
    }

    // 检查是否有未处理的原始面,将它们添加到特殊面
    for (size_t i = 0; i < regions.size(); ++i) {
        int originalFaceIdx = regions[i].originalFaceIndex;
        if (processedFaces.find(originalFaceIdx) == processedFaces.end()) {
            specialFaces.push_back(data.faces[originalFaceIdx]);
        }
    }

    //==========================================================================
    // 第6步:重建网格
    //==========================================================================
    ModelData newData;
    newData.materials = data.materials;
    
    // 用于顶点和UV坐标去重的哈希表
    std::unordered_map<VertexKey, int> vertexMap;
    std::unordered_map<UVKey, int> uvMap;

    //----------------------------------------------------------------------
    // 处理合并后的区域,生成新的面
    //----------------------------------------------------------------------
    for (auto& r : merged) {
        // 根据面的方向确定坐标轴
        int daxis, aaxis, baxis;
        switch (r.dir) {
        case UP:    daxis = 1; aaxis = 0; baxis = 2; break;
        case DOWN:  daxis = 1; aaxis = 0; baxis = 2; break;
        case NORTH: daxis = 2; aaxis = 0; baxis = 1; break;
        case SOUTH: daxis = 2; aaxis = 0; baxis = 1; break;
        case WEST:  daxis = 0; aaxis = 2; baxis = 1; break;
        case EAST:  daxis = 0; aaxis = 2; baxis = 1; break;
        default: continue;
        }
        
        // 计算合并区域的四个顶点坐标(按顺序:左下、右下、右上、左上)
        std::array<std::array<float, 3>, 4> pts;
        auto setPt = [&](int idx, float a, float b) {
            pts[idx][daxis] = r.plane;
            pts[idx][aaxis] = a;
            pts[idx][baxis] = b;
        };
        
        // 根据面的方向设置不同的顶点顺序
        switch (r.dir) {
        case UP:
            // 对于UP方向的面,反转顶点顺序以确保法向量方向正确
            setPt(0, r.a0, r.b1);  // 左上
            setPt(1, r.a1, r.b1);  // 右上
            setPt(2, r.a1, r.b0);  // 右下
            setPt(3, r.a0, r.b0);  // 左下
            break;
        case DOWN:
            // DOWN方向保持原来的顺序
            setPt(0, r.a0, r.b0);  // 左下
            setPt(1, r.a1, r.b0);  // 右下
            setPt(2, r.a1, r.b1);  // 右上
            setPt(3, r.a0, r.b1);  // 左上
            break;
        case NORTH:
            // NORTH方向法线反向,需要反转顶点顺序
            setPt(0, r.a0, r.b1);  // 左上
            setPt(1, r.a1, r.b1);  // 右上
            setPt(2, r.a1, r.b0);  // 右下
            setPt(3, r.a0, r.b0);  // 左下
            break;
        case SOUTH:
            // SOUTH方向保持原来的顺序
            setPt(0, r.a0, r.b0);  // 左下
            setPt(1, r.a1, r.b0);  // 右下
            setPt(2, r.a1, r.b1);  // 右上
            setPt(3, r.a0, r.b1);  // 左上
            break;
        case EAST:
            // EAST方向法线反向,需要反转顶点顺序
            setPt(0, r.a0, r.b1);  // 左上
            setPt(1, r.a1, r.b1);  // 右上
            setPt(2, r.a1, r.b0);  // 右下
            setPt(3, r.a0, r.b0);  // 左下
            break;
        case WEST:
            // WEST方向保持原来的顺序
            setPt(0, r.a0, r.b0);  // 左下
            setPt(1, r.a1, r.b0);  // 右下
            setPt(2, r.a1, r.b1);  // 右上
            setPt(3, r.a0, r.b1);  // 左上
            break;
        default:
            // 其他方向保持原来的顺序
            setPt(0, r.a0, r.b0);  // 左下
            setPt(1, r.a1, r.b0);  // 右下
            setPt(2, r.a1, r.b1);  // 右上
            setPt(3, r.a0, r.b1);  // 左上
        }
        
        // 计算合并区域的UV映射尺寸
        int aw = static_cast<int>(r.a1 - r.a0);
        int bh = static_cast<int>(r.b1 - r.b0);

        // 创建基础UV映射矩形
        std::array<std::array<float, 2>, 4> baseUvRect;
        
        // 为不同方向使用不同的UV排列
        if (r.dir == NORTH || r.dir == EAST) {
            // 为反向法线的面使用反转的UV坐标,保持贴图方向正确
            baseUvRect = { {
                {0.0f,      bh * 1.0f},  // 左上 - 对应0
                {aw * 1.0f, bh * 1.0f},  // 右上 - 对应1
                {aw * 1.0f, 0.0f},       // 右下 - 对应2
                {0.0f,      0.0f}        // 左下 - 对应3
            } };
        } else {
            // 其他方向使用标准UV排列
            baseUvRect = { {
                {0.0f,      0.0f},       // 左下 - 对应0
                {aw * 1.0f, 0.0f},       // 右下 - 对应1
                {aw * 1.0f, bh * 1.0f},  // 右上 - 对应2
                {0.0f,      bh * 1.0f}   // 左上 - 对应3
            } };
        }

        // 应用UV变换(旋转和翻转)
        std::array<std::array<float, 2>, 4> uvRect = baseUvRect;
        
        // 计算UV矩形的中心点
        float centerU = (baseUvRect[0][0] + baseUvRect[2][0]) / 2.0f;
        float centerV = (baseUvRect[0][1] + baseUvRect[2][1]) / 2.0f;

        // 根据顶点索引顺序进行UV旋转
        // 构造顶点顺序字符串,避免八进制数字问题
        std::string vertexOrderStr = 
            std::to_string(r.vertexOrder[0]) + 
            std::to_string(r.vertexOrder[1]) + 
            std::to_string(r.vertexOrder[2]) + 
            std::to_string(r.vertexOrder[3]);
        
        // 创建变换矩阵
        Matrix2x2 transformMatrix = Matrix2x2::identity();
        
        if (vertexOrderStr == "0123") { // [0,1,2,3] - 标准顺序,不需要旋转
            //transformMatrix = Matrix2x2::rotation(90.0f);
            // 保持单位矩阵
        }
        else if (vertexOrderStr == "0321") { // [0,3,2,1] - 垂直翻转(绕水平轴)
            //transformMatrix = Matrix2x2::rotation(270.0f);
        }
        else if (vertexOrderStr == "1032") { // [1,0,3,2] - 水平翻转(绕垂直轴)
            transformMatrix = Matrix2x2::rotation(270.0f);
        }
        else if (vertexOrderStr == "1230") { // [1,2,3,0] - 顺时针旋转270度
            //transformMatrix = Matrix2x2::rotation(70.0f);
        }
        else if (vertexOrderStr == "2103") { // [2,1,0,3] - 顺时针旋转180度
            //transformMatrix = Matrix2x2::rotation(270.0f);
        }
        else if (vertexOrderStr == "2301") { // [2,3,0,1] - 顺时针旋转90度
           // transformMatrix = Matrix2x2::rotation(90.0f);
        }
        else if (vertexOrderStr == "3012") { // [3,0,1,2] - 对角线翻转(主对角线)
            // 主对角线翻转相当于先旋转90度再水平翻转
            //transformMatrix = Matrix2x2::rotation(180.0f);
        }
        else if (vertexOrderStr == "3210") { // [3,2,1,0] - 对角线翻转(副对角线)
            // 副对角线翻转相当于先旋转90度再垂直翻转
            transformMatrix = Matrix2x2::rotation(90.0f) ;
        }
        
        // 应用变换矩阵到每个UV坐标
        for (int i = 0; i < 4; ++i) {
            auto [newU, newV] = transformPoint(transformMatrix, baseUvRect[i][0], baseUvRect[i][1], centerU, centerV);
            uvRect[i][0] = newU;
            uvRect[i][1] = newV;
        }
        
        // 找出变换后的左下角点的坐标(四个点中u和v值最小的点)
        float minU = uvRect[0][0];
        float minV = uvRect[0][1];
        for (int i = 1; i < 4; ++i) {
            minU = std::fmin(minU, uvRect[i][0]);
            minV = std::fmin(minV, uvRect[i][1]);
        }
        
        // 将所有点偏移到原点(减去最小u,v值)
        for (int i = 0; i < 4; ++i) {
            uvRect[i][0] -= minU;
            uvRect[i][1] -= minV;
        }
        
        // 创建新面
        Face nf;
        nf.materialIndex = r.material;
        nf.faceDirection = r.dir;
        
        // 添加面的顶点和UV坐标
        for (int i = 0; i < 4; ++i) {
            // 添加顶点,利用哈希表去重
            float x = pts[i][0], y = pts[i][1], z = pts[i][2];
            int rx = static_cast<int>(x * 10000 + 0.5f);  // 保留四位小数
            int ry = static_cast<int>(y * 10000 + 0.5f);
            int rz = static_cast<int>(z * 10000 + 0.5f);
            VertexKey vkey{ rx, ry, rz };

            int vertexIndex;
            auto vit = vertexMap.find(vkey);
            if (vit != vertexMap.end()) {
                vertexIndex = vit->second;  // 使用已存在的顶点
            }
            else {
                vertexIndex = newData.vertices.size() / 3;  // 创建新顶点
                vertexMap[vkey] = vertexIndex;
                newData.vertices.push_back(x);
                newData.vertices.push_back(y);
                newData.vertices.push_back(z);
            }
            nf.vertexIndices[i] = vertexIndex;

            // 添加UV坐标,利用哈希表去重
            float u = uvRect[i][0], v = uvRect[i][1];
            int ru = static_cast<int>(u * 1000000 + 0.5f);  // 保留六位小数
            int rv = static_cast<int>(v * 1000000 + 0.5f);
            UVKey uvkey{ ru, rv };

            int uvIndex;
            auto uvit = uvMap.find(uvkey);
            if (uvit != uvMap.end()) {
                uvIndex = uvit->second;  // 使用已存在的UV坐标
            }
            else {
                uvIndex = newData.uvCoordinates.size() / 2;  // 创建新UV坐标
                uvMap[uvkey] = uvIndex;
                newData.uvCoordinates.push_back(u);
                newData.uvCoordinates.push_back(v);
            }
            nf.uvIndices[i] = uvIndex;
        }
        newData.faces.push_back(nf);
    }

    //==========================================================================
    // 第7步:处理特殊面
    //==========================================================================
    // 将之前保存的特殊面(不符合合并条件或无法合并的面)添加回模型
    for (const auto& face : specialFaces) {
        // 直接使用原始特殊面的数据,只将顶点和UV索引转换为新模型中的索引
        Face nf = face;
        
        // 处理特殊面的顶点
        for (int i = 0; i < 4; ++i) {
            int vidx = face.vertexIndices[i];
            float x = data.vertices[vidx * 3];
            float y = data.vertices[vidx * 3 + 1];
            float z = data.vertices[vidx * 3 + 2];
            
            // 尝试查找或添加顶点
            int rx = static_cast<int>(x * 10000 + 0.5f);
            int ry = static_cast<int>(y * 10000 + 0.5f);
            int rz = static_cast<int>(z * 10000 + 0.5f);
            VertexKey vkey{ rx, ry, rz };

            int vertexIndex;
            auto vit = vertexMap.find(vkey);
            if (vit != vertexMap.end()) {
                vertexIndex = vit->second;
            }
            else {
                vertexIndex = newData.vertices.size() / 3;
                vertexMap[vkey] = vertexIndex;
                newData.vertices.push_back(x);
                newData.vertices.push_back(y);
                newData.vertices.push_back(z);
            }
            nf.vertexIndices[i] = vertexIndex;

            // 处理特殊面的UV坐标
            int uvidx = face.uvIndices[i];
            float u = data.uvCoordinates[uvidx * 2];
            float v = data.uvCoordinates[uvidx * 2 + 1];
            
            // 添加UV坐标,利用哈希表去重
            int ru = static_cast<int>(u * 1000000 + 0.5f);
            int rv = static_cast<int>(v * 1000000 + 0.5f);
            UVKey uvkey{ ru, rv };

            int uvIndex;
            auto uvit = uvMap.find(uvkey);
            if (uvit != uvMap.end()) {
                uvIndex = uvit->second;
            }
            else {
                uvIndex = newData.uvCoordinates.size() / 2;
                uvMap[uvkey] = uvIndex;
                newData.uvCoordinates.push_back(u);
                newData.uvCoordinates.push_back(v);
            }
            nf.uvIndices[i] = uvIndex;
        }
        
        newData.faces.push_back(nf);
    }

    //==========================================================================
    // 第8步:更新原始数据
    //==========================================================================
    // 用新生成的数据替换原始数据
    data.vertices = std::move(newData.vertices);
    data.uvCoordinates = std::move(newData.uvCoordinates);
    data.faces = std::move(newData.faces);
    data.materials = std::move(newData.materials);
}

// 综合去重和优化方法
void ModelDeduplicator::DeduplicateModel(ModelData& data) {
    DeduplicateVertices(data);
    DeduplicateUV(data);
    DeduplicateFaces(data);
    if (config.useGreedyMesh) {
        GreedyMesh(data);
    }
}