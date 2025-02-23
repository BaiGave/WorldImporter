#pragma once
#include <string>
#include <unordered_map>
#include <mutex>

class Biome {
public:
    // 获取或注册群系ID（线程安全）
    static int GetId(const std::string& name);

    // 打印所有已注册群系
    static void PrintAllRegisteredBiomes();

    static std::vector<std::vector<int>> GenerateBiomeMap(int minX, int minZ, int maxX, int maxZ, int y = -1);

private:
    static std::unordered_map<std::string, int> biomeRegistry;
    static std::mutex registryMutex;

    // 禁止实例化
    Biome() = delete;
};

// 通过坐标获取生物群系ID
int GetBiomeId(int blockX, int blockY, int blockZ);