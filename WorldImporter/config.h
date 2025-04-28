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
    std::string jarPath; //jar 路径
    std::string versionJsonPath;
    std::string modsPath; // mods 路径
    std::vector<std::string> resourcepacksPaths; // 资源包路径

    std::string solidBlocksFile;  // 固体方块列表文件路径
    std::string fluidsFile; //流体列表文件路径
    int minX, minY, minZ, maxX, maxY, maxZ; // 坐标范围
    int status; // 运行状态
    
    bool useChunkPrecision; //使用区块精度导出
    bool keepBoundary; // 保留边界面
    bool strictDeduplication;//严格剔除面
    bool cullCave;//剔除洞穴
    bool exportLightBlock;//导出光源方块
    bool exportLightBlockOnly;//仅导出光源方块
    float lightBlockSize; //光源方块半径大小
    bool allowDoubleFace;//允许重叠面
    bool activeLOD; //使用LOD
    bool isLODAutoCenter; //是否自动计算LOD中心坐标
    int LODCenterX; //LOD中心坐标X
    int LODCenterZ; //LOD中心坐标Z
    int LOD0renderDistance;//渲染距离
    int LOD1renderDistance;//LOD1 x1渲染距离
    int LOD2renderDistance;//LOD1 x2渲染距离
    int LOD3renderDistance;//LOD1 x4渲染距离
    bool useUnderwaterLOD; //水下LOD模型生成
    bool useGreedyMesh; //是否使用GreedyMesh算法合并面

    bool exportFullModel;  // 是否完整导入
    int partitionSize; //分割大小

    int decimalPlaces; //lod群系颜色值小数精度 #待做
    bool importByBlockType;  // 是否按方块种类导入 #待做
    int pointCloudType;  // 实心或空心，0为实心，1为空心 #待做

    Config()
        :
        worldPath(""),
        jarPath(""),
        versionJsonPath(""),
        modsPath(""),
        resourcepacksPaths({}),

        solidBlocksFile("config\\jsons\\solids.json"),
        fluidsFile("config\\jsons\\fluids.json"),
        minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0),
        status(0),

        useChunkPrecision(false),
        keepBoundary(false),
        strictDeduplication(true),
        cullCave(true),
        exportLightBlock(true),
        exportLightBlockOnly(false),
        lightBlockSize(0.05f),
        allowDoubleFace(false),
        activeLOD(true),
        isLODAutoCenter(true),
        LODCenterX(0),
        LODCenterZ(0),
        LOD0renderDistance(6),
        LOD1renderDistance(6),
        LOD2renderDistance(6),
        LOD3renderDistance(6),
        useUnderwaterLOD(true),
        useGreedyMesh(false),

        exportFullModel(false),
        partitionSize(4),

        decimalPlaces(2),
        importByBlockType(false),
        pointCloudType(0)
        {}
};

// 声明全局 config 变量
extern Config config;

// 声明读取配置函数
Config LoadConfig(const std::string& configFile);

#endif // CONFIG_H
