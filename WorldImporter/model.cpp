#include "model.h"
#include "fileutils.h"
#include "SpecialBlock.h"
#include "HangingSignModel.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <unordered_map>
// #include <omp.h>
#include <chrono>
#include <span>
#include <cmath>

using namespace std::chrono;  


//---------------- 几何变换 ----------------
// 应用缩放(以0.5,0.5,0.5为中心)
void ApplyScaleToVertices(std::span<float> vertices, float sx, float sy, float sz) {
    constexpr float center = 0.5f;
    for (size_t i = 0; i < vertices.size(); i += 3) {
        // 平移至中心点相对坐标
        float x = vertices[i] - center;
        float y = vertices[i + 1] - center;
        float z = vertices[i + 2] - center;

        // 应用缩放
        x *= sx;
        y *= sy;
        z *= sz;

        // 平移回原坐标系
        vertices[i] = x + center;
        vertices[i + 1] = y + center;
        vertices[i + 2] = z + center;
    }
}
// 应用旋转(以0.5,0.5,0.5为中心,按X->Y->Z轴顺序)
void ApplyRotationToVertices(std::span<float> vertices, float rx, float ry, float rz) {
    constexpr float center = 0.5f;

    // 转换角度为弧度(按原始值直接使用,若需/16则改为 rx/16.0f)
    const float radX = rx * (M_PI / 180.0f);
    const float radY = ry * (M_PI / 180.0f);
    const float radZ = rz * (M_PI / 180.0f);

    // 预计算三角函数值
    const float cosX = cos(radX), sinX = sin(radX);
    const float cosY = cos(radY), sinY = sin(radY);
    const float cosZ = cos(radZ), sinZ = sin(radZ);

    for (size_t i = 0; i < vertices.size(); i += 3) {
        // 平移至中心点相对坐标
        float x = vertices[i] - center;
        float y = vertices[i + 1] - center;
        float z = vertices[i + 2] - center;

        // 按X轴旋转(与element处理逻辑一致)
        if (rx != 0.0f) {
            float new_y = y * cosX - z * sinX;
            float new_z = y * sinX + z * cosX;
            y = new_y;
            z = new_z;
        }

        // 按Y轴旋转(与element处理逻辑一致)
        if (ry != 0.0f) {
            float new_x = x * cosY + z * sinY;
            float new_z = -x * sinY + z * cosY;
            x = new_x;
            z = new_z;
        }

        // 按Z轴旋转(与element处理逻辑一致)
        if (rz != 0.0f) {
            float new_x = x * cosZ - y * sinZ;
            float new_y = x * sinZ + y * cosZ;
            x = new_x;
            y = new_y;
        }

        // 平移回原坐标系
        vertices[i] = x + center;
        vertices[i + 1] = y + center;
        vertices[i + 2] = z + center;
    }
}


// 旋转函数 - 使用整数参数的版本,C++20版本
void ApplyRotationToVertices(std::span<float> vertices, int rotationX, int rotationY) {
    // 参数校验
    if (vertices.size() % 3 != 0) {
        throw std::invalid_argument("Invalid vertex data size");
    }

    // 绕 X 轴旋转(90度增量)
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        // 将坐标平移到以 (0.5, 0.5) 为中心
        y -= 0.5f;
        z -= 0.5f;

        switch (rotationX) {
        case 90:
            std::tie(y, z) = std::make_pair(z, -y);
            break;
        case 180:
            y = -y;
            z = -z;
            break;
        case 270:
            std::tie(y, z) = std::make_pair(-z, y);
            break;
        default:
            break;
        }

        // 将坐标平移回原位置
        y += 0.5f;
        z += 0.5f;
    }

    // 绕 Y 轴旋转(90度增量)
    for (size_t i = 0; i < vertices.size(); i += 3) {
        float& x = vertices[i];
        float& y = vertices[i + 1];
        float& z = vertices[i + 2];

        // 将坐标平移到以 (0.5, 0.5) 为中心
        x -= 0.5f;
        z -= 0.5f;

        switch (rotationY) {
        case 90:
            std::tie(x, z) = std::make_pair(-z, x);
            break;
        case 180:
            x = -x;
            z = -z;
            break;
        case 270:
            std::tie(x, z) = std::make_pair(z, -x);
            break;
        default:
            break;
        }

        // 将坐标平移回原位置
        x += 0.5f;
        z += 0.5f;
    }
}

// 带旋转中心的UV旋转(内联优化)
static inline void fastRotateUV(float& u, float& v, float cosA, float sinA) {
    constexpr float centerU = 0.5f;
    constexpr float centerV = 0.5f;

    const float relU = u - centerU;
    const float relV = v - centerV;

    // 向量化友好计算
    const float newU = relU * cosA - relV * sinA + centerU;
    const float newV = relU * sinA + relV * cosA + centerV;

    // Extended resource-pack UVs are legal and must remain repeatable.
    u = newU;
    v = newV;
}

// 预计算三角函数值(包含常见角度优化)
static void getCosSin(int angle, float& cosA, float& sinA) {
    angle = (angle % 360 + 360) % 360;

    // 常见角度快速返回
    switch (angle) {
    case 0:   cosA = 1.0f; sinA = 0.0f; return;
    case 90:  cosA = 0.0f; sinA = 1.0f; return;
    case 180: cosA = -1.0f; sinA = 0.0f; return;
    case 270: cosA = 0.0f; sinA = -1.0f; return;
    }

    const float rad = angle * (3.14159265f / 180.0f);
    cosA = std::cos(rad);
    sinA = std::sin(rad);
}

// 优化后的UV分离,使用Face结构体中的uvIndices
static void createUniqueUVs(ModelData& modelData) {
    std::vector<float> newUVs;
    const size_t faceCount = modelData.faces.size();
    const size_t vertexCount = faceCount * 4; // 每个面4个点
    
    newUVs.reserve(vertexCount * 2); // 每个点2个UV坐标
    
    // 遍历每个Face,提取并重建UV坐标,更新uvIndices
    for (size_t fi = 0; fi < faceCount; ++fi) {
        Face& face = modelData.faces[fi];
        for (int j = 0; j < 4; ++j) {
            int oldIdx = face.uvIndices[j];
            // 边界检查
            if (oldIdx >= 0 && oldIdx*2+1 < modelData.uvCoordinates.size()) {
                newUVs.push_back(modelData.uvCoordinates[oldIdx * 2]);
                newUVs.push_back(modelData.uvCoordinates[oldIdx * 2 + 1]);
                face.uvIndices[j] = static_cast<int>(newUVs.size()/2 - 1); // 更新为新索引
            } else {
                // 无效索引处理,使用默认UV值
                newUVs.push_back(0.0f);
                newUVs.push_back(0.0f);
                face.uvIndices[j] = static_cast<int>(newUVs.size()/2 - 1);
            }
        }
    }
    
    modelData.uvCoordinates = std::move(newUVs);
}

// 应用旋转到单个Face的UV(批量处理优化)
static void applyFaceRotation(ModelData& modelData, size_t faceIdx, int angle) {
    if (angle == 0 || faceIdx >= modelData.faces.size()) return;
    
    float cosA, sinA;
    getCosSin(angle, cosA, sinA);
    if (cosA == 1.0f && sinA == 0.0f) return;
    
    Face& f = modelData.faces[faceIdx];
    for (int j = 0; j < 4; ++j) {
        int uvIdx = f.uvIndices[j];
        // 边界检查
        if (uvIdx >= 0 && uvIdx*2+1 < modelData.uvCoordinates.size()) {
            float& u = modelData.uvCoordinates[uvIdx * 2];
            float& v = modelData.uvCoordinates[uvIdx * 2 + 1];
            float frameRatio = 1.0f;
            if (f.materialIndex >= 0 && static_cast<size_t>(f.materialIndex) < modelData.materials.size()) {
                const auto& material = modelData.materials[f.materialIndex];
                // 与 ComputeFaceUvCoords 一致: 以实际条带比例为准, 不依赖 ANIMATED 标记
                if (material.aspectRatio > 1.0f)
                    frameRatio = material.aspectRatio;
            }
            // Undo animation cropping before rotation, then map back to frame 0.
            // Rotating the packed frame UV around the entire sheet selects other frames.
            float localV = 1.0f - (1.0f - v) * frameRatio;
            fastRotateUV(u, localV, cosA, sinA);
            v = 1.0f - (1.0f - localV) / frameRatio;
        }
    }
}

// uvlock: Minecraft (BlockMath.getUVLockTransform) turns the face UVs by
// L2B(face)^-1 * R^-1 * L2B(rotated face). For 90-degree blockstate rotations
// that is a quarter turn in the face plane; return it as a UV rotation angle.
static std::vector<std::vector<float>> UvCoordsFromRegion(
    std::vector<float> uvRegion, int rotation, const Material* material);

using IMat3 = std::array<std::array<int, 3>, 3>;
static IMat3 IMul(const IMat3& a, const IMat3& b) {
    IMat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}
static IMat3 ITrans(const IMat3& a) {
    IMat3 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[i][j] = a[j][i];
    return r;
}
static void ICosSin(int deg, int& c, int& s) {
    deg = ((deg % 360) + 360) % 360;
    c = deg == 0 ? 1 : (deg == 180 ? -1 : 0);
    s = deg == 90 ? 1 : (deg == 270 ? -1 : 0);
}
static IMat3 IRotX(int deg) { int c, s; ICosSin(deg, c, s); return {{{1, 0, 0}, {0, c, -s}, {0, s, c}}}; }
static IMat3 IRotY(int deg) { int c, s; ICosSin(deg, c, s); return {{{c, 0, s}, {0, 1, 0}, {-s, 0, c}}}; }
static std::array<int, 3> FaceNormal(FaceType f) {
    switch (f) {
    case UP: return {0, 1, 0};
    case DOWN: return {0, -1, 0};
    case NORTH: return {0, 0, -1};
    case SOUTH: return {0, 0, 1};
    case WEST: return {-1, 0, 0};
    default: return {1, 0, 0};
    }
}
static IMat3 LocalToBlock(FaceType f) {
    switch (f) {
    case EAST: return IRotY(90);
    case WEST: return IRotY(-90);
    case NORTH: return IRotY(180);
    case UP: return IRotX(-90);
    case DOWN: return IRotX(90);
    default: return IRotY(0);
    }
}
int UvLockAngle(FaceType face, int rotationX, int rotationY) {
    if (face != UP && face != DOWN && face != NORTH && face != SOUTH && face != WEST && face != EAST) return 0;
    const IMat3 r = IMul(IRotY(-rotationY), IRotX(-rotationX));
    const auto n = FaceNormal(face);
    std::array<int, 3> rn{};
    for (int i = 0; i < 3; ++i) rn[i] = r[i][0] * n[0] + r[i][1] * n[1] + r[i][2] * n[2];
    FaceType rotated = EAST;
    for (FaceType f : {UP, DOWN, NORTH, SOUTH, WEST, EAST})
        if (FaceNormal(f) == rn) rotated = f;
    const IMat3 m = IMul(IMul(ITrans(LocalToBlock(face)), ITrans(r)), LocalToBlock(rotated));
    // m * (1,0,0) = (cos, sin) in Minecraft UV space (v down); OBJ UVs have v up.
    const int c = m[0][0], s = m[1][0];
    const int mcAngle = c == 1 ? 0 : (c == -1 ? 180 : (s == 1 ? 90 : 270));
    return (360 - mcAngle) % 360;
}

static IMat3 UvLockMatrix(FaceType face, int rotationX, int rotationY) {
    const IMat3 r = IMul(IRotY(-rotationY), IRotX(-rotationX));
    const auto n = FaceNormal(face);
    std::array<int, 3> rn{};
    for (int i = 0; i < 3; ++i) rn[i] = r[i][0] * n[0] + r[i][1] * n[1] + r[i][2] * n[2];
    FaceType rotated = EAST;
    for (FaceType f : {UP, DOWN, NORTH, SOUTH, WEST, EAST})
        if (FaceNormal(f) == rn) rotated = f;
    return IMul(IMul(ITrans(LocalToBlock(face)), ITrans(r)), LocalToBlock(rotated));
}

void RecomputeUvLock(const float region[4], int rotation, FaceType face, int rotationX, int rotationY,
                     float outRegion[4], int& outRotation) {
    const IMat3 m = UvLockMatrix(face, rotationX, rotationY);
    const int steps = ((rotation % 360) + 360) % 360 / 90;
    auto getU = [&](int i) { const int j = (i + steps) % 4; return region[(j != 0 && j != 1) ? 2 : 0]; };
    auto getV = [&](int i) { const int j = (i + steps) % 4; return region[(j != 0 && j != 3) ? 3 : 1]; };
    auto reverse = [&](int i) { return (i + 4 - steps) % 4; };
    // blockCenterToCorner: rotate around (0.5, 0.5, 0.5); the face plane maps onto itself.
    auto transform = [&](float u, float v, float& tu, float& tv) {
        const float x = u / 16.0f - 0.5f, y = v / 16.0f - 0.5f, z = -0.5f;
        tu = 16.0f * (m[0][0] * x + m[0][1] * y + m[0][2] * z + 0.5f);
        tv = 16.0f * (m[1][0] * x + m[1][1] * y + m[1][2] * z + 0.5f);
    };
    auto signum = [](float x) { return x > 0.0f ? 1 : (x < 0.0f ? -1 : 0); };
    const float f = getU(reverse(0)), g = getV(reverse(0));
    const float j = getU(reverse(2)), k = getV(reverse(2));
    float h, i, l, mm;
    transform(f, g, h, i);
    transform(j, k, l, mm);
    if (signum(j - f) == signum(l - h)) { outRegion[0] = h; outRegion[2] = l; }
    else { outRegion[0] = l; outRegion[2] = h; }
    if (signum(k - g) == signum(mm - i)) { outRegion[1] = i; outRegion[3] = mm; }
    else { outRegion[1] = mm; outRegion[3] = i; }
    // Vanilla: floorMod(-round(atan2(rotated dir) / 90) * 90, 360).
    int c, s;
    ICosSin(rotation, c, s);
    const int rx = m[0][0] * c + m[0][1] * s, ry = m[1][0] * c + m[1][1] * s;
    const int quarter = rx == 1 ? 0 : (rx == -1 ? 2 : (ry == 1 ? 1 : -1));
    outRotation = ((-quarter * 90) % 360 + 360) % 360;
}

// 主逻辑优化(预处理面类型+并行处理)
void ApplyRotationToUV(ModelData& modelData, int rotationX, int rotationY) {
    createUniqueUVs(modelData);
    
    // 预处理面类型
    std::vector<FaceType> faceTypes;
    size_t faceCount = modelData.faces.size(); // 每个 Face 结构体代表一个完整的面
    faceTypes.reserve(faceCount);
    
    for (size_t i = 0; i < faceCount; ++i) {
        // cullface is an occlusion hint, not the geometric direction. Most
        // extra leaf/frond quads intentionally omit it.
        const Face& f = modelData.faces[i];
        FaceType faceType = f.faceDirection;
        if (faceType == DO_NOT_CULL || faceType == UNKNOWN) {
            auto position = [&](int j) {
                size_t k = static_cast<size_t>(f.vertexIndices[j]) * 3;
                if (k + 2 >= modelData.vertices.size()) return std::array<float,3>{};
                return std::array<float,3>{modelData.vertices[k],modelData.vertices[k+1],modelData.vertices[k+2]};
            };
            const auto a=position(0), b=position(1), c=position(2);
            float ax=b[0]-a[0], ay=b[1]-a[1], az=b[2]-a[2];
            float bx=c[0]-a[0], by=c[1]-a[1], bz=c[2]-a[2];
            // Undo blockstate Y then X rotation to recover the original face.
            float x=ay*bz-az*by, y=az*bx-ax*bz, z=ax*by-ay*bx;
            float ry=rotationY*static_cast<float>(M_PI/180.0), rx=rotationX*static_cast<float>(M_PI/180.0);
            float nx=x*std::cos(ry)+z*std::sin(ry), nz=-x*std::sin(ry)+z*std::cos(ry);
            x=nx; z=nz;
            float ny=y*std::cos(rx)-z*std::sin(rx); nz=y*std::sin(rx)+z*std::cos(rx);
            y=ny; z=nz;
            if (std::fabs(x)>=std::fabs(y) && std::fabs(x)>=std::fabs(z)) faceType=x>0?EAST:WEST;
            else if (std::fabs(y)>=std::fabs(z)) faceType=y>0?UP:DOWN;
            else faceType=z>0?SOUTH:NORTH;
        }
        faceTypes.push_back(faceType);
    }

    // OpenMP并行处理
// #pragma omp parallel for
    const bool exact = modelData.uvSpecs.size() == modelData.faces.size();
    for (int i = 0; i < static_cast<int>(faceTypes.size()); ++i) {
        Face& face = modelData.faces[i];
        const bool declared = face.uvFace >= UP && face.uvFace <= EAST;
        if (exact && declared) {
            // 与原版一致: 用模型里的 uv 区域和 rotation 重算, 而不是旋转已算好的 UV。
            const auto& spec = modelData.uvSpecs[i];
            float region[4];
            int rotation = 0;
            RecomputeUvLock(spec.region, spec.rotation, static_cast<FaceType>(face.uvFace),
                            rotationX, rotationY, region, rotation);
            const Material* material =
                (face.materialIndex >= 0 && static_cast<size_t>(face.materialIndex) < modelData.materials.size())
                ? &modelData.materials[face.materialIndex] : nullptr;
            const auto uvs = UvCoordsFromRegion({region[0], region[1], region[2], region[3]}, rotation, material);
            for (int j = 0; j < 4 && j < static_cast<int>(uvs.size()); ++j) {
                const int uvIdx = face.uvIndices[j];
                if (uvIdx >= 0 && static_cast<size_t>(uvIdx) * 2 + 1 < modelData.uvCoordinates.size()) {
                    modelData.uvCoordinates[uvIdx * 2] = uvs[j][0];
                    modelData.uvCoordinates[uvIdx * 2 + 1] = uvs[j][1];
                }
            }
            continue;
        }
        const int angle = UvLockAngle(declared ? static_cast<FaceType>(face.uvFace) : faceTypes[i],
                                      rotationX, rotationY);
        if (angle != 0) {
            applyFaceRotation(modelData, i, angle);
        }
    }
}

// 旋转函数
void ApplyRotationToFaceDirections(std::vector<Face>& faces, int rotationX, int rotationY) {
    // 定义旋转规则
    auto rotateY = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::EAST;
            case FaceType::EAST: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::WEST;
            case FaceType::WEST: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateYReverse = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::WEST;
            case FaceType::WEST: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::EAST;
            case FaceType::EAST: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateX = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::UP;
            case FaceType::UP: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::DOWN;
            case FaceType::DOWN: return FaceType::NORTH;
            default: return direction;
        }
    };

    auto rotateXReverse = [](FaceType direction) -> FaceType {
        switch (direction) {
            case FaceType::NORTH: return FaceType::DOWN;
            case FaceType::DOWN: return FaceType::SOUTH;
            case FaceType::SOUTH: return FaceType::UP;
            case FaceType::UP: return FaceType::NORTH;
            default: return direction;
        }
    };

    // 对每个面的方向应用旋转
    for (Face& face : faces) {
        // 不旋转 DO_NOT_CULL
        if (face.faceDirection == FaceType::DO_NOT_CULL) continue;
        
        // 绕 X 轴旋转(90度增量)
        switch (rotationX) {
        case 270: face.faceDirection = rotateX(face.faceDirection); break;
        case 180: face.faceDirection = rotateX(rotateX(face.faceDirection)); break;
        case 90: face.faceDirection = rotateXReverse(face.faceDirection); break;
        }
        
        // 绕 Y 轴旋转(90度增量)
        switch (rotationY) {
        case 90: face.faceDirection = rotateY(face.faceDirection); break;
        case 180: face.faceDirection = rotateY(rotateY(face.faceDirection)); break;
        case 270: face.faceDirection = rotateYReverse(face.faceDirection); break;
        }
    }
}

void ApplyPositionOffset(ModelData& model, int x, int y, int z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
    }
}

void ApplyDoublePositionOffset(ModelData& model, double x, double y, double z) {
    for (size_t i = 0; i < model.vertices.size(); i += 3) {
        model.vertices[i] += x;    // X坐标偏移
        model.vertices[i + 1] += y;  // Y坐标偏移
        model.vertices[i + 2] += z;  // Z坐标偏移
    }
}

//============== 模型数据处理模块 ==============//
//---------------- JSON处理 ----------------
nlohmann::json LoadParentModel(const std::string& namespaceName, const std::string& blockId, nlohmann::json& currentModelJson,
                               std::unordered_set<std::string>* visiting, int depth) {
    std::unordered_set<std::string> localVisiting;
    if (!visiting) visiting = &localVisiting;
    if (depth > 128) {
        std::cerr << "Parent model depth limit reached: " << namespaceName << ":" << blockId << std::endl;
        currentModelJson.erase("parent");
        return currentModelJson;
    }

    // 如果当前模型没有 parent 属性,直接返回
    if (!currentModelJson.contains("parent")) {
        return currentModelJson;
    }

    // 获取当前模型的 parent
    std::string parentModelId = currentModelJson["parent"];

    // 判断 parentModelId 是否包含冒号(即是否包含命名空间)
    size_t colonPos = parentModelId.find(':');
    std::string parentNamespace = "minecraft";  // 默认使用minecraft作为当前的 namespaceName

    if (colonPos != std::string::npos) {
        parentNamespace = parentModelId.substr(0, colonPos);  // 提取冒号前的部分作为父模型的命名空间
        parentModelId = parentModelId.substr(colonPos + 1);  // 提取冒号后的部分作为父模型的 ID
    }

    // 生成唯一缓存键，并检测父链循环
    std::string cacheKey = parentNamespace + ":" + parentModelId;
    if (!visiting->insert(cacheKey).second) {
        std::cerr << "Parent model cycle detected: " << cacheKey << std::endl;
        currentModelJson.erase("parent");
        return currentModelJson;
    }
    // 检查缓存是否存在
    {
        std::lock_guard<std::recursive_mutex> lock(parentModelCacheMutex);
        auto cacheIt = parentModelCache.find(cacheKey);
        if (cacheIt != parentModelCache.end()) {
            // 从缓存中获取父模型数据
            nlohmann::json parentModelJson = cacheIt->second;

            // 合并父模型的属性到当前模型中
            currentModelJson = MergeModelJson(parentModelJson, currentModelJson);

            // 如果父模型没有 parent 属性,停止递归
            if (!parentModelJson.contains("parent")) {
                return currentModelJson;
            }

            // 递归加载父模型的父模型
            return LoadParentModel(parentNamespace, parentModelId, currentModelJson, visiting, depth + 1);
        }
    }

    // 缓存未命中,加载父模型
    nlohmann::json parentModelJson = GetModelJson(parentNamespace, parentModelId);

    // 如果父模型不存在,直接返回当前模型
    if (parentModelJson.is_null()) {
        return currentModelJson;
    }

    // 将父模型数据存入缓存
    {
        std::lock_guard<std::recursive_mutex> lock(parentModelCacheMutex); // 加锁
        parentModelCache[cacheKey] = parentModelJson;
    }

    // 合并父模型的属性到当前模型中
    currentModelJson = MergeModelJson(parentModelJson, currentModelJson);

    // 如果父模型没有 parent 属性,停止递归
    if (!parentModelJson.contains("parent")) {
        return currentModelJson;
    }

    // 递归加载父模型的父模型
    return LoadParentModel(parentNamespace, parentModelId, currentModelJson, visiting, depth + 1);
}

// Minecraft 26.1+ 模型纹理值可能是对象(如 {"sprite": "...", "force_translucent": true})。
// 这里归一化为字符串: 字符串直接返回, 对象取 "sprite" 字段, 其他返回空串。
static std::string GetTextureValueString(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_object() && value.contains("sprite")) {
        const auto& sprite = value["sprite"];
        if (sprite.is_string()) return sprite.get<std::string>();
    }
    return "";
}

nlohmann::json MergeModelJson(const nlohmann::json& parentModelJson, const nlohmann::json& currentModelJson) {
    nlohmann::json mergedModelJson = currentModelJson;
    std::map<std::string, std::string> textureMap;

    // 保存子级的 textures
    if (currentModelJson.contains("textures")) {
        for (const auto& item : currentModelJson["textures"].items()) {
            textureMap[item.key()] = GetTextureValueString(item.value());
        }
    }

    // 父模型的 parent 属性覆盖子模型的 parent 属性
    if (parentModelJson.contains("parent")) {
        mergedModelJson["parent"] = parentModelJson["parent"];
    }

    // 合并 "textures"
    if (parentModelJson.contains("textures")) {
        if (!mergedModelJson.contains("textures")) {
            mergedModelJson["textures"] = nlohmann::json::object();
        }
        for (const auto& item : parentModelJson["textures"].items()) {
            const std::string& key = item.key();
            // 仅当子级不存在该键时处理父级的键
            if (!mergedModelJson["textures"].contains(key)) {
                std::string textureValue = GetTextureValueString(item.value());
                // 处理变量引用(如 #texture)
                if (!textureValue.empty() && textureValue[0] == '#') {
                    std::string varName = textureValue.substr(1);
                    if (textureMap.find(varName) != textureMap.end()) {
                        textureValue = textureMap[varName];
                    }
                }
                mergedModelJson["textures"][key] = textureValue;
            }
        }
    }

    // Minecraft 模型继承语义：子模型一旦声明 elements，就完整替换父模型
    // 的 elements，而不是追加。旧实现把两者拼接，会让自定义 top/bottom slab
    // 同时生成两套几何；重合面随后被偏移/去重，表现为半砖缺面或上下错位。
    if (!currentModelJson.contains("elements") && parentModelJson.contains("elements")) {
        mergedModelJson["elements"] = parentModelJson["elements"];
    }

    // 合并 "display"
    if (parentModelJson.contains("display") && !currentModelJson.contains("display")) {
        mergedModelJson["display"] = parentModelJson["display"];
    }

    // 合并其他需要继承的属性
    if (parentModelJson.contains("ambientocclusion") && !currentModelJson.contains("ambientocclusion")) {
        mergedModelJson["ambientocclusion"] = parentModelJson["ambientocclusion"];
    }

    // porting_lib:composite — inherit children + loader from parent when child has neither.
    if (!currentModelJson.contains("children") && parentModelJson.contains("children")) {
        mergedModelJson["children"] = parentModelJson["children"];
    }
    // Custom loaders (forge:obj, forge:composite, porting_lib:obj) and their OBJ
    // "model" / "flip_v" are inherited, e.g. red_valve_handle -> valve_handle.
    for (const char* key : { "porting_lib:loader", "loader", "model", "flip_v" }) {
        if (!currentModelJson.contains(key) && parentModelJson.contains(key)) {
            mergedModelJson[key] = parentModelJson[key];
        }
    }

    return mergedModelJson;
}

nlohmann::json GetModelJson(const std::string& namespaceName, const std::string& modelPath) {
    // 使用快速查找索引(O(1)), 回退到线性扫描(O(N))
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        std::string indexKey = std::string("models:") + namespaceName + ":" + modelPath;
        auto it = GlobalCache::modelIndex.find(indexKey);
        if (it != GlobalCache::modelIndex.end()) {
            auto cacheIt = GlobalCache::models.find(it->second);
            if (cacheIt != GlobalCache::models.end()) {
                return cacheIt->second;
            }
        }
    }

    // 回退: 线性扫描
    std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + modelPath;
        auto it = GlobalCache::models.find(cacheKey);
        if (it != GlobalCache::models.end()) {
            return it->second;
        }
    }

    std::cerr << "Model not found: " << namespaceName << ":" << modelPath << std::endl;
    return nlohmann::json();
}

//———————————将JSON数据转为结构体的方法———————————————
//---------------- 材质处理 ----------------
// 选择材质的贴图键：忽略 particle(仅粒子效果引用, 常与某个面共图)，其余取字典序
// 最小的键，保证命名结果稳定（如 cactus 侧面的键取 side 而不是 particle）。
static void UpdateMaterialTextureKey(Material& material, const std::string& textureKey) {
    if (textureKey.empty()) return;
    if (material.textureKey.empty()) {
        material.textureKey = textureKey;
        return;
    }
    const bool currentParticle = (material.textureKey == "particle");
    const bool nextParticle = (textureKey == "particle");
    if (currentParticle && !nextParticle) {
        material.textureKey = textureKey;
    }
    else if (!nextParticle && textureKey < material.textureKey) {
        material.textureKey = textureKey;
    }
}

void processTextures(const nlohmann::json& modelJson, ModelData& data,
    std::unordered_map<std::string, int>& textureKeyToMaterialIndex) {

    std::unordered_map<std::string, int> processedMaterials; // 材质名称到索引的映射

    if (modelJson.contains("textures") && modelJson["textures"].is_object()) {
        const auto& textures = modelJson["textures"];
        std::unordered_map<std::string, std::string> rawTextures;
        for (const auto& texture : textures.items()) {
            rawTextures[texture.key()] = GetTextureValueString(texture.value());
        }

        // 递归解析 #side、#top 等纹理变量。兼容旧版字符串值和 26.1+
        // {"sprite":"#side", ...} 对象值，并限制深度避免循环引用。
        auto resolveTexture = [&](const std::string& key) {
            std::string value;
            auto it = rawTextures.find(key);
            if (it != rawTextures.end()) value = it->second;
            for (int depth = 0; depth < 32 && !value.empty() && value[0] == '#'; ++depth) {
                auto ref = rawTextures.find(value.substr(1));
                if (ref == rawTextures.end() || ref->second == value) return std::string();
                value = ref->second;
            }
            return (!value.empty() && value[0] == '#') ? std::string() : value;
        };

        for (const auto& texture : textures.items()) {
            std::string textureKey = texture.key();
            std::string textureValue = resolveTexture(textureKey);

            // 解析命名空间和路径
            size_t colonPos = textureValue.find(':');
            std::string namespaceName = "minecraft";
            std::string pathPart = textureValue;
            if (colonPos != std::string::npos) {
                namespaceName = textureValue.substr(0, colonPos);
                pathPart = textureValue.substr(colonPos + 1);
            }

            // ---- START MODIFIED CODE ----
            // 检查 pathPart 是否有问题 (例如, 空, 以'/'结尾), 或者原始 textureKey 是否为 "missing"
            if (textureKey == "missing" || pathPart.empty() || pathPart.back() == '/') {
                std::string placeholderMaterialName = namespaceName + ":" + pathPart + (textureKey == "missing" ? "missing_placeholder" : "empty_path_placeholder");
                
                // 使用原始 textureKey 进行映射,确保唯一性
                std::string uniqueMaterialKeyForMap = textureKey; 

                if (processedMaterials.find(placeholderMaterialName) == processedMaterials.end()) {
                    Material newMaterial;
                    newMaterial.name = placeholderMaterialName;
                    newMaterial.texturePath = ""; // 空路径表示缺失纹理
                    newMaterial.tintIndex = -1;
                    newMaterial.type = NORMAL;
                    newMaterial.aspectRatio = 1.0f;
                    
                    int materialIndex = data.materials.size();
                    data.materials.push_back(newMaterial);
                    processedMaterials[placeholderMaterialName] = materialIndex;
                    textureKeyToMaterialIndex[uniqueMaterialKeyForMap] = materialIndex;
                } else {
                    textureKeyToMaterialIndex[uniqueMaterialKeyForMap] = processedMaterials[placeholderMaterialName];
                }
                continue; // 跳过对此纹理条目的常规处理
            }
            // ---- END MODIFIED CODE ----

            // 生成唯一材质标识
            std::string fullMaterialName = namespaceName + ":" + pathPart;

            // 检查是否已处理过该材质
            if (processedMaterials.find(fullMaterialName) == processedMaterials.end()) {
                // 生成缓存键
                std::string cacheKey = namespaceName + ":" + pathPart;

                // 保存纹理并获取路径。锁内只查询；Save/Register 必须在锁外执行，
                // 否则 RegisterTexture 会再次锁同一 mutex，造成自死锁。
                std::string textureSavePath;
                {
                    std::lock_guard<std::mutex> lock(texturePathCacheMutex);
                    auto cacheIt = texturePathCache.find(cacheKey);
                    if (cacheIt != texturePathCache.end()) {
                        textureSavePath = cacheIt->second;
                    }
                }
                if (textureSavePath.empty()) {
                    std::string saveDir = "textures";
                    if (SaveTextureToFile(namespaceName, pathPart, saveDir)) {
                        textureSavePath = "textures/" + namespaceName + "/" + pathPart + ".png";
                        // RegisterTexture 内部自行加锁并处理并发重复注册。
                        RegisterTexture(namespaceName, pathPart, textureSavePath);
                    }
                }

                // 记录材质信息
                Material newMaterial;
                newMaterial.name = fullMaterialName;
                newMaterial.texturePath = textureSavePath;
                newMaterial.textureKey = textureKey;
                newMaterial.tintIndex = -1;  // 默认值
                
                // 检测材质类型和长宽比(如果为动态材质)
                float aspectRatio = 1.0f;
                newMaterial.type = DetectMaterialType(namespaceName, pathPart, aspectRatio);
                newMaterial.aspectRatio = aspectRatio;
                
                int materialIndex = data.materials.size();
                data.materials.push_back(newMaterial);
                processedMaterials[fullMaterialName] = materialIndex;
            }
            else {
                // 同一贴图可能有多个键(如 top/particle 同图)：忽略 particle 后取最小键
                UpdateMaterialTextureKey(data.materials[processedMaterials[fullMaterialName]], textureKey);
            }

            // 记录材质键到索引的映射
            textureKeyToMaterialIndex[textureKey] = processedMaterials[fullMaterialName];
        }
    }
}
//---------------- 材质重命名 ----------------
// 材质名 -> 贴图路径 的全局注册表：同一张图复用同一个名字；不同图重名时
// 使用更长候选名降级，保证"名字 -> 贴图"唯一。多线程处理方块状态时需要加锁。
static std::mutex g_materialNameMutex;
static std::unordered_map<std::string, std::string> g_materialNameToTexture;
// 贴图路径 -> 该贴图已确定的材质名（先到先得）：模组方块复用同一张贴图时直接沿用，
// 避免一张零件贴图被几十个方块各建一个材质。原版方块不参与复用，但会作为可复用的目标。
static std::unordered_map<std::string, std::string> g_textureToMaterialName;
static size_t g_modMaterialReuseHits = 0;   // 受 g_materialNameMutex 保护

size_t GetModMaterialReuseCount() {
    std::lock_guard<std::mutex> lock(g_materialNameMutex);
    return g_modMaterialReuseHits;
}

namespace {
std::string TryRegisterMaterialName(const std::string& candidate, const std::string& texturePath) {
    if (candidate.empty()) return {};
    std::lock_guard<std::mutex> lock(g_materialNameMutex);
    auto it = g_materialNameToTexture.find(candidate);
    if (it == g_materialNameToTexture.end()) {
        g_materialNameToTexture.emplace(candidate, texturePath);
        return candidate;
    }
    return (it->second == texturePath) ? candidate : std::string();
}

// textures/minecraft/block/oak_log.png -> minecraft/block/oak_log
std::string MaterialTextureRelPath(const std::string& texturePath) {
    const std::string prefix = "textures/";
    std::string path = texturePath;
    if (path.rfind(prefix, 0) == 0) path = path.substr(prefix.size());
    size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) path = path.substr(0, dot);
    return path;
}
}

std::string MaterialTextureBasename(const std::string& texturePath) {
    size_t slash = texturePath.find_last_of('/');
    std::string leaf = (slash == std::string::npos) ? texturePath : texturePath.substr(slash + 1);
    size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos) leaf = leaf.substr(0, dot);
    return leaf;
}

void RenameBlockMaterials(ModelData& model, const std::string& namespaceName, const std::string& baseBlockId) {
    if (model.materials.empty() || baseBlockId.empty()) return;
    const bool singleMaterial = (model.materials.size() == 1);
    const std::string prefix = namespaceName + ":" + baseBlockId;
    // 原版方块之间始终不合并；模组方块在开关打开时复用同一张贴图已有的材质。
    const bool vanillaBlock = (namespaceName == "minecraft");
    for (auto& material : model.materials) {
        // 只处理来自模型 textures 的材质；占位/特殊材质(无贴图键)保持原名
        if (material.textureKey.empty()) continue;

        // 光方块：硬编码旧版名 minecraft:block/light_block_NN，避免与 light_blue 等混淆
        if (namespaceName == "minecraft" && baseBlockId == "light") {
            std::string level = "15";
            const std::string texBase = MaterialTextureBasename(material.texturePath);
            size_t i = texBase.size();
            while (i > 0 && texBase[i - 1] >= '0' && texBase[i - 1] <= '9') --i;
            if (i < texBase.size() && i > 0 && texBase[i - 1] == '_') {
                std::string digits = texBase.substr(i);
                if (!digits.empty()) {
                    if (digits.size() == 1) digits = "0" + digits;
                    level = digits;
                }
            }
            material.name = "minecraft:block/light_block_" + level;
            continue;
        }

        // 可复用的前提：贴图有效、该材质不参与 tint、且这个模型没有共面叠加层
        // (叠加层元数据按材质名索引，共享材质会把叠加层扩散到别的方块)
        const bool mergeEligible = config.mergeModTextures
            && !material.texturePath.empty()
            && material.texturePath != "None"
            && material.tintIndex < 0
            && model.overlayPairs.empty();

        // 模组方块：这张贴图已经有材质了就直接复用，不再新建
        if (mergeEligible && !vanillaBlock) {
            std::lock_guard<std::mutex> lock(g_materialNameMutex);
            auto it = g_textureToMaterialName.find(material.texturePath);
            if (it != g_textureToMaterialName.end()) {
                material.name = it->second;
                ++g_modMaterialReuseHits;
                continue;
            }
        }

        std::string resolved;
        // 1. 单材质且贴图就是 block/<方块id>：直接用方块 id（名字最简且稳定）
        if (singleMaterial &&
            material.texturePath == "textures/" + namespaceName + "/block/" + baseBlockId + ".png") {
            resolved = TryRegisterMaterialName(prefix, material.texturePath);
        }
        if (resolved.empty()) {
            const std::string base = prefix + "#" + material.textureKey;
            // 2. 方块id#贴图键~贴图文件名（名字只由 方块/键/贴图 决定，与处理顺序无关）
            resolved = TryRegisterMaterialName(base + "~" + MaterialTextureBasename(material.texturePath),
                material.texturePath);
            // 3. 方块id#贴图键~相对路径（同一键下不同目录同名文件的兜底）
            if (resolved.empty()) {
                resolved = TryRegisterMaterialName(base + "~" + MaterialTextureRelPath(material.texturePath),
                    material.texturePath);
            }
        }

        if (!resolved.empty()) {
            material.name = resolved;
            // 登记"贴图 -> 材质名"（先到先得）：供后续模组方块复用
            if (mergeEligible) {
                std::lock_guard<std::mutex> lock(g_materialNameMutex);
                g_textureToMaterialName.emplace(material.texturePath, resolved);
            }
        }
    }
}

// 计算面的最终 UV 坐标(显式 uv、镜像翻转、动态贴图 V 缩放、90° 旋转)。
// 面重叠配对比较与实际写出共用同一份逻辑,保证两层规格比较一致。
static std::vector<std::vector<float>> UvCoordsFromRegion(
    std::vector<float> uvRegion, int rotation, const Material* material);

// 面的 uv 区域(像素): 显式 uv 优先, 否则按原版由元素范围推导。
static std::vector<float> FaceUvRegion(
    const std::string& faceName,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    const nlohmann::json& faceData)
{
    std::vector<float> uvRegion;
    if (faceName == "down") {
        uvRegion = { x1 * 16, (1 - z2) * 16, x2 * 16, (1 - z1) * 16 };
    }
    else if (faceName == "up") {
        uvRegion = { x1 * 16, z1 * 16, x2 * 16, z2 * 16 };
    }
    else if (faceName == "north") {
        uvRegion = { (1 - x2) * 16, (1 - y2) * 16, (1 - x1) * 16, (1 - y1) * 16 };
    }
    else if (faceName == "south") {
        uvRegion = { x1 * 16, (1 - y2) * 16, x2 * 16, (1 - y1) * 16 };
    }
    else if (faceName == "west") {
        uvRegion = { z1 * 16, (1 - y2) * 16, z2 * 16, (1 - y1) * 16 };
    }
    else if (faceName == "east") {
        uvRegion = { (1 - z2) * 16, (1 - y2) * 16, (1 - z1) * 16, (1 - y1) * 16 };
    }
    else {
        return {};
    }

    // 如果 JSON 中存在 uv 则使用其数据
    if (faceData.contains("uv")) {
        auto uv = faceData["uv"];
        uvRegion = {
            uv[0].get<float>(),
            uv[1].get<float>(),
            uv[2].get<float>(),
            uv[3].get<float>()
        };
    }
    return uvRegion;
}

static std::vector<std::vector<float>> ComputeFaceUvCoords(
    const std::string& faceName,
    float x1, float y1, float z1,
    float x2, float y2, float z2,
    const nlohmann::json& faceData,
    const Material* material)
{
    std::vector<float> uvRegion = FaceUvRegion(faceName, x1, y1, z1, x2, y2, z2, faceData);
    if (uvRegion.empty()) return {};
    return UvCoordsFromRegion(std::move(uvRegion), faceData.value("rotation", 0), material);
}

static std::vector<std::vector<float>> UvCoordsFromRegion(
    std::vector<float> uvRegion, int rotation, const Material* material)
{

    // 检测UV区域是否有镜像翻转
    bool flipX = uvRegion[0] > uvRegion[2];
    bool flipY = uvRegion[1] > uvRegion[3];
    if (flipX) {
        std::swap(uvRegion[0], uvRegion[2]);
    }
    if (flipY) {
        std::swap(uvRegion[1], uvRegion[3]);
    }

    std::vector<std::vector<float>> uvCoords = {
        {uvRegion[2] / 16.0f, 1 - uvRegion[3] / 16.0f},
        {uvRegion[2] / 16.0f, 1 - uvRegion[1] / 16.0f},
        {uvRegion[0] / 16.0f, 1 - uvRegion[1] / 16.0f},
        {uvRegion[0] / 16.0f, 1 - uvRegion[3] / 16.0f}
    };

    // 条带贴图(高>宽, 如灯笼 16x48)只显示第 0 帧: 缩放 V。
    // 用 aspectRatio>1 判定, 不依赖 type==ANIMATED——资源包覆盖 PNG 而 mcmeta
    // 仍指向原版 key 时 type 可能是 ANIMATED 但 aspect 曾被算成 1, 或反过来只有尺寸没有 mcmeta。
    if (material != nullptr && material->aspectRatio > 1.0f) {
        float aspectRatio = material->aspectRatio;
        for (auto& uv : uvCoords) {
            float v = 1.0f - uv[1];
            v = v / aspectRatio;
            uv[1] = 1.0f - v;
        }
    }

    if (flipX) {
        std::swap(uvCoords[0], uvCoords[3]);
        std::swap(uvCoords[1], uvCoords[2]);
    }
    if (flipY) {
        std::swap(uvCoords[0], uvCoords[1]);
        std::swap(uvCoords[3], uvCoords[2]);
    }

    int steps = ((rotation % 360) + 360) % 360 / 90;
    if (steps != 0) {
        std::vector<std::vector<float>> rotatedUV(4);
        for (int i = 0; i < 4; i++) {
            // 原版 BlockFaceUV.getShiftedIndex 是 (i + rotation/90)%4，
            // 顺时针旋转；这里必须是 +steps。用 -steps 会让 90/270 的面差 180°
            // （180 因 ±2 等价而看不出，故此前未暴露）。
            rotatedUV[i] = uvCoords[(i + steps) % 4];
        }
        uvCoords = rotatedUV;
    }
    return uvCoords;
}

static bool UvCoordsEqual(const std::vector<std::vector<float>>& a,
    const std::vector<std::vector<float>>& b)
{
    if (a.size() != 4 || b.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (std::fabs(a[i][0] - b[i][0]) > 1e-6f) return false;
        if (std::fabs(a[i][1] - b[i][1]) > 1e-6f) return false;
    }
    return true;
}

//---------------- 几何数据处理 ----------------
void processElements(const nlohmann::json& modelJson, ModelData& data,
    const std::unordered_map<std::string, int>& textureKeyToMaterialIndex)
{
    std::unordered_map<std::string, int> vertexCache;
    std::unordered_map<std::string, int> uvCache;
    int faceId = 0;
    short tintindex = -1;
    std::unordered_map<std::string, int> faceCountMap; // 面计数映射
    std::unordered_map<std::string, std::unordered_set<int>> faceMaterialMap;
    std::unordered_map<std::string, int> faceBaseIndexMap;               // 每个共面键的首面(底层)索引
    std::unordered_map<std::string, std::vector<std::vector<float>>> faceBaseUvMap; // 底层最终 UV
    // 本模型里属于"双面几何"的面名（同元素内朝向相反且完全重合的正反两面）。
    // 跨元素累积：只要某个元素声明过正反两面，该面名就按双面几何处理。
    std::unordered_set<std::string> bothSidedFaceNames;

    auto elements = modelJson["elements"];

    for (const auto& element : elements) {
        if (element.contains("from") && element.contains("to") && element.contains("faces")) {
            auto from = element["from"];
            auto to = element["to"];
            auto faces = element["faces"];


            // 转换原始坐标为 OBJ 坐标系(/16)
            float x1 = from[0].get<float>() / 16.0f;
            float y1 = from[1].get<float>() / 16.0f;
            float z1 = from[2].get<float>() / 16.0f;
            float x2 = to[0].get<float>() / 16.0f;
            float y2 = to[1].get<float>() / 16.0f;
            float z2 = to[2].get<float>() / 16.0f;
            
            // 检测是否为"极薄"块
            constexpr float THIN_THRESHOLD = 0.01f; // 1/100 方块单位的阈值
            bool isThinX = std::abs(x2 - x1) < THIN_THRESHOLD;
            bool isThinY = std::abs(y2 - y1) < THIN_THRESHOLD;
            bool isThinZ = std::abs(z2 - z1) < THIN_THRESHOLD;
            
            // 生成基础顶点数据
            std::unordered_map<std::string, std::vector<std::vector<float>>> elementVertices;
            
            // 遍历元素的面,动态生成顶点数据
            for (auto& face : faces.items()) { // 'faces' is from element["faces"]
                std::string faceName = face.key();
                if (faceName == "north") {
                    elementVertices[faceName] = { {x1, y1, z1}, {x1, y2, z1}, {x2, y2, z1}, {x2, y1, z1} };
                }
                else if (faceName == "south") {
                    elementVertices[faceName] = { {x2, y1, z2}, {x2, y2, z2}, {x1, y2, z2}, {x1, y1, z2} };
                }
                else if (faceName == "east") {
                    elementVertices[faceName] = { {x2, y1, z1}, {x2, y2, z1}, {x2, y2, z2}, {x2, y1, z2} };
                }
                else if (faceName == "west") {
                    elementVertices[faceName] = { {x1, y1, z2}, {x1, y2, z2}, {x1, y2, z1}, {x1, y1, z1} };
                }
                else if (faceName == "up") {
                    elementVertices[faceName] = { {x2, y2, z2}, {x2, y2, z1} ,{x1, y2, z1}, {x1, y2, z2}  };
                }
                else if (faceName == "down") {
                    // 必须与其他五面一致地相对原版 FaceInfo 循环移 2 位：
                    // 通用 UV 顺序是 [(u2,v2),(u2,v1),(u1,v1),(u1,v2)]，比原版
                    // BlockFaceUV 的索引约定正好差 2 位；五面靠角点表移 2 位抵消，
                    // down 若照抄原版角点表就会整体差 180° 旋转（贴图上下颠倒）。
                    // 循环移 2 位不改变绕序与法线(仍为 -y)。
                    elementVertices[faceName] = {  {x2, y1, z1}, {x2, y1, z2}, {x1, y1, z2},{ x1, y1, z1 }};
                }
            }

            // 处理元素旋转
            if (element.contains("rotation") && element["rotation"].is_object()) {
                const auto& rotation = element["rotation"];
                // 旧版 axis + angle 格式，保持原逻辑。
                if (rotation.contains("axis") && rotation["axis"].is_string() &&
                    rotation.contains("angle") && rotation["angle"].is_number()) {
                std::string axis = rotation["axis"].get<std::string>();

                float angle_deg = rotation["angle"].get<float>();
                auto origin = rotation["origin"];
                // 转换旋转中心到 OBJ 坐标系
                float ox = origin[0].get<float>() / 16.0f;
                float oy = origin[1].get<float>() / 16.0f;
                float oz = origin[2].get<float>() / 16.0f;
                float angle_rad = angle_deg * (M_PI / 180.0f); // 转换为弧度
                // 对每个面的顶点应用旋转
                for (auto& faceEntry : elementVertices) {
                    auto& vertices = faceEntry.second;
                    for (auto& vertex : vertices) {
                        float& vx = vertex[0];
                        float& vy = vertex[1];
                        float& vz = vertex[2];

                        // 平移至旋转中心相对坐标
                        float tx = vx - ox;
                        float ty = vy - oy;
                        float tz = vz - oz;

                        // 根据轴类型进行旋转
                        if (axis == "x") {
                            // 绕X轴旋转
                            float new_y = ty * cos(angle_rad) - tz * sin(angle_rad);
                            float new_z = ty * sin(angle_rad) + tz * cos(angle_rad);
                            ty = new_y;
                            tz = new_z;
                        }
                        else if (axis == "y") {
                            // 绕Y轴旋转
                            float new_x = tx * cos(angle_rad) + tz * sin(angle_rad);
                            float new_z = -tx * sin(angle_rad) + tz * cos(angle_rad);
                            tx = new_x;
                            tz = new_z;
                        }
                        else if (axis == "z") {
                            // 绕Z轴旋转
                            float new_x = tx * cos(angle_rad) - ty * sin(angle_rad);
                            float new_y = tx * sin(angle_rad) + ty * cos(angle_rad);
                            tx = new_x;
                            ty = new_y;
                        }

                        // 平移回原坐标系
                        vx = tx + ox;
                        vy = ty + oy;
                        vz = tz + oz;
                    }
                }

                // 处理rescale参数
                // 在旋转处理部分的缩放逻辑修改如下:
                bool rescale = rotation.value("rescale", false);
                if (rescale) {
                    // Minecraft FaceBakery uses sec(angle) on the two perpendicular axes.
                    // The old 22.5-degree factor was 0.765 (shrink), not 1.082 (expand).
                    bool applyScaling = std::fabs(angle_deg) > 0.0001f &&
                        (std::fabs(std::fabs(angle_deg) - 22.5f) < 0.0001f ||
                         std::fabs(std::fabs(angle_deg) - 45.0f) < 0.0001f);
                    float scale = applyScaling ? 1.0f / std::cos(angle_rad) : 1.0f;

                    if (applyScaling) {
                        // 根据旋转轴应用缩放,保留原有旋转中心偏移逻辑
                        for (auto& faceEntry : elementVertices) {
                            auto& vertices = faceEntry.second;
                            for (auto& vertex : vertices) {
                                float& vx = vertex[0];
                                float& vy = vertex[1];
                                float& vz = vertex[2];

                                // 平移至旋转中心相对坐标系
                                float tx = vx - ox;
                                float ty = vy - oy;
                                float tz = vz - oz;

                                // 根据轴类型应用缩放
                                if (axis == "x") {
                                    ty *= scale;
                                    tz *= scale;
                                }
                                else if (axis == "y") {
                                    tx *= scale;
                                    tz *= scale;
                                }
                                else if (axis == "z") {
                                    tx *= scale;
                                    ty *= scale;
                                }

                                // 平移回原坐标系
                                vx = tx + ox;
                                vy = ty + oy;
                                vz = tz + oz;
                            }
                        }
                    }
                }
                }
                else {
                    // Minecraft 26.1+ 欧拉角格式:
                    // {"x": 180, "y": -67.5, "z": -180, "origin": [8,0,8]}。
                    float ox = 0.5f, oy = 0.5f, oz = 0.5f;
                    if (rotation.contains("origin") && rotation["origin"].is_array() &&
                        rotation["origin"].size() >= 3) {
                        ox = rotation["origin"][0].get<float>() / 16.0f;
                        oy = rotation["origin"][1].get<float>() / 16.0f;
                        oz = rotation["origin"][2].get<float>() / 16.0f;
                    }
                    auto angle = [&](const char* key) {
                        return rotation.contains(key) && rotation[key].is_number()
                            ? rotation[key].get<float>() * static_cast<float>(M_PI / 180.0)
                            : 0.0f;
                    };
                    const float rx = angle("x"), ry = angle("y"), rz = angle("z");
                    for (auto& faceEntry : elementVertices) {
                        for (auto& vertex : faceEntry.second) {
                            float x = vertex[0] - ox;
                            float y = vertex[1] - oy;
                            float z = vertex[2] - oz;
                            if (rx != 0.0f) {
                                float ny = y * std::cos(rx) - z * std::sin(rx);
                                float nz = y * std::sin(rx) + z * std::cos(rx);
                                y = ny; z = nz;
                            }
                            if (ry != 0.0f) {
                                float nx = x * std::cos(ry) + z * std::sin(ry);
                                float nz = -x * std::sin(ry) + z * std::cos(ry);
                                x = nx; z = nz;
                            }
                            if (rz != 0.0f) {
                                float nx = x * std::cos(rz) - y * std::sin(rz);
                                float ny = x * std::sin(rz) + y * std::cos(rz);
                                x = nx; y = ny;
                            }
                            vertex[0] = x + ox;
                            vertex[1] = y + oy;
                            vertex[2] = z + oz;
                        }
                    }
                }
            }

            // Zero-thickness foliage explicitly defines both windings. Keep
            // both sides: their texture, UV rotation or tint may differ.
            // 默认保留正反两面（与游戏一致；Blender 判重问题由 SeparateCoincidentFaces 解决）。
            // doubleSidedGeometry=false 时退回旧行为：朝向相反且完全重合的两面只留一张。
            //
            // 注意：这套"正反重合"判定是双面几何的唯一权威依据 —— 它跑在元素自身的
            // 面表上，只认原版模型里显式声明的正反两面。合并后的大网格里，相邻方块的
            // 接触面同样是"共面 + 朝向相反 + 顶点重合"，但那不是双面几何；靠这里的
            // bothSidedFaceNames 标记把它们区分开（doubleSided.json 只收录带标记的面）。
            auto getOppositeFace = [](const std::string& faceName) -> std::string {
                if (faceName == "north") return "south";
                if (faceName == "south") return "north";
                if (faceName == "east") return "west";
                if (faceName == "west") return "east";
                if (faceName == "up") return "down";
                if (faceName == "down") return "up";
                return "";
            };
            auto areFacesCoinciding = [](const std::vector<std::vector<float>>& face1,
                const std::vector<std::vector<float>>& face2) -> bool {
                    if (face1.size() != face2.size()) return false;
                    auto toKey = [](const std::vector<float>& v) {
                        char buffer[64];
                        snprintf(buffer, sizeof(buffer), "%.4f,%.4f,%.4f", v[0], v[1], v[2]);
                        return std::string(buffer);
                    };
                    std::unordered_set<std::string> set1;
                    for (const auto& v : face1) set1.insert(toKey(v));
                    for (const auto& v : face2) {
                        if (!set1.count(toKey(v))) return false;
                    }
                    return true;
            };

            std::vector<std::string> facesToRemove;
            for (const auto& faceEntry : elementVertices) {
                const std::string& faceName = faceEntry.first;
                const std::string opposite = getOppositeFace(faceName);
                if (opposite.empty()) continue;
                auto oppositeIt = elementVertices.find(opposite);
                if (oppositeIt == elementVertices.end()) continue;
                if (areFacesCoinciding(faceEntry.second, oppositeIt->second)) {
                    // 双面几何: 判定命中时两个面名都要标记。只标记被删的那一侧是不够的 ——
                    // 后续 DeduplicateFaces 可能把另一侧删掉(up/down 的 UV 循环移位后相同),
                    // 那样留下的面就没有标记, 整对双面几何都不会被 doubleSided.json 收录。
                    bothSidedFaceNames.insert(faceName);
                    bothSidedFaceNames.insert(opposite);
                    if (faceName == "south" || faceName == "west" || faceName == "down") {
                        facesToRemove.push_back(faceName);
                    } else {
                        facesToRemove.push_back(opposite);
                    }
                }
            }
            // 同一面名可能被两个方向各推一次，去重后再删
            std::sort(facesToRemove.begin(), facesToRemove.end());
            facesToRemove.erase(std::unique(facesToRemove.begin(), facesToRemove.end()), facesToRemove.end());
            if (!config.doubleSidedGeometry) {
                for (const auto& faceToRemove : facesToRemove) {
                    elementVertices.erase(faceToRemove);
                }
            }

            // 遍历每个面的数据,判断面是否存在,如果存在则处理
            for (auto& face : faces.items()) {
                std::string faceName = face.key();
                
                if (elementVertices.find(faceName) != elementVertices.end()) {
                    // 处理当前面
                    auto faceVertices = elementVertices[faceName];
                    
                    // ======== 面重叠处理逻辑 ========
                    if (faceVertices.size() >= 3) {
                        // 计算法线方向
                        const auto& v0 = faceVertices[0];
                        const auto& v1 = faceVertices[1];
                        const auto& v2 = faceVertices[2];

                        float vec1x = v1[0] - v0[0];
                        float vec1y = v1[1] - v0[1];
                        float vec1z = v1[2] - v0[2];
                        float vec2x = v2[0] - v0[0];
                        float vec2y = v2[1] - v0[1];
                        float vec2z = v2[2] - v0[2];

                        float crossX = vec1y * vec2z - vec1z * vec2y;
                        float crossY = vec1z * vec2x - vec1x * vec2z;
                        float crossZ = vec1x * vec2y - vec1y * vec2x;

                        float length = std::sqrt(crossX * crossX + crossY * crossY + crossZ * crossZ);
                        if (length > 0) {
                            crossX /= length;
                            crossY /= length;
                            crossZ /= length;
                        }

                        // 将法线方向四舍五入到两位小数
                        crossX = std::round(crossX * 100.0f) / 100.0f;
                        crossY = std::round(crossY * 100.0f) / 100.0f;
                        crossZ = std::round(crossZ * 100.0f) / 100.0f;

                        // 生成面的几何特征指纹
                        // 使用所有顶点坐标而不仅仅是中心点
                        std::stringstream vertexStream;
                        vertexStream << std::fixed << std::setprecision(4);
                        
                        // 按照几何坐标排序,以确保相同的面(即使顶点顺序不同)有相同的键
                        std::vector<std::string> vertexKeys;
                        for (const auto& v : faceVertices) {
                            std::stringstream vs;
                            vs << std::fixed << std::setprecision(4)
                               << v[0] << "," << v[1] << "," << v[2];
                            vertexKeys.push_back(vs.str());
                        }
                        std::sort(vertexKeys.begin(), vertexKeys.end());
                        
                        // 创建基于所有顶点的键
                        std::string vertexFingerprint;
                        for (const auto& vk : vertexKeys) {
                            if (!vertexFingerprint.empty()) {
                                vertexFingerprint += "|";
                            }
                            vertexFingerprint += vk;
                        }
                        
                        // 组合法线和顶点指纹作为面的唯一标识
                        std::stringstream keyStream;
                        keyStream << std::fixed << std::setprecision(2)
                            << crossX << "," << crossY << "," << crossZ << "_"
                            << vertexFingerprint;
                        std::string key = keyStream.str();

                        int currentMaterialIndex = -1;
                        if (face.value().contains("texture")) {
                            std::string texture = face.value()["texture"];
                            if (!texture.empty() && texture.front() == '#') texture.erase(0, 1);
                            auto materialIt = textureKeyToMaterialIndex.find(texture);
                            if (materialIt != textureKeyToMaterialIndex.end()) {
                                currentMaterialIndex = materialIt->second;
                            }
                        }

                        bool skipFace = false;
                        auto& seenMaterials = faceMaterialMap[key];
                        bool isLayeredMaterial = !seenMaterials.empty() &&
                            !seenMaterials.contains(currentMaterialIndex);

                        const Material* currentMaterialPtr =
                            (currentMaterialIndex >= 0 &&
                             currentMaterialIndex < static_cast<int>(data.materials.size()))
                            ? &data.materials[currentMaterialIndex] : nullptr;

                        if (seenMaterials.empty()) {
                            // 记录底层(首个面)索引与最终 UV 规格,供叠加层配对比较
                            faceBaseIndexMap[key] = static_cast<int>(data.faces.size());
                            faceBaseUvMap[key] = ComputeFaceUvCoords(
                                faceName, x1, y1, z1, x2, y2, z2,
                                face.value(), currentMaterialPtr);
                        }

                        if (isLayeredMaterial) {
                            // 共面异材质层(如草方块侧面的 overlay 元素): UV 规格与底层
                            // 一致时记入 overlayPairs(导出阶段若底层存活就删除该面并写
                            // overlay.json, 由 Blender 材质节点实现叠加)。逐层外移保留:
                            // 配对未解析(实体模型/底层被剔除)时仍靠它避免共面闪烁。
                            auto baseUvIt = faceBaseUvMap.find(key);
                            auto baseIdxIt = faceBaseIndexMap.find(key);
                            bool sameUv = baseUvIt != faceBaseUvMap.end() &&
                                UvCoordsEqual(baseUvIt->second,
                                    ComputeFaceUvCoords(faceName, x1, y1, z1, x2, y2, z2,
                                        face.value(), currentMaterialPtr));
                            if (sameUv && baseIdxIt != faceBaseIndexMap.end()) {
                                data.overlayPairs.push_back(
                                    { baseIdxIt->second, static_cast<int>(data.faces.size()) });
                            }

                            int count = ++faceCountMap[key];
                            float offset = (count - 1) * config.overlayLayerStep;
                            for (auto& v : faceVertices) {
                                v[0] += crossX * offset;
                                v[1] += crossY * offset;
                                v[2] += crossZ * offset;
                            }
                        }
                        else if (config.allowDoubleFace) {
                            // 同材质重复面: 保持逐层外移
                            int count = ++faceCountMap[key];
                            float offset = (count - 1) * config.overlayLayerStep;
                            for (auto& v : faceVertices) {
                                v[0] += crossX * offset;
                                v[1] += crossY * offset;
                                v[2] += crossZ * offset;
                            }
                        }
                        else {
                            if (faceCountMap[key]++ >= 1) {
                                skipFace = true;
                            }
                        }

                        seenMaterials.insert(currentMaterialIndex);

                        if (skipFace) {
                            continue;
                        }

                        elementVertices[faceName] = faceVertices;
                    }

                    // ======== 顶点处理逻辑 ========
                    std::array<int, 4> vertexIndices;
                    for (int i = 0; i < 4; ++i) {
                        const auto& vertex = faceVertices[i];
                        std::string vertexKey =
                            std::to_string(vertex[0]) + "," +
                            std::to_string(vertex[1]) + "," +
                            std::to_string(vertex[2]);

                        if (vertexCache.find(vertexKey) == vertexCache.end()) {
                            vertexCache[vertexKey] = data.vertices.size() / 3;
                            data.vertices.insert(data.vertices.end(),
                                { vertex[0], vertex[1], vertex[2] });
                        }
                        vertexIndices[i] = vertexCache[vertexKey];
                    }

                    Face newFace;
                    newFace.vertexIndices = vertexIndices;
                    newFace.materialIndex = -1;
                    newFace.faceDirection = StringToFaceType("DO_NOT_CULL");
                    // 双面几何标记（见 bothSidedFaceNames）：doubleSided.json 只收录它
                    newFace.bothSided = bothSidedFaceNames.count(faceName) > 0;
                    // 薄片贴边标记：极薄元素(厚度 < 1 像素)贴到方块界面上的那一面。
                    // 导出时若该方向邻居是完整遮挡体，会沿法线外推 overlayLayerStep，
                    // 避免与邻居方块的接触面严格共面而在 Blender 里 z-fighting。
                    {
                        constexpr float EDGE_EPS = 1e-4f;
                        const bool onMinEdge = (faceName == "north" || faceName == "west" || faceName == "down");
                        const bool onMaxEdge = (faceName == "south" || faceName == "east" || faceName == "up");
                        float edgeValue = 0.0f;   // 该面应贴的方块界面坐标(0 或 1)
                        bool thicknessOk = false;
                        if (faceName == "north" || faceName == "south") {
                            thicknessOk = isThinZ; edgeValue = onMinEdge ? z1 : z2;
                        } else if (faceName == "west" || faceName == "east") {
                            thicknessOk = isThinX; edgeValue = onMinEdge ? x1 : x2;
                        } else if (faceName == "up" || faceName == "down") {
                            thicknessOk = isThinY; edgeValue = onMinEdge ? y1 : y2;
                        }
                        const bool atBoundary = onMinEdge ? (edgeValue < EDGE_EPS)
                                                          : (edgeValue > 1.0f - EDGE_EPS);
                        if (thicknessOk && atBoundary) {
                            // 元素可能带自身 rotation(如 45° 斜面)，那种面根本没贴在界面上：
                            // 这里再逐个顶点确认，只有顶点确实落在该界面才认作贴边面。
                            const int axis = (faceName == "north" || faceName == "south") ? 2
                                           : ((faceName == "west" || faceName == "east") ? 0 : 1);
                            const auto itVerts = elementVertices.find(faceName);
                            newFace.thinEdge = true;
                            if (itVerts != elementVertices.end()) {
                                for (const auto& v : itVerts->second) {
                                    if (std::fabs(v[axis] - edgeValue) > EDGE_EPS) {
                                        newFace.thinEdge = false;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    data.faces.push_back(newFace);
                    {
                        ModelData::UvSpec spec{};
                        const auto region = FaceUvRegion(faceName, x1, y1, z1, x2, y2, z2, face.value());
                        for (int k = 0; k < 4 && k < static_cast<int>(region.size()); ++k) spec.region[k] = region[k];
                        spec.rotation = face.value().value("rotation", 0);
                        data.uvSpecs.push_back(spec);
                    }

                    if (face.value().contains("texture")) {
                        std::string texture = face.value()["texture"];
                        if (texture.front() == '#') texture.erase(0, 1);

                        auto it = textureKeyToMaterialIndex.find(texture);
                        if (it != textureKeyToMaterialIndex.end()) {
                            data.faces.back().materialIndex = it->second;
                        }
                        
                        // UV数据处理
                        nlohmann::json faceData = face.value();
                        const Material* faceMaterialPtr =
                            (data.faces.back().materialIndex >= 0 &&
                             data.faces.back().materialIndex < static_cast<int>(data.materials.size()))
                            ? &data.materials[data.faces.back().materialIndex] : nullptr;
                        std::vector<std::vector<float>> uvCoords = ComputeFaceUvCoords(
                            faceName, x1, y1, z1, x2, y2, z2, faceData, faceMaterialPtr);

                        std::array<int, 4> uvIndices;
                        for (int i = 0; i < 4; ++i) {
                            const auto& uv = uvCoords[i];
                            std::string uvKey =
                                std::to_string(uv[0]) + "," +
                                std::to_string(uv[1]);

                            if (uvCache.find(uvKey) == uvCache.end()) {
                                uvCache[uvKey] = data.uvCoordinates.size() / 2;
                                data.uvCoordinates.insert(data.uvCoordinates.end(),
                                    { uv[0], uv[1] });
                            }
                            uvIndices[i] = uvCache[uvKey];
                        }

                        data.faces.back().uvIndices = uvIndices;
                    }

                    std::string faceDirection;
                    if (face.value().contains("cullface")) {
                        faceDirection = face.value()["cullface"].get<std::string>();
                    }
                    else {
                        faceDirection = "DO_NOT_CULL";
                    }
                    
                    short localTintIndex = -1;
                    if (face.value().contains("tintindex")) {
                        localTintIndex = face.value()["tintindex"].get<int>();
                    }
                    // 面级 tintindex(用于导出阶段的逐面色型解析)
                    data.faces.back().tintIndex = static_cast<int8_t>(localTintIndex);
                    // 更新此材质的tintIndex
                    if (!data.materials.empty() && data.faces.back().materialIndex >= 0 && data.faces.back().materialIndex < data.materials.size()) {
                        data.materials[data.faces.back().materialIndex].tintIndex = localTintIndex;
                    }
                    
                    // 将字符串方向转换为枚举类型并添加到faceDirections
                    data.faces.back().faceDirection = StringToFaceType(faceDirection);
                    data.faces.back().uvFace = static_cast<int8_t>(StringToFaceType(faceName));
                    faceId++;
                }

            }
        }
    }

}


// 处理模型数据的方法
ModelData ProcessModelData(const nlohmann::json& modelJson, const std::string& blockName) {
    ModelData data;
    nlohmann::json effective = modelJson;

    // 处理纹理和材质
    std::unordered_map<std::string, int> textureKeyToMaterialIndex;

    // porting_lib:composite: flatten children[].elements into one elements array so the
    // normal element baker can run (nixie_tube etc.). Child textures fill gaps only.
    if (!effective.contains("elements") && effective.contains("children") &&
        effective["children"].is_object()) {
        nlohmann::json flat = nlohmann::json::array();
        if (!effective.contains("textures") || !effective["textures"].is_object())
            effective["textures"] = nlohmann::json::object();
        for (auto& childItem : effective["children"].items()) {
            const auto& child = childItem.value();
            if (child.contains("textures") && child["textures"].is_object()) {
                for (auto& tex : child["textures"].items()) {
                    if (!effective["textures"].contains(tex.key()))
                        effective["textures"][tex.key()] = tex.value();
                }
            }
            if (child.contains("elements") && child["elements"].is_array()) {
                for (const auto& el : child["elements"]) flat.push_back(el);
            }
        }
        if (!flat.empty()) effective["elements"] = std::move(flat);
    }

    if (effective.contains("elements")) {
        // 处理元素生成材质数据
        processTextures(effective, data, textureKeyToMaterialIndex);

        // 处理元素生成几何数据
        processElements(effective, data, textureKeyToMaterialIndex);
    }
    else if (SpecialBlock::IsObjModelJson(effective)) {
        // forge:obj / porting_lib:obj 模型(如 Create 阀门手轮、斜坡/对角铁轨)
        data = SpecialBlock::GenerateObjModel(effective);
    }
    else {
        // 当模型中没有 "elements" 字段时,生成实体方块模型
        // 1.20-1.21 悬挂告示牌只有 particle; 26.x 自带 elements, 不会走到这里
        data = GenerateHangingSignModel(blockName);
        if (data.faces.empty()) {
            data = SpecialBlock::GenerateSpecialBlockModel(blockName);
        }
    }
    

    return data;
}

// 将model类型的json文件变为网格数据
ModelData ProcessModelJson(const std::string& namespaceName, const std::string& blockId,
    int rotationX, int rotationY, bool uvlock, int randomIndex,const std::string& blockstateName) {
    // 生成唯一缓存键(添加模型索引)
    std::string cacheKey = namespaceName + ":" + blockId + ":" + std::to_string(randomIndex);

    // 在访问缓存前加锁
    std::lock_guard<std::mutex> lock(cacheMutex);
    // 检查缓存是否存在(同时查扩展key,SpecialBlock方块如床)
    auto cacheIt = modelCache.find(cacheKey);
    if (cacheIt == modelCache.end() && !blockstateName.empty()) {
        cacheIt = modelCache.find(cacheKey + "@" + blockstateName);
    }
    if (cacheIt != modelCache.end()) {
        // 从缓存中获取原始模型数据
        ModelData cachedModel = cacheIt->second;
        if (rotationX != 0 || rotationY != 0) {
            // 使用C++20的span进行函数调用
            ApplyRotationToVertices(std::span<float>(cachedModel.vertices.data(), cachedModel.vertices.size()), rotationX, rotationY);
        }
        if (uvlock)
        {
            ApplyRotationToUV(cachedModel, rotationX, rotationY);
        }
        // 施加旋转到 faceDirections
        ApplyRotationToFaceDirections(cachedModel.faces, rotationX, rotationY);
        return cachedModel;
    }
    // 缓存未命中,正常加载模型
    nlohmann::json modelJson = GetModelJson(namespaceName, blockId);

    ModelData modelData;
    if (modelJson.is_null()) {
        return modelData;
    }
    // 递归加载父模型并合并属性
    modelJson = LoadParentModel(namespaceName, blockId, modelJson);
    
    // 处理模型数据(不包含旋转)
    modelData = ProcessModelData(modelJson,blockstateName);


    // 将原始数据存入缓存(不包含旋转)
    // SpecialBlock 方块(床等)用 blockstateName 区分缓存，避免不同颜色共用
    std::string storeKey = cacheKey;
    if (!modelJson.contains("elements") && !blockstateName.empty()) {
        storeKey = cacheKey + "@" + blockstateName;
    }
    modelCache[storeKey] = modelData;

    if (rotationX != 0 || rotationY != 0) {
        // 如果指定了旋转,则应用旋转
        ApplyRotationToVertices(std::span<float>(modelData.vertices.data(), modelData.vertices.size()), rotationX, rotationY);
    }
    if (uvlock)
    {
        ApplyRotationToUV(modelData, rotationX, rotationY);
    }

    // 施加旋转到 faceDirections
    ApplyRotationToFaceDirections(modelData.faces, rotationX, rotationY);
    return modelData;
}


//——————————————合并网格体方法———————————————

ModelData MergeModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1:顶点处理 ------------------------
    std::unordered_map<VertexKey, int> vertexMap;
    // 预估合并后顶点数量,避免重复 rehash
    vertexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);
    std::vector<float> uniqueVertices;
    uniqueVertices.reserve(data1.vertices.size() + data2.vertices.size());
    std::vector<int> vertexIndexMap;
    vertexIndexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);

    auto processVertices = [&](const std::vector<float>& vertices) {
        size_t count = vertices.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            float x = vertices[i * 3];
            float y = vertices[i * 3 + 1];
            float z = vertices[i * 3 + 2];
            // 转为整数表示(保留6位小数)
            int rx = static_cast<int>(std::round(x * 1000000.0f));
            int ry = static_cast<int>(std::round(y * 1000000.0f));
            int rz = static_cast<int>(std::round(z * 1000000.0f));
            VertexKey key{ rx, ry, rz };

            auto it = vertexMap.find(key);
            if (it == vertexMap.end()) {
                int newIndex = uniqueVertices.size() / 3;
                uniqueVertices.push_back(x);
                uniqueVertices.push_back(y);
                uniqueVertices.push_back(z);
                vertexMap[key] = newIndex;
                vertexIndexMap.push_back(newIndex);
            }
            else {
                vertexIndexMap.push_back(it->second);
            }
        }
        };

    processVertices(data1.vertices);
    processVertices(data2.vertices);
    mergedData.vertices = std::move(uniqueVertices);

    //------------------------ 阶段2:UV处理 ------------------------
    std::unordered_map<UVKey, int> uvMap;
    uvMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);
    std::vector<float> uniqueUVs;
    uniqueUVs.reserve(data1.uvCoordinates.size() + data2.uvCoordinates.size());
    std::vector<int> uvIndexMap;
    uvIndexMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);

    auto processUVs = [&](const std::vector<float>& uvs) {
        size_t count = uvs.size() / 2;
        for (size_t i = 0; i < count; ++i) {
            float u = uvs[i * 2];
            float v = uvs[i * 2 + 1];
            int ru = static_cast<int>(std::round(u * 1000000.0f));
            int rv = static_cast<int>(std::round(v * 1000000.0f));
            UVKey key{ ru, rv };

            auto it = uvMap.find(key);
            if (it == uvMap.end()) {
                int newIndex = uniqueUVs.size() / 2;
                uniqueUVs.push_back(u);
                uniqueUVs.push_back(v);
                uvMap[key] = newIndex;
                uvIndexMap.push_back(newIndex);
            }
            else {
                uvIndexMap.push_back(it->second);
            }
        }
        };

    processUVs(data1.uvCoordinates);
    processUVs(data2.uvCoordinates);
    mergedData.uvCoordinates = std::move(uniqueUVs);

    //------------------------ 阶段3:面数据处理 ------------------------
    // 先处理材质映射,以便在处理面时使用
    std::vector<int> materialIndexMap;
    {
        // 使用哈希映射快速判断 data1 中已存在的材质
        std::unordered_map<std::string, int> materialMap;
        materialMap.reserve(data1.materials.size());
        mergedData.materials = data1.materials;

        for (size_t i = 0; i < data1.materials.size(); i++) {
            materialMap[data1.materials[i].name] = static_cast<int>(i);
        }

        materialIndexMap.resize(data2.materials.size(), -1);
        for (size_t i = 0; i < data2.materials.size(); ++i) {
            auto it = materialMap.find(data2.materials[i].name);
            if (it != materialMap.end()) {
                materialIndexMap[i] = it->second;
            }
            else {
                int newIndex = mergedData.materials.size();
                materialMap[data2.materials[i].name] = newIndex;
                materialIndexMap[i] = newIndex;
                mergedData.materials.push_back(data2.materials[i]);
            }
        }
    }

    // 面和 UV 面数据合并时直接使用映射后的索引
    auto remapFaces = [&](const std::vector<Face>& faces, bool isData1) {
        // data2 的顶点需要加上 data1 原始顶点数偏移
        int vertexIndexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        int uvIndexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        
        for (const auto& face : faces) {
            Face newFace;
            
            // 重映射顶点索引
            for (int j = 0; j < 4; ++j) {
                int originalVertIndex = face.vertexIndices[j];
                int globalVertIndex = originalVertIndex + (isData1 ? 0 : vertexIndexOffset);
                if (globalVertIndex >= 0 && globalVertIndex < vertexIndexMap.size()) {
                    newFace.vertexIndices[j] = vertexIndexMap[globalVertIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.vertexIndices[j] = 0;
                }
            }
            
            // 重映射UV索引
            for (int j = 0; j < 4; ++j) {
                int originalUVIndex = face.uvIndices[j];
                int globalUVIndex = originalUVIndex + (isData1 ? 0 : uvIndexOffset);
                if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                    newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.uvIndices[j] = 0;
                }
            }
            
            // 重映射材质索引
            if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                newFace.materialIndex = materialIndexMap[face.materialIndex];
            } else {
                newFace.materialIndex = face.materialIndex;
            }
            
            // 保留面方向
            newFace.faceDirection = face.faceDirection;
            newFace.tintIndex = face.tintIndex;
            newFace.bothSided = face.bothSided;
            newFace.thinEdge = face.thinEdge;
            
            mergedData.faces.push_back(newFace);
        }
    };

    remapFaces(data1.faces, true);
    remapFaces(data2.faces, false);

    // 叠加层配对随面索引平移(data1 面在前保持不变, data2 面整体偏移)
    mergedData.overlayPairs = data1.overlayPairs;
    const int overlayFaceOffset = static_cast<int>(data1.faces.size());
    for (auto pair : data2.overlayPairs) {
        pair.baseFace += overlayFaceOffset;
        pair.overlayFace += overlayFaceOffset;
        mergedData.overlayPairs.push_back(pair);
    }

    //------------------------ 阶段4:材质数据合并 ------------------------
    // 材质数据已在上面处理完成

    //------------------------ 阶段5:合并其他数据 ------------------------
    // 合并处理tintIndex
    // 注意:这里使用新的方式处理tint索引,我们需要遍历所有材质检查tintIndex
    bool hasTintIndex1 = false;
    for (const auto& material : data1.materials)
    {
        if (material.tintIndex != -1)
        {
            hasTintIndex1 = true;
            break;
        }
    }
    // 如果data1没有tint索引,检查data2
    if (!hasTintIndex1)
    {
        for (size_t i = 0; i < data2.materials.size(); ++i)
        {
            if (data2.materials[i].tintIndex != -1 && i < materialIndexMap.size())
            {
                int targetIndex = materialIndexMap[i];
                if (targetIndex >= 0 && targetIndex < mergedData.materials.size())
                {
                    mergedData.materials[targetIndex].tintIndex = data2.materials[i].tintIndex;
                }
            }
        }
    }

    return mergedData;
}

// 以下方法用于合并两个模型数据,针对流体/水方块模型,对重复面做严格剔除:
// 如果 mesh2 的面被包含在 mesh1 的某个面内,则跳过该面及其对应数据
ModelData MergeFluidModelData(const ModelData& data1, const ModelData& data2) {
    ModelData mergedData;

    //------------------------ 阶段1:顶点处理 ------------------------
    std::unordered_map<VertexKey, int> vertexMap;
    vertexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);
    std::vector<float> uniqueVertices;
    uniqueVertices.reserve(data1.vertices.size() + data2.vertices.size());
    std::vector<int> vertexIndexMap;
    vertexIndexMap.reserve((data1.vertices.size() + data2.vertices.size()) / 3);

    auto processVertices = [&](const std::vector<float>& vertices) {
        size_t count = vertices.size() / 3;
        for (size_t i = 0; i < count; ++i) {
            float x = vertices[i * 3];
            float y = vertices[i * 3 + 1];
            float z = vertices[i * 3 + 2];
            // 转为整数表示(保留6位小数)
            int rx = static_cast<int>(std::round(x * 1000000.0f));
            int ry = static_cast<int>(std::round(y * 1000000.0f));
            int rz = static_cast<int>(std::round(z * 1000000.0f));
            VertexKey key{ rx, ry, rz };

            auto it = vertexMap.find(key);
            if (it == vertexMap.end()) {
                int newIndex = uniqueVertices.size() / 3;
                uniqueVertices.push_back(x);
                uniqueVertices.push_back(y);
                uniqueVertices.push_back(z);
                vertexMap[key] = newIndex;
                vertexIndexMap.push_back(newIndex);
            }
            else {
                vertexIndexMap.push_back(it->second);
            }
        }
        };

    processVertices(data1.vertices);
    processVertices(data2.vertices);
    mergedData.vertices = std::move(uniqueVertices);

    //------------------------ 阶段2:UV处理 ------------------------
    std::unordered_map<UVKey, int> uvMap;
    uvMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);
    std::vector<float> uniqueUVs;
    uniqueUVs.reserve(data1.uvCoordinates.size() + data2.uvCoordinates.size());
    std::vector<int> uvIndexMap;
    uvIndexMap.reserve((data1.uvCoordinates.size() + data2.uvCoordinates.size()) / 2);

    auto processUVs = [&](const std::vector<float>& uvs) {
        size_t count = uvs.size() / 2;
        for (size_t i = 0; i < count; ++i) {
            float u = uvs[i * 2];
            float v = uvs[i * 2 + 1];
            int ru = static_cast<int>(std::round(u * 1000000.0f));
            int rv = static_cast<int>(std::round(v * 1000000.0f));
            UVKey key{ ru, rv };

            auto it = uvMap.find(key);
            if (it == uvMap.end()) {
                int newIndex = uniqueUVs.size() / 2;
                uniqueUVs.push_back(u);
                uniqueUVs.push_back(v);
                uvMap[key] = newIndex;
                uvIndexMap.push_back(newIndex);
            }
            else {
                uvIndexMap.push_back(it->second);
            }
        }
        };

    processUVs(data1.uvCoordinates);
    processUVs(data2.uvCoordinates);
    mergedData.uvCoordinates = std::move(uniqueUVs);

    //------------------------ 阶段3:材质数据处理 ------------------------
    std::unordered_map<std::string, int> materialMap;
    materialMap.reserve(data1.materials.size());
    mergedData.materials = data1.materials;
    for (size_t i = 0; i < data1.materials.size(); i++) {
        materialMap[data1.materials[i].name] = static_cast<int>(i);
    }
    std::vector<int> materialIndexMap; // 用于记录 data2 材质索引映射
    materialIndexMap.resize(data2.materials.size(), -1);
    for (size_t i = 0; i < data2.materials.size(); i++) {
        auto it = materialMap.find(data2.materials[i].name);
        if (it != materialMap.end()) {
            materialIndexMap[i] = it->second;
        }
        else {
            int newIndex = mergedData.materials.size();
            materialMap[data2.materials[i].name] = newIndex;
            materialIndexMap[i] = newIndex;
            mergedData.materials.push_back(data2.materials[i]);
        }
    }

    //------------------------ 阶段4:网格体1面数据处理 ------------------------
    // 直接将 data1 的面(及 UV 面)数据进行重映射后加入 mergedData
    auto remapFaces = [&](const std::vector<Face>& faces, bool isData1) {
        // data2 的顶点需要加上 data1 原始顶点数偏移
        int vertexIndexOffset = isData1 ? 0 : data1.vertices.size() / 3;
        int uvIndexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        
        for (const auto& face : faces) {
            Face newFace;
            
            // 重映射顶点索引
            for (int j = 0; j < 4; ++j) {
                int originalVertIndex = face.vertexIndices[j];
                int globalVertIndex = originalVertIndex + (isData1 ? 0 : vertexIndexOffset);
                if (globalVertIndex >= 0 && globalVertIndex < vertexIndexMap.size()) {
                    newFace.vertexIndices[j] = vertexIndexMap[globalVertIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.vertexIndices[j] = 0;
                }
            }
            
            // 重映射UV索引
            for (int j = 0; j < 4; ++j) {
                int originalUVIndex = face.uvIndices[j];
                int globalUVIndex = originalUVIndex + (isData1 ? 0 : uvIndexOffset);
                if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                    newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                } else {
                    // 索引无效的情况处理
                    newFace.uvIndices[j] = 0;
                }
            }
            
            // 重映射材质索引
            if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                newFace.materialIndex = materialIndexMap[face.materialIndex];
            } else {
                newFace.materialIndex = face.materialIndex;
            }
            
            // 保留面方向
            newFace.faceDirection = face.faceDirection;
            newFace.tintIndex = face.tintIndex;
            newFace.bothSided = face.bothSided;
            newFace.thinEdge = face.thinEdge;
            
            mergedData.faces.push_back(newFace);
        }
    };

    auto remapUVFaces = [&](const std::vector<Face>& faces, bool isData1) {
        int indexOffset = isData1 ? 0 : data1.uvCoordinates.size() / 2;
        for (size_t faceIndex = 0; faceIndex < faces.size(); ++faceIndex) {
            const auto& face = faces[faceIndex];
            // 获取对应的新面索引
            size_t newFaceIndex = isData1 ? faceIndex : (data1.faces.size() + faceIndex);
            
            // 确保新面索引在合法范围内
            if (newFaceIndex < mergedData.faces.size()) {
                Face& newFace = mergedData.faces[newFaceIndex];
                
                // 重映射UV索引
                for (int j = 0; j < 4; ++j) {
                    int originalUVIndex = face.uvIndices[j];
                    int globalUVIndex = originalUVIndex + (isData1 ? 0 : indexOffset);
                    
                    // 边界检查
                    if (globalUVIndex >= 0 && globalUVIndex < uvIndexMap.size()) {
                        newFace.uvIndices[j] = uvIndexMap[globalUVIndex];
                    } else {
                        // 处理无效索引
                        newFace.uvIndices[j] = 0;
                    }
                }
                
                // 重映射材质索引
                if (!isData1 && face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
                    newFace.materialIndex = materialIndexMap[face.materialIndex];
                } else {
                    newFace.materialIndex = face.materialIndex;
                }
                
                // 保留面方向
                newFace.faceDirection = face.faceDirection;
            }
        }
    };

    // mesh1 的面数据直接加入
    remapFaces(data1.faces, true);
    remapUVFaces(data1.faces, true);
    int mesh1FaceCount = data1.faces.size(); // mesh1 面数量

    //------------------------ 阶段5:辅助几何检测函数 ------------------------

    // 计算面(四边形)的法向,使用前三个顶点
    auto computeNormal = [&](const std::vector<float>& vertices, const std::array<int, 4>& faceIndices) -> std::array<float, 3> {
        float x0 = vertices[faceIndices[0] * 3];
        float y0 = vertices[faceIndices[0] * 3 + 1];
        float z0 = vertices[faceIndices[0] * 3 + 2];
        float x1 = vertices[faceIndices[1] * 3];
        float y1 = vertices[faceIndices[1] * 3 + 1];
        float z1 = vertices[faceIndices[1] * 3 + 2];
        float x2 = vertices[faceIndices[2] * 3];
        float y2 = vertices[faceIndices[2] * 3 + 1];
        float z2 = vertices[faceIndices[2] * 3 + 2];
        float ux = x1 - x0, uy = y1 - y0, uz = z1 - z0;
        float vx = x2 - x0, vy = y2 - y0, vz = z2 - z0;
        std::array<float, 3> normal = { uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx };
        float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (len > 1e-6f) {
            normal[0] /= len; normal[1] /= len; normal[2] /= len;
        }
        return normal;
        };

    // 根据法向判断投影时丢弃哪个坐标轴:返回 0 表示丢弃 x,1 表示丢弃 y,2 表示丢弃 z
    auto determineDropAxis = [&](const std::array<float, 3>& normal) -> int {
        float absX = std::fabs(normal[0]);
        float absY = std::fabs(normal[1]);
        float absZ = std::fabs(normal[2]);
        if (absX >= absY && absX >= absZ)
            return 0;
        else if (absY >= absX && absY >= absZ)
            return 1;
        else
            return 2;
        };

    // 将某个顶点投影到二维平面,dropAxis 指定丢弃的坐标
    auto projectPoint = [&](const std::vector<float>& vertices, int vertexIndex, int dropAxis) -> std::pair<float, float> {
        float x = vertices[vertexIndex * 3];
        float y = vertices[vertexIndex * 3 + 1];
        float z = vertices[vertexIndex * 3 + 2];
        if (dropAxis == 0) return { y, z };
        else if (dropAxis == 1) return { x, z };
        else return { x, y };
        };

    // 判断二维点 p 是否在三角形 (a, b, c) 内(采用重心坐标法)
    auto pointInTriangle = [&](const std::pair<float, float>& p,
        const std::pair<float, float>& a,
        const std::pair<float, float>& b,
        const std::pair<float, float>& c) -> bool {
            float denom = (b.second - c.second) * (a.first - c.first) + (c.first - b.first) * (a.second - c.second);
            if (std::fabs(denom) < 1e-6f) return false;
            float alpha = ((b.second - c.second) * (p.first - c.first) + (c.first - b.first) * (p.second - c.second)) / denom;
            float beta = ((c.second - a.second) * (p.first - c.first) + (a.first - c.first) * (p.second - c.second)) / denom;
            float gamma = 1.0f - alpha - beta;
            return (alpha >= 0.0f && beta >= 0.0f && gamma >= 0.0f);
        };

    // 判断二维点 p 是否在四边形内(将四边形分解为两个三角形)
    auto pointInQuad = [&](const std::array<int, 4>& quadIndices, const std::vector<float>& vertices, const std::pair<float, float>& p, int dropAxis) -> bool {
        std::vector<std::pair<float, float>> proj;
        for (int idx : quadIndices) {
            proj.push_back(projectPoint(vertices, idx, dropAxis));
        }
        if (pointInTriangle(p, proj[0], proj[1], proj[2])) return true;
        if (pointInTriangle(p, proj[0], proj[2], proj[3])) return true;
        return false;
        };

    // 判断 mesh2 的某个面(给定其4个顶点索引,均为 mergedData.vertices 的下标)是否被包含在 mesh1 的任一面内
    auto isFaceContained = [&](const std::array<int, 4>& faceIndices2) -> bool {
        // 遍历所有 mesh1 的面(已在 mergedData.faces 中,前 mesh1FaceCount 个面)
        for (size_t i = 0; i < static_cast<size_t>(mesh1FaceCount); i++) {
            const auto& faceIndices1 = mergedData.faces[i].vertexIndices;
            auto normal = computeNormal(mergedData.vertices, faceIndices1);
            const int planeVertex = faceIndices1[0];
            const float planeX = mergedData.vertices[planeVertex * 3];
            const float planeY = mergedData.vertices[planeVertex * 3 + 1];
            const float planeZ = mergedData.vertices[planeVertex * 3 + 2];
            bool coplanar = true;
            for (int idx : faceIndices2) {
                const float dx = mergedData.vertices[idx * 3] - planeX;
                const float dy = mergedData.vertices[idx * 3 + 1] - planeY;
                const float dz = mergedData.vertices[idx * 3 + 2] - planeZ;
                if (std::fabs(normal[0] * dx + normal[1] * dy + normal[2] * dz) > 1e-5f) {
                    coplanar = false;
                    break;
                }
            }
            if (!coplanar) continue;
            int dropAxis = determineDropAxis(normal);
            bool allInside = true;
            for (int idx : faceIndices2) {
                auto p = projectPoint(mergedData.vertices, idx, dropAxis);
                if (!pointInQuad(faceIndices1, mergedData.vertices, p, dropAxis)) {
                    allInside = false;
                    break;
                }
            }
            if (allInside)
                return true;
        }
        return false;
        };

    //------------------------ 阶段6:网格体2面数据处理(严格检查重复面) ------------------------
    int data2FaceCount = data2.faces.size();
    int data2VertexOffset = data1.vertices.size() / 3;
    int data2UVOffset = data1.uvCoordinates.size() / 2;
    std::vector<int> data2FaceNewIndex(data2.faces.size(), -1);
    for (size_t i = 0; i < static_cast<size_t>(data2FaceCount); i++) {
        // 重映射 mesh2 面的顶点索引
        std::array<int, 4> faceIndices;
        for (int j = 0; j < 4; j++) {
            int originalIndex = data2.faces[i].vertexIndices[j] + data2VertexOffset;
            faceIndices[j] = vertexIndexMap[originalIndex];
        }

        // 获取 data2 对应面的 faceDirections(假设每个面有4个方向信息,索引为 i*4 ~ i*4+3)
        // 根据 Face 结构体获取剔除标志
        bool doNotCull = (data2.faces[i].faceDirection == FaceType::DO_NOT_CULL);

        // 如果不标记 DO_NOT_CULL 且该面完全被包含在 mesh1 的某个面内,则跳过该面
        if (!doNotCull && isFaceContained(faceIndices))
            continue;

        // 添加该面到合并结果
        Face newFace;
        newFace.vertexIndices = faceIndices;
        newFace.materialIndex = (data2.faces[i].materialIndex != -1) ? materialIndexMap[data2.faces[i].materialIndex] : -1;
        newFace.faceDirection = data2.faces[i].faceDirection;
        newFace.tintIndex = data2.faces[i].tintIndex;
        newFace.bothSided = data2.faces[i].bothSided;
        newFace.thinEdge = data2.faces[i].thinEdge;
        data2FaceNewIndex[i] = static_cast<int>(mergedData.faces.size());
        mergedData.faces.push_back(newFace);

        // 对应的UV面处理
        std::array<int, 4> uvFaceIndices;
        for (int j = 0; j < 4; j++) {
            int originalUVIndex = data2.faces[i].uvIndices[j] + data2UVOffset;
            uvFaceIndices[j] = uvIndexMap[originalUVIndex];
        }
        mergedData.faces.back().uvIndices = uvFaceIndices;
    }

    // 叠加层配对:data1 面 1:1 保留;data2 面被包含剔除时配对一并丢弃
    mergedData.overlayPairs = data1.overlayPairs;
    for (const auto& pair : data2.overlayPairs) {
        if (pair.baseFace < 0 || pair.overlayFace < 0 ||
            pair.baseFace >= data2FaceCount || pair.overlayFace >= data2FaceCount) {
            continue;
        }
        int baseNew = data2FaceNewIndex[pair.baseFace];
        int overlayNew = data2FaceNewIndex[pair.overlayFace];
        if (baseNew >= 0 && overlayNew >= 0) {
            mergedData.overlayPairs.push_back({ baseNew, overlayNew });
        }
    }

    return mergedData;
}

void MergeModelsDirectly(ModelData& data1, const ModelData& data2,
    std::unordered_map<std::string, int>* materialLookup) {
    // Empty sections contribute neither geometry nor materials to the mesh.
    if (data2.faces.empty()) return;
    // 优化:预分配并按倍增扩容,减少内存重分配
    {
        size_t oldV = data1.vertices.size();
        size_t addV = data2.vertices.size();
        size_t needV = oldV + addV;
        size_t capV = data1.vertices.capacity();
        if (needV > capV) {
            size_t newCapV = capV * 2 > needV ? capV * 2 : needV;
            data1.vertices.reserve(newCapV);
        }
    }
    {
        size_t oldUV = data1.uvCoordinates.size();
        size_t addUV = data2.uvCoordinates.size();
        size_t needUV = oldUV + addUV;
        size_t capUV = data1.uvCoordinates.capacity();
        if (needUV > capUV) {
            size_t newCapUV = capUV * 2 > needUV ? capUV * 2 : needUV;
            data1.uvCoordinates.reserve(newCapUV);
        }
    }
    {
        size_t oldMat = data1.materials.size();
        size_t addMat = data2.materials.size();
        size_t needMat = oldMat + addMat;
        size_t capMat = data1.materials.capacity();
        if (needMat > capMat) {
            size_t newCapMat = capMat * 2 > needMat ? capMat * 2 : needMat;
            data1.materials.reserve(newCapMat);
        }
    }
    {
        size_t oldF = data1.faces.size();
        size_t addF = data2.faces.size();
        size_t needF = oldF + addF;
        size_t capF = data1.faces.capacity();
        if (needF > capF) {
            size_t newCapF = capF * 2 > needF ? capF * 2 : needF;
            data1.faces.reserve(newCapF);
        }
    }

    // 顶点偏移
    const size_t vertexOffset = data1.vertices.size() / 3;

    // 复制顶点数据
    data1.vertices.insert(data1.vertices.end(), 
        data2.vertices.begin(), data2.vertices.end());

    // UV坐标偏移
    const size_t uvOffset = data1.uvCoordinates.size() / 2;
    
    // 复制UV坐标
    data1.uvCoordinates.insert(data1.uvCoordinates.end(),
        data2.uvCoordinates.begin(), data2.uvCoordinates.end());

    // 材质索引映射
    std::unordered_map<std::string, int> temporaryLookup;
    auto& materialNameMap = materialLookup ? *materialLookup : temporaryLookup;
    if (materialNameMap.empty()) {
        materialNameMap.reserve(data1.materials.size() + data2.materials.size());
        for (size_t i = 0; i < data1.materials.size(); ++i) {
            materialNameMap[data1.materials[i].name] = static_cast<int>(i);
        }
    }

    std::vector<int> materialIndexMap(data2.materials.size(), -1);
    for (size_t i = 0; i < data2.materials.size(); ++i) {
        auto it = materialNameMap.find(data2.materials[i].name);
        if (it != materialNameMap.end()) {
            materialIndexMap[i] = it->second;
        } else {
            materialIndexMap[i] = static_cast<int>(data1.materials.size());
            materialNameMap.emplace(data2.materials[i].name, materialIndexMap[i]);
            data1.materials.push_back(data2.materials[i]);
        }
    }

    // 复制面数据
    for (const auto& face : data2.faces) {
        Face newFace;
        
        // 顶点索引偏移
        for (int j = 0; j < 4; ++j) {
            newFace.vertexIndices[j] = face.vertexIndices[j] + vertexOffset;
        }
        
        // UV索引偏移
        for (int j = 0; j < 4; ++j) {
            newFace.uvIndices[j] = face.uvIndices[j] + uvOffset;
        }
        
        // 材质索引映射
        if (face.materialIndex >= 0 && face.materialIndex < materialIndexMap.size()) {
            newFace.materialIndex = materialIndexMap[face.materialIndex];
        } else {
            newFace.materialIndex = 0; // 默认第一个材质
        }
        
        // 保留面方向
        newFace.faceDirection = face.faceDirection;
        newFace.tintIndex = face.tintIndex;
        // 双面几何标记必须跟着面一起搬，否则合并后全部丢失(doubleSided.json 会变空)
        newFace.bothSided = face.bothSided;
        newFace.thinEdge = face.thinEdge;
        
        data1.faces.push_back(newFace);
    }

    // 叠加层配对随面索引平移(data2 面整体追加到 data1 之后)
    if (!data2.overlayPairs.empty()) {
        const int overlayBaseOffset = static_cast<int>(data1.faces.size()) -
            static_cast<int>(data2.faces.size());
        for (auto pair : data2.overlayPairs) {
            pair.baseFace += overlayBaseOffset;
            pair.overlayFace += overlayBaseOffset;
            data1.overlayPairs.push_back(pair);
        }
    }
}

// 剪掉没有任何面引用的材质, 并重映射面的材质索引(见 model.h 说明)
size_t PruneUnusedMaterials(ModelData& model, const std::unordered_set<std::string>* keepNames) {
    const int materialCount = static_cast<int>(model.materials.size());
    if (materialCount == 0) return 0;

    std::vector<char> used(materialCount, 0);
    for (const auto& face : model.faces) {
        if (face.materialIndex >= 0 && face.materialIndex < materialCount) {
            used[face.materialIndex] = 1;
        }
    }
    // overlay 等元数据按材质名索引: 这些名字即便当前没有面引用也必须保留
    if (keepNames != nullptr && !keepNames->empty()) {
        for (int i = 0; i < materialCount; ++i) {
            if (keepNames->count(model.materials[i].name)) used[i] = 1;
        }
    }

    size_t removed = 0;
    for (int i = 0; i < materialCount; ++i) {
        if (!used[i]) ++removed;
    }
    if (removed == 0) return 0;

    std::vector<int> remap(materialCount, -1);
    std::vector<Material> kept;
    kept.reserve(static_cast<size_t>(materialCount) - removed);
    for (int i = 0; i < materialCount; ++i) {
        if (!used[i]) continue;
        remap[i] = static_cast<int>(kept.size());
        kept.push_back(std::move(model.materials[i]));
    }
    model.materials.swap(kept);

    for (auto& face : model.faces) {
        if (face.materialIndex >= 0 && face.materialIndex < materialCount) {
            face.materialIndex = remap[face.materialIndex];
        }
    }
    return removed;
}

// Blender 的网格校验按"顶点索引集合"判重（不看绕序），会把共面正反两面删成一张。
// 让同一组重合面里第二张及以后的面各持有一份独立顶点，索引集合就不同了，Blender 会保留双面几何。
// config.doubleSidedFaceOffset > 0 时，这份新顶点还会沿该面法线外移一点（两面分开 offset，
// 关闭背面剔除时不再 z-fighting）；= 0 时坐标保持严格共面，只换索引。
namespace {
struct FaceVertexSet {
    int v[4];
    bool operator==(const FaceVertexSet& other) const {
        return v[0] == other.v[0] && v[1] == other.v[1] &&
               v[2] == other.v[2] && v[3] == other.v[3];
    }
};
struct FaceVertexSetHash {
    size_t operator()(const FaceVertexSet& key) const {
        size_t h = 1469598103934665603ull;
        for (int i = 0; i < 4; ++i) {
            h ^= static_cast<size_t>(static_cast<unsigned int>(key.v[i]));
            h *= 1099511628211ull;
        }
        return h;
    }
};

// 取面 4 个角的 UV；索引越界（无贴图/损坏数据）时返回 false。
bool GetFaceUvQuad(const ModelData& model, const Face& face,
    std::array<std::array<float, 2>, 4>& uv)
{
    for (int k = 0; k < 4; ++k) {
        const int idx = face.uvIndices[k];
        if (idx < 0 || static_cast<size_t>(idx) * 2 + 1 >= model.uvCoordinates.size()) return false;
        uv[k][0] = model.uvCoordinates[static_cast<size_t>(idx) * 2];
        uv[k][1] = model.uvCoordinates[static_cast<size_t>(idx) * 2 + 1];
    }
    return true;
}

// 判断一对共面正反面的 UV 关系。Same 才能安全合并：两面世界 UV 同向，
// 删掉一张后剩下一张走双面渲染，从背面看到的镜像与原背面那张完全一致。
DoubleSidedPairKind ClassifyDoubleSidedPair(const ModelData& model, const Face& a, const Face& b)
{
    // 材质必须完全一致（贴图、tint 变体、类型都算），否则合并会丢贴图/颜色。
    if (a.materialIndex < 0 || a.materialIndex != b.materialIndex) return DoubleSidedPairKind::Other;
    if (a.tintIndex != b.tintIndex) return DoubleSidedPairKind::Other;

    std::array<std::array<float, 2>, 4> ua{}, ub{};
    if (!GetFaceUvQuad(model, a, ua) || !GetFaceUvQuad(model, b, ub)) {
        return DoubleSidedPairKind::Other;
    }

    // 把 b 的 UV 按"同一顶点"对齐到 a 的角序上。
    std::array<std::array<float, 2>, 4> aligned{};
    for (int k = 0; k < 4; ++k) {
        int match = -1;
        for (int j = 0; j < 4; ++j) {
            if (b.vertexIndices[j] == a.vertexIndices[k]) { match = j; break; }
        }
        if (match < 0) return DoubleSidedPairKind::Other;
        aligned[k] = ub[match];
    }

    constexpr float eps = 1e-5f;
    const auto equal = [eps](const std::array<float, 2>& x, const std::array<float, 2>& y) {
        return std::fabs(x[0] - y[0]) <= eps && std::fabs(x[1] - y[1]) <= eps;
    };

    bool same = true;
    for (int k = 0; k < 4 && same; ++k) same = equal(ua[k], aligned[k]);
    if (same) return DoubleSidedPairKind::Same;

    // 绕序相反 + 同一组 UV 四角 => 两面是同贴图区域的镜像关系（u 翻/v 翻/对角翻都可能）。
    // 用四角值的多重集判断，比枚举排列更耐受 rotation/flip 带来的角序变化。
    std::array<std::array<float, 2>, 4> sa = ua, sb = aligned;
    const auto byUv = [](const std::array<float, 2>& x, const std::array<float, 2>& y) {
        if (x[0] != y[0]) return x[0] < y[0];
        return x[1] < y[1];
    };
    std::sort(sa.begin(), sa.end(), byUv);
    std::sort(sb.begin(), sb.end(), byUv);
    bool mirror = true;
    for (int k = 0; k < 4 && mirror; ++k) mirror = equal(sa[k], sb[k]);
    return mirror ? DoubleSidedPairKind::Mirror : DoubleSidedPairKind::Other;
}

// 两面的 UV 四角值是否构成同一个多重集（忽略方向/旋转），供"同贴图就合并"模式使用。
bool UvQuadsHaveSameCorners(const ModelData& model, const Face& a, const Face& b)
{
    std::array<std::array<float, 2>, 4> ua{}, ub{};
    if (!GetFaceUvQuad(model, a, ua) || !GetFaceUvQuad(model, b, ub)) return false;
    const auto byUv = [](const std::array<float, 2>& x, const std::array<float, 2>& y) {
        if (x[0] != y[0]) return x[0] < y[0];
        return x[1] < y[1];
    };
    std::sort(ua.begin(), ua.end(), byUv);
    std::sort(ub.begin(), ub.end(), byUv);
    constexpr float eps = 1e-5f;
    for (int k = 0; k < 4; ++k) {
        if (std::fabs(ua[k][0] - ub[k][0]) > eps ||
            std::fabs(ua[k][1] - ub[k][1]) > eps) return false;
    }
    return true;
}

// "同贴图就合并"模式的判定：贴图路径与材质类型一致、面级 tint 一致、UV 四角值相同。
// 不要求材质索引相同（模组里同一张贴图可能有多个材质名），也不要求 UV 方向一致。
bool PairSharesSameTextureAndUv(const ModelData& model, const Face& a, const Face& b)
{
    if (a.materialIndex < 0 || b.materialIndex < 0) return false;
    if (static_cast<size_t>(a.materialIndex) >= model.materials.size() ||
        static_cast<size_t>(b.materialIndex) >= model.materials.size()) return false;
    const Material& ma = model.materials[a.materialIndex];
    const Material& mb = model.materials[b.materialIndex];
    if (ma.texturePath.empty() || ma.texturePath != mb.texturePath) return false;
    if (ma.type != mb.type) return false;
    if (ma.tint != mb.tint) return false;
    if (a.tintIndex != b.tintIndex) return false;
    return UvQuadsHaveSameCorners(model, a, b);
}
}

size_t SeparateCoincidentFaces(ModelData& model) {
    if (model.faces.size() < 2) return 0;
    const float offset = config.doubleSidedFaceOffset;
    // 合并开关：只在"几何双面"开启时才有意义（关闭时解析阶段已经把重合对删成一张）。
    const bool mergeSame = config.doubleSidedGeometry && config.mergeSameDoubleSided;
    const bool mergeMirror = config.doubleSidedGeometry && config.mergeMirrorDoubleSided;
    // 按材质(剥掉 @tint 后缀的基础名)累计面数: 分离出的重合面数 == 总面数 时该材质
    // 整个都是双面几何, 否则说明它还用在实心几何上, 整材质剔除会漏光, 需要告警。
    std::unordered_map<std::string, size_t> totalFacesMap;
    std::unordered_map<std::string, size_t> bothSidedFacesMap;
    totalFacesMap.reserve(model.materials.size() * 2 + 1);

    // 每个顶点组记录组内第一个双面几何面，用于配对（避免把相邻方块的接触面当成双面几何）。
    struct SeenInfo {
        int firstBothSided = -1;
        bool paired = false;   // 该组已经配对过一对，避免重复统计/再次合并
    };
    std::unordered_map<FaceVertexSet, SeenInfo, FaceVertexSetHash> seen;
    seen.reserve(model.faces.size());
    std::vector<char> removeMask(model.faces.size(), 0);
    size_t separated = 0;
    size_t mergedFaces = 0;
    for (size_t fi = 0; fi < model.faces.size(); ++fi) {
        Face& face = model.faces[fi];
        // 与下方分离流程共用一次遍历: 每个面都先计入总数
        const std::string materialBaseName =
            (face.materialIndex >= 0 && static_cast<size_t>(face.materialIndex) < model.materials.size())
            ? StripTintSuffix(model.materials[face.materialIndex].name)
            : std::string();
        if (!materialBaseName.empty()) ++totalFacesMap[materialBaseName];

        FaceVertexSet key;
        for (int k = 0; k < 4; ++k) key.v[k] = face.vertexIndices[k];
        std::sort(key.v, key.v + 4);

        auto seenIt = seen.find(key);
        if (seenIt == seen.end()) {
            SeenInfo info;
            if (face.bothSided) info.firstBothSided = static_cast<int>(fi);
            seen.emplace(key, info);
            continue;   // 这组顶点第一次出现
        }
        SeenInfo& info = seenIt->second;

        // 双面几何配对：同组第一个双面面 + 当前双面面。分类统计与合并开关无关，
        // 这样 doubleSided.json 始终能告诉插件端 Same/Mirror 各有多少对。
        if (face.bothSided && info.firstBothSided >= 0 &&
            info.firstBothSided != static_cast<int>(fi) && !info.paired) {
            const Face& partner = model.faces[info.firstBothSided];
            const DoubleSidedPairKind kind = ClassifyDoubleSidedPair(model, partner, face);
            info.paired = true;
            // 两面都要登记材质（Classify 返回 Other 时两面材质可能不同），
            // 一对双面几何共 2 面，下面"实心几何混用"的告警才能和 totalFacesMap 同口径。
            const auto registerPairFace = [&](const Face& f) {
                if (f.materialIndex < 0 ||
                    static_cast<size_t>(f.materialIndex) >= model.materials.size()) return;
                const std::string base = StripTintSuffix(model.materials[f.materialIndex].name);
                if (base.empty()) return;
                ++bothSidedFacesMap[base];
                RegisterDoubleSidedMaterial(model.materials[f.materialIndex]);
            };
            registerPairFace(partner);
            registerPairFace(face);
            if (!materialBaseName.empty()) {
                RegisterDoubleSidedPairKind(model.materials[face.materialIndex], kind, false);
            }
            // 合并条件：
            //  - Same 模式只合并 UV 一一对应的对（视觉等价）；
            //  - Mirror 模式只合并同贴图但方向不同的对（接受一面变镜像），不包含同向。
            bool canMerge = mergeSame && kind == DoubleSidedPairKind::Same;
            if (!canMerge && mergeMirror) {
                canMerge = kind != DoubleSidedPairKind::Same &&
                           PairSharesSameTextureAndUv(model, partner, face);
            }
            if (canMerge) {
                // 删掉当前面，保留 partner（firstBothSided）；剩一张靠背面剔除显示双面。
                removeMask[fi] = 1;
                ++mergedFaces;
                if (partner.materialIndex >= 0 &&
                    static_cast<size_t>(partner.materialIndex) < model.materials.size()) {
                    RegisterDoubleSidedPairKind(model.materials[partner.materialIndex], kind, true);
                }
                continue;   // 合并掉的面不再分离
            }
        }
        else if (face.bothSided && info.firstBothSided < 0) {
            // 组里第一个双面面本身是重复面（首面是相邻方块的实心接触面）：记为配对基准。
            info.firstBothSided = static_cast<int>(fi);
        }

        float p[4][3];
        bool valid = true;
        for (int k = 0; k < 4 && valid; ++k) {
            const int idx = face.vertexIndices[k];
            if (idx < 0 || static_cast<size_t>(idx) * 3 + 2 >= model.vertices.size()) {
                valid = false;
                break;
            }
            const size_t base = static_cast<size_t>(idx) * 3;
            for (int c = 0; c < 3; ++c) p[k][c] = model.vertices[base + c];
        }
        if (!valid) continue;

        // 面法线（按绕序，右手定则）
        float n[3] = { 0.0f, 0.0f, 0.0f };
        if (offset > 0.0f) {
            const float e1[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
            const float e2[3] = { p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2] };
            n[0] = e1[1] * e2[2] - e1[2] * e2[1];
            n[1] = e1[2] * e2[0] - e1[0] * e2[2];
            n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 1e-12f) {
                n[0] /= len; n[1] /= len; n[2] /= len;
            } else {
                n[0] = n[1] = n[2] = 0.0f;   // 退化面：只换索引，不偏移
            }
        }

        if (n[0] != 0.0f || n[1] != 0.0f || n[2] != 0.0f) {
            // 4 个顶点各自复制一份并沿法线外移，面保持平面
            for (int k = 0; k < 4; ++k) {
                const int newIndex = static_cast<int>(model.vertices.size() / 3);
                model.vertices.push_back(p[k][0] + n[0] * offset);
                model.vertices.push_back(p[k][1] + n[1] * offset);
                model.vertices.push_back(p[k][2] + n[2] * offset);
                face.vertexIndices[k] = newIndex;
            }
        } else {
            // 只复制第一个顶点：索引集合不同即可，坐标严格共面
            const int newIndex = static_cast<int>(model.vertices.size() / 3);
            model.vertices.push_back(p[0][0]);
            model.vertices.push_back(p[0][1]);
            model.vertices.push_back(p[0][2]);
            face.vertexIndices[0] = newIndex;
        }
        // 双面几何的统计与材质登记已在配对时完成（一对 2 面）；这里只累计被分离的面数。
        // 相邻实心方块的接触面同样会走到这里，但它们没有 bothSided 标记，不会进 doubleSided.json。
        ++separated;
    }

    // 只对"同一材质还用在实心几何上"的情况告警: 整材质关背面剔除会让实心面漏光。
    // 这里只是提示, 不影响 doubleSided.json 的内容(清单照常收录该材质)。
    for (const auto& entry : bothSidedFacesMap) {
        const auto totalIt = totalFacesMap.find(entry.first);
        const size_t total = (totalIt != totalFacesMap.end()) ? totalIt->second : 0;
        if (total > entry.second) {
            std::cerr << "[WARN] 材质 " << entry.first << " 同时用于实心几何(双面 "
                << entry.second << " / 共 " << total
                << " 面), 整材质剔除背面可能漏光" << std::endl;
        }
    }

    // 诊断: 记录本次分离的每个材质的(bothSided 面数 / 总面数)。
    // 用于排查"某材质的双面几何没进 doubleSided.json"这类问题:
    // 从 ds_diag.txt 里搜材质名, 有记录=标记正常但收录/匹配有问题; 无记录=标记没打上。
    {
        static std::mutex diagMutex;
        static std::ofstream diagFile;
        std::lock_guard<std::mutex> diagLock(diagMutex);
        if (!diagFile.is_open()) {
            diagFile.open("ds_diag.txt", std::ios::out | std::ios::trunc);
        }
        if (diagFile.is_open()) {
            for (const auto& entry : bothSidedFacesMap) {
                const auto totalIt = totalFacesMap.find(entry.first);
                const size_t total = (totalIt != totalFacesMap.end()) ? totalIt->second : 0;
                diagFile << entry.second << "\t" << total << "\t" << entry.first << "\n";
            }
            diagFile.flush();
        }
    }
    // 合并掉的面从面表里移除（合并的两面共用同一组顶点，不会留下孤立顶点）。
    if (mergedFaces > 0) {
        std::vector<Face> kept;
        kept.reserve(model.faces.size() - mergedFaces);
        for (size_t i = 0; i < model.faces.size(); ++i) {
            if (!removeMask[i]) kept.push_back(std::move(model.faces[i]));
        }
        model.faces = std::move(kept);
        std::cout << "双面几何合并: " << mergedFaces << " 张 Same 反面已并掉" << std::endl;
    }

    return separated;
}

// 辅助函数:将字符串方向转换为FaceType枚举
FaceType StringToFaceType(const std::string& dirString) {
    if (dirString == "down") return FaceType::DOWN;
    if (dirString == "up") return FaceType::UP;
    if (dirString == "north") return FaceType::NORTH;
    if (dirString == "south") return FaceType::SOUTH;
    if (dirString == "west") return FaceType::WEST;
    if (dirString == "east") return FaceType::EAST;
    if (dirString == "DO_NOT_CULL") return FaceType::DO_NOT_CULL;
    return FaceType::UNKNOWN;
}


// 辅助函数:根据面索引获取面方向 (每4个顶点构成一个面)
FaceType GetFaceTypeByIndex(size_t faceIndex) {
    // 标准立方体模型的面顺序通常是:底面、顶面、北面、南面、西面、东面
    size_t normalizedIndex = faceIndex / 4; // 每个面由4个顶点索引组成
    switch (normalizedIndex % 6) {
        case 0: return FaceType::DOWN;  // 底面
        case 1: return FaceType::UP;    // 顶面
        case 2: return FaceType::NORTH; // 北面
        case 3: return FaceType::SOUTH; // 南面
        case 4: return FaceType::WEST;  // 西面
        case 5: return FaceType::EAST;  // 东面
        default: return FaceType::UNKNOWN;
    }
}
