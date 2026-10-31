#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <map>
#include <locale>
#include <unordered_set>
#include "fileutils.h"

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

// 加权变体的挑选方式。
//   Game  : 与原版一致。DataVersion < 4325（1.21.5 之前）用
//           abs((int)nextLong()) % 总权重；否则或缺 DataVersion 时用 nextInt。
//           同一坐标永远同一变体。
//   First : 永远取第一个变体(关掉防重复贴图的观感)。
//   Random: 旧行为 —— std::random_device 播种的真随机，每次导出结果都不同。
enum class VariantSeedMode { Game, First, Random };

// 全局配置结构体
struct Config {
    std::string worldPath;  // Minecraft 世界路径
    std::string jarPath; //jar 路径
    std::string versionJsonPath;
    std::string modsPath; // mods 路径
    std::vector<std::string> resourcepacksPaths; // 资源包路径
    std::unordered_set<std::string> lod1Blocks; // LOD1级别使用原始模型的方块列表
    std::string selectedDimension; // 当前选择的维度ID
    std::string fluidsFile; //流体列表文件路径
    int minX, minY, minZ, maxX, maxY, maxZ; // 坐标范围
    int chunkXStart, chunkXEnd, chunkZStart, chunkZEnd; // 区块坐标范围
    int sectionYStart, sectionYEnd; // Section 坐标范围
    int status; // 运行状态
    
    bool useChunkPrecision; //使用区块精度导出
    bool keepBoundary; // 保留边界面
    bool strictDeduplication;//严格剔除面
    bool cullCave;//剔除洞穴
    bool exportLightBlock;//导出光源方块
    bool exportLightBlockOnly;//仅导出光源方块
    float lightBlockSize; //光源方块半径大小
    bool allowDoubleFace;//允许重叠面
    bool doubleSidedGeometry;//是否生成几何双面: 零厚度贴片保留正反两面(关掉则朝向相反的重合面只留一张)
    // 只合并"同材质且 UV 一一对应"的正反两面（世界 UV 同向，从背面看与保留两张面等价）;
    // 镜像 UV 的对（两面从左到右都正常）不能合并，否则背面会变成镜像文字。
    bool mergeSameDoubleSided;
    // 只合并"同贴图但 UV 方向不同"的对（镜像）：同向的对由 mergeSameDoubleSided 控制。
    // 合并后一面会变成镜像，接受"看起来的效果"损失时使用。
    bool mergeMirrorDoubleSided;
    float doubleSidedFaceOffset; //几何双面里反面沿法线的外移量(米): 0=严格共面只换索引, 需大于该坐标处 float 精度
    float overlayLayerStep; //共面外移步长(米): 叠加层逐层外移 + 薄片贴边沿法线外推, 避免 Blender 下 z-fighting
    bool ctmPrefixMatchTiles; //matchTiles 以 '_' 结尾时是否再做前缀族匹配(默认开=保持既有行为; 关掉只保留剥掉下划线后的精确匹配)
    std::string mcmetaCtm; //ConnectedTexturesMod 的 .png.mcmeta: "auto"|"on"|"off"(默认 auto=仅 mods 里有 ctm 模组时启用; 资源包不算)
    bool mcmetaCtmEnabled; //mcmetaCtm 解析后的实际开关(枚举 jar 后写入; 关掉则跳过 ctm mcmeta)
    bool mergeModTextures; //模组方块是否复用同一张贴图已有的材质(原版方块之间始终不合并; 默认开)
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
    bool activeLOD2; // 是否启用LOD2
    bool activeLOD3; // 是否启用LOD3
    bool activeLOD4; // 是否启用LOD4
    bool useBiomeColors; // 是否启用群系颜色叠加
    // Vanilla client option biomeBlendRadius. 0 keeps one sample per column
    // (seed fuzz still applies). Clamped to [0, 32] so the channel sum fits in int32.
    // Samples past the preloaded chunk ring are clamped back into the export rect.
    int biomeBlendRadius;
    VariantSeedMode variantSeedMode; // 加权变体的挑选方式(默认 Game=与游戏一致且可复现)
    bool plantOffset; // 原版 OffsetType 植物随机位移(草/花/蕨等); 关掉后植物都待在格子中心
    bool importEntities; // 导出区域内生物/实体元数据
    bool exportEntityModels; // 导出区域内生物的几何模型(entities.obj/.mtl, 村民层走 overlay.json)

    bool exportFullModel;  // 是否完整导入
    bool downloadPlayerSkins; // 下载玩家头颅皮肤(默认开; 关掉且无本地缓存时保持 Steve)
    int partitionSize; //分割大小
    size_t maxTasksPerBatch; //每批次区块任务数量上限
    int modelThreads; //模型生成线程数(默认1，自动模式可提高)

    int decimalPlaces; //lod群系颜色值小数精度 #待做
    bool importByBlockType;  // 是否按方块种类导入 #待做
    int pointCloudType;  // 实心或空心,0为实心,1为空心 #待做
    std::string optifineCtm; // OptiFine .properties CTM: auto/on/off(默认 auto)
    Config()
        :
        worldPath(""),
        jarPath(""),
        versionJsonPath(""),
        modsPath(""),
        resourcepacksPaths({}),
        selectedDimension("minecraft:overworld"),
        fluidsFile(JoinPath("config", "jsons", "fluids.json")),
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
        overlayLayerStep(0.0005f),
        ctmPrefixMatchTiles(true),
        mcmetaCtm("auto"),
        mcmetaCtmEnabled(false),
        mergeModTextures(true),
        doubleSidedGeometry(true),
        mergeSameDoubleSided(false),
        mergeMirrorDoubleSided(false),
        doubleSidedFaceOffset(0.0005f),
        isLODAutoCenter(true),
        LODCenterX(0),
        LODCenterZ(0),
        LOD0renderDistance(6),
        LOD1renderDistance(6),
        LOD2renderDistance(6),
        LOD3renderDistance(6),
        useUnderwaterLOD(true),
        useGreedyMesh(false),
        activeLOD(true),
        activeLOD2(true),
        activeLOD3(true),
        activeLOD4(true),
        lod1Blocks({}),
        useBiomeColors(true),
        biomeBlendRadius(2),
        variantSeedMode(VariantSeedMode::Game),
        plantOffset(true),
        importEntities(false),
        exportEntityModels(true),

        exportFullModel(false),
        downloadPlayerSkins(true),
        partitionSize(4),
        maxTasksPerBatch(32768),
        modelThreads(1),

        decimalPlaces(2),
        importByBlockType(false),
        pointCloudType(0),
        optifineCtm("auto")
        {}
};

extern Config config;

// 声明读取配置函数
Config LoadConfig(const std::string& configFile);

#endif // CONFIG_H
