#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <map>
#include <locale>

// 游戏整合包配置结构体
struct VersionConfig {
    std::string gameFolderPath;  // 整合包的路径
    std::string minecraftVersion;  // Minecraft版本
    std::string modLoaderType;  // 模组加载器类型
    std::vector<std::string> modList;  // 模组列表
    std::vector<std::string> resourcePackList;  // 资源包列表
    std::vector<std::string> saveGameList;  // 存档列表

    VersionConfig()
        : gameFolderPath(""), minecraftVersion("1.21"), modLoaderType("Forge") {
    }
};

// 全局配置结构体
struct Config {
    std::string worldPath;  // Minecraft 世界路径
    std::string packagePath;  // 游戏整合包路径
    std::string biomeMappingFile; // 生物群系映射文件路径
    std::string solidBlocksFile;  // 固体方块列表文件路径
    int minX, minY, minZ, maxX, maxY, maxZ; // 坐标范围
    int status; // 运行状态
    bool importByChunk;  // 是否按区块导入
    bool importByBlockType;  // 是否按方块种类导入
    int pointCloudType;  // 实心或空心，0为实心，1为空心
    int lodLevel;  // LOD等级: 0低，1中，2高
    std::string importFilePath; // 导入文件路径
    std::string selectedGameVersion; // 选择的游戏版本
    std::map<std::string, VersionConfig> versionConfigs;  // 按版本存储不同的配置

    Config()
        : worldPath(""), packagePath(""), biomeMappingFile("config\\jsons\\biomes.json"),solidBlocksFile("config\\jsons\\solids.json"),
        minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0), status(0), importByChunk(false),
        importByBlockType(false), pointCloudType(0), lodLevel(0), selectedGameVersion(""),
        versionConfigs() {
    }
};

// 声明写入配置函数
void WriteConfig(const Config& config, const std::string& configFile);
// 声明读取配置函数
Config LoadConfig(const std::string& configFile);

#endif // CONFIG_H
