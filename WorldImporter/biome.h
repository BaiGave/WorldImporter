#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include "include/json.hpp"

enum class BiomeColorType {
    Foliage,
    DryFoliage,
    Grass,
    Fog,
    Sky,
    Water,
    WaterFog
};

struct BiomeColors {
    // 直接颜色值
    int foliage = -1;
    int dryFoliage = -1;
    int grass = -1;
    int fog = 0;
    int sky = 0;
    int water = 0;
    int waterFog = 0;
    // grass_color_modifier == swamp. Export uses BiomeSwampGrassColor per column.
    // grass stays the baked 0x6A7039 so LOD / GetColor keep the previous colour.
    bool swampGrass = false;

    // 色图计算参数
    float adjTemperature = 0.0f;
    float adjDownfall = 0.0f;
};

namespace BiomeUtils {
    template<typename T>
    constexpr const T& clamp(const T& v, const T& lo, const T& hi) {
        return (v < lo) ? lo : (hi < v) ? hi : v;
    }
}

struct BiomeInfo {
    explicit BiomeInfo(int id, BiomeColors&& initialColors)
        : id(id), colors(std::move(initialColors)) {
    }

    int id;
    std::string namespaceName;
    std::string biomeName;
    BiomeColors colors;
    mutable std::mutex colorMutex;
};

    // 全局生物群系地图数据
    static std::vector<std::vector<int>> g_biomeMap;
    extern int g_biomeMapMinX;
    extern int g_biomeMapMinZ;

class Biome {
public:
    // 初始化生物群系地图尺寸
    static void InitializeBiomeMap(int minX, int minZ, int maxX, int maxZ);
public:
    // 获取或注册群系ID(线程安全)
    static int GetId(const std::string& fullName);

    static int GetColor(int biomeId, BiomeColorType colorType);

    static int GetBiomeColor(int blockX, int blockY, int blockZ, BiomeColorType colorType);

    static void GenerateBiomeMap(int minX, int minZ, int maxX, int maxZ);

    static bool ExportToPNG(const std::string& filename, BiomeColorType colorType);

    static nlohmann::json GetBiomeJson(const std::string& namespaceName, const std::string& biomeId);

    static std::string GetColormapData(const std::string& namespaceName, const std::string& colormapName);



private:
    static std::unordered_map<std::string, BiomeInfo> biomeRegistry;
    static std::shared_mutex registryMutex; // 改用读写锁
    static int CalculateColorFromColormap(const std::string& filePath,float temperature,float downfall);
    static BiomeColors ParseBiomeColors(const nlohmann::json& biomeJson);
    static void FillBiomeExportLut(std::vector<int> (&colors)[7], std::vector<uint8_t>& swamp);


    // 禁止实例化
    Biome() = delete;
};

// 通过坐标获取生物群系ID
int GetBiomeId(int blockX, int blockY, int blockZ);

// Vanilla BiomeManager / LinearCongruentialGenerator / swamp noise / tint average.
// Pure functions: no world access. Vectors live in tests/biome_blend_tests.cpp.
int64_t BiomeObfuscateSeed(int64_t worldSeed);
int64_t BiomeLcgNext(int64_t seed, int64_t salt);
double BiomeFiddledDistance(int64_t zoomSeed, int qx, int qy, int qz, double dx, double dy, double dz);
void BiomeChosenQuart(int64_t zoomSeed, int x, int y, int z, int& qx, int& qy, int& qz);
double BiomeSwampNoise(int blockX, int blockZ);
int BiomeSwampGrassColor(int blockX, int blockZ);
int BiomeBlendTint(const int* colors, int count);