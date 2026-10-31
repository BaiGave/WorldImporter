/**
 * @file GlobalCache.cpp
 * @brief 全局资源缓存系统实现
 * 
 * 本文件实现了游戏资源的全局缓存管理系统，主要功能包括:
 * 1. 加载和管理游戏资源(材质、模型、方块状态等)
 * 2. 支持多线程并行加载以提高性能
 * 3. 处理模组和资源包覆盖的优先级
 * 
 * 缓存的资源包括:
 * - 材质(textures): 方块和物品的图像文件
 * - 方块状态(blockstates): 方块的不同状态定义
 * - 模型(models): 3D模型定义
 * - 生物群系(biomes): 生物群系数据
 * - 颜色映射(colormaps): 用于植被和水的颜色映射
 * - mcmeta数据: 材质的动画和属性配置
 */
// GlobalCache.cpp
#include "GlobalCache.h"
#include "JarReader.h"
#include "fileutils.h"
#include "CTM.h"
#include "ModCT.h"
#include "CableMesher.h"
#include <iostream>
#include <filesystem> 
#include <chrono>
#include <vector>
#include <thread>
#include <future>
#include <queue>
#include <atomic>
#include "include/json.hpp"
#include <fstream>
#include <locale>
#include <algorithm>
#include <cctype>

#ifdef _WIN32
extern "C" {
    __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int wCodePageID);
}
#define CP_UTF8 65001
#endif

// 全局变量定义
std::unordered_set<std::string> fluidBlocks;  // 流体方块集合

// ========= 全局缓存命名空间 =========
namespace GlobalCache {
    // 缓存数据结构
    std::unordered_map<std::string, std::vector<unsigned char>> textures;    // 材质缓存
    std::unordered_map<std::string, nlohmann::json> mcmetaCache;             // 材质元数据缓存
    std::unordered_map<std::string, nlohmann::json> blockstates;             // 方块状态缓存
    std::unordered_map<std::string, nlohmann::json> models;                  // 模型缓存
    std::unordered_map<std::string, std::string> modelAssets;                // OBJ/MTL模型资源
    std::unordered_map<std::string, nlohmann::json> biomes;                  // 生物群系缓存
    std::unordered_map<std::string, nlohmann::json> paintingVariants;        // 画变体缓存
    std::unordered_map<std::string, std::vector<unsigned char>> colormaps;   // 颜色映射缓存

    // CTM 资源缓存
    std::unordered_map<std::string, std::string> ctmProperties;            // CTM properties 文本
    std::unordered_map<std::string, std::vector<unsigned char>> ctmTextures; // CTM tile 贴图

    // 图集定义缓存: "namespace:相对 atlases/ 的路径" -> JSON
    std::unordered_map<std::string, nlohmann::json> atlases;

    // 快速查找索引: "resourceType:namespace:resourcePath" -> 完整缓存键
    std::unordered_map<std::string, std::string> blockstateIndex;
    std::unordered_map<std::string, std::string> modelIndex;
    std::unordered_map<std::string, std::string> modelAssetIndex;
    std::unordered_map<std::string, std::string> textureIndex;
    std::unordered_map<std::string, std::string> mcmetaIndex;
    std::unordered_map<std::string, std::string> biomeIndex;
    std::unordered_map<std::string, std::string> paintingVariantIndex;
    std::unordered_map<std::string, std::string> colormapIndex;

    // CTM 快速查找索引
    std::unordered_map<std::string, std::string> ctmPropertiesIndex;
    std::unordered_map<std::string, std::string> ctmTexturesIndex;

    // 同步原语
    std::once_flag initFlag;       // 确保初始化只执行一次
    std::shared_mutex cacheMutex;  // 读写锁,支持并发读
    std::mutex queueMutex;         // 保护任务队列的互斥锁

    // 线程控制
    std::atomic<bool> stopFlag{ false };   // 控制工作线程停止标志
    std::queue<std::wstring> jarQueue;     // JAR文件路径队列
    std::vector<std::string> jarOrder;     // JAR文件加载顺序和对应的模组ID
}

/**
 * @brief 每个JAR文件资源处理任务的结果数据结构
 * 存储从单个JAR文件中提取的所有资源
 */
struct TaskResult {
    std::unordered_map<std::string, std::vector<unsigned char>> localTextures;    // 本地材质
    std::unordered_map<std::string, nlohmann::json> localBlockstates;             // 本地方块状态
    std::unordered_map<std::string, nlohmann::json> localModels;                  // 本地模型
    std::unordered_map<std::string, std::string> localModelAssets;                // OBJ/MTL模型资源
    std::unordered_map<std::string, nlohmann::json> localMcmetas;                 // 本地材质元数据
    std::unordered_map<std::string, nlohmann::json> localBiomes;                  // 本地生物群系
    std::unordered_map<std::string, nlohmann::json> localPaintingVariants;        // 本地画变体
    std::unordered_map<std::string, std::vector<unsigned char>> localColormaps;   // 本地颜色映射
    std::unordered_map<std::string, std::string> localCtmProperties;              // 本地 CTM properties
    std::unordered_map<std::string, std::vector<unsigned char>> localCtmTextures; // 本地 CTM 贴图
    std::unordered_map<std::string, nlohmann::json> localAtlases;                 // 本地图集定义(atlases/*.json)
};

//========== 辅助函数 ==========

// mods 目录里是否扫到 ConnectedTexturesMod。只在枚举 jar 时写入, 资源包不参与。
static bool g_connectedTexturesModLoaded = false;

/**
 * @brief 从JAR文件中提取模组ID
 * 
 * 根据不同模组加载器类型(Forge, Fabric, NeoForge)从JAR文件中
 * 提取模组ID，用于资源命名空间
 * 
 * @param jarPath JAR文件路径
 * @param modLoaderType 模组加载器类型
 * @return std::string 提取的模组ID，失败返回空字符串
 */
std::string GetModIdFromJar(std::wstring jarPath){
    std::string modId;
    try
    {
        // 使用 JarReader 处理 .jar 文件
        JarReader jarReader(jarPath);
        
        // 明确调用open()方法并检查是否成功
        if (!jarReader.open()) {
            std::cerr << "Failed to open jar for mod ID extraction: " << wstring_to_string(jarPath) << std::endl;
            return modId;
        }
        modId = jarReader.getID();
        // ConnectedTexturesMod: mod id 为 ctm, 或 jar 内有 team/chisel/ctm/ 类。
        // 本函数只给 mods 目录的 jar 用, 资源包不会因此打开 ctm mcmeta。
        if (modId == "ctm" || !jarReader.getFilesInSubDirectory("team/chisel/ctm/").empty())
            g_connectedTexturesModLoaded = true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error occurred during mod ID extraction: " << e.what() << std::endl;
    }
    return modId;
}

// 枚举完 mods 后只算一次: auto 看有没有 ctm 模组, on/off 忽略检测结果。
static void ResolveMcmetaCtm() {
    std::string mode = config.mcmetaCtm;
    for (char& ch : mode)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string detail;
    if (mode == "on") {
        config.mcmetaCtmEnabled = true;
        detail = "on";
    } else if (mode == "off") {
        config.mcmetaCtmEnabled = false;
        detail = "off";
    } else {
        config.mcmetaCtmEnabled = g_connectedTexturesModLoaded;
        detail = config.mcmetaCtmEnabled
            ? "on (auto, ctm mod found)"
            : "off (auto, ctm mod not found)";
    }
    std::cout << "[CTM] mcmeta CTM (ConnectedTexturesMod): " << detail << std::endl;
}

/**
 * @brief 初始化所有资源缓存
 * 
 * 该函数是全局缓存系统的主入口点，执行以下操作:
 * 1. 设置UTF-8控制台输出
 * 2. 准备要加载的JAR文件队列
 * 3. 创建多线程任务池并行处理JAR文件
 * 4. 按优先级顺序合并所有资源到全局缓存
 * 
 * 使用std::call_once确保只初始化一次
 */
void InitializeAllCaches() {
    // 设置控制台输出编码为UTF-8（跨平台方式）
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    // 在类Unix系统中，通常通过LANG环境变量设置UTF-8
    try {
        std::locale::global(std::locale("en_US.UTF-8"));
    } catch (const std::exception&) {
        // 如果失败，可能系统不支持此locale
        setlocale(LC_ALL, "en_US.UTF-8");
    }
#endif

    std::call_once(GlobalCache::initFlag, []() {
        auto start = std::chrono::high_resolution_clock::now();

        // 准备 jarQueue 与 jarOrder
        auto prepareQueue = []() {
            std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
            // 清空旧队列和 jarOrder
            while (!GlobalCache::jarQueue.empty()) {
                GlobalCache::jarQueue.pop();
            }
            GlobalCache::jarOrder.clear();
            
            
            // 步骤1: 加载资源包
            // 资源包从配置中读取并添加到队列，优先级低于模组
            for (const auto& resourcepack : config.resourcepacksPaths){
                std::string resourcepackname = std::filesystem::path(resourcepack).filename().string();
                std::string resourcepackid = resourcepackname.substr(0, resourcepackname.rfind("."));
                GlobalCache::jarQueue.push(string_to_wstring(resourcepack));
                GlobalCache::jarOrder.push_back(resourcepackid);
            }
            
            // 步骤2: 加载模组
            // 检查mods目录是否存在且路径有效
            if (config.modsPath != "None"){
                if (!config.modsPath.empty()) {
                    std::wstring modsPathW = string_to_wstring(config.modsPath);
                    namespace fs = std::filesystem;
                    
                    // 使用filesystem检查目录是否存在
                    if (fs::exists(modsPathW) && fs::is_directory(modsPathW)) {
                        // 目录存在，处理mod文件
                        for (const auto& entry : fs::directory_iterator(modsPathW)) {
                            if (entry.is_regular_file()) {
                                std::wstring modPath = entry.path().wstring();
                                std::string modStr = wstring_to_string(entry.path().filename().wstring());
                                //判断是否以.jar结尾
                                if (modStr.length() > 4 && modStr.substr(modStr.length() - 4) == ".jar") {
                                    // 获取模组ID或使用文件名作为备用ID
                                    std::string modid = GetModIdFromJar(modPath);
                                    if (modid.empty()) {
                                        modid = modStr.substr(0, modStr.length() - 4); // 移除.jar后缀
                                    }
                                    GlobalCache::jarQueue.push(modPath);
                                    GlobalCache::jarOrder.push_back(modid);
                                }
                            }
                        }
                    } else {
                        std::cerr << "Warning: Mods directory not found or not accessible: " << config.modsPath << std::endl;
                    }
                }
            }
            
            // 步骤3: 最后加载主JAR文件(优先级最高)
            GlobalCache::jarQueue.push(string_to_wstring(config.jarPath));
            GlobalCache::jarOrder.push_back("minecraft");
            };

        if (config.jarPath.empty()) {
            throw std::runtime_error("Config jarPath is empty: set the vanilla Minecraft jar path");
        }
        if (!std::filesystem::exists(config.jarPath)) {
            throw std::runtime_error(std::string("Config jarPath does not exist: ") + config.jarPath);
        }

        g_connectedTexturesModLoaded = false;
        prepareQueue();
        // jar 已枚举完(含 mods 的 mod id); 资源包不参与 ctm 检测
        ResolveMcmetaCtm();

        // 将 jarQueue 中的 jar 路径复制到 vector 中,保证顺序与 GlobalCache::jarOrder 保持一致
        std::vector<std::wstring> jarPaths;
        {
            std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
            while (!GlobalCache::jarQueue.empty()) {
                jarPaths.push_back(GlobalCache::jarQueue.front());
                GlobalCache::jarQueue.pop();
            }
        }

        const auto scanStart = std::chrono::high_resolution_clock::now();
        const unsigned hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
        struct ScanTask {
            size_t jarIndex;
            size_t rangeIndex;
            size_t rangeCount;
        };
        std::vector<ScanTask> scanTasks;
        // A few large archives otherwise leave the jar worker pool idle. Each
        // range gets its own reader/handle; no libzip handle crosses threads.
        const size_t rangesPerLargeJar = std::min<size_t>(4, hardwareThreads / jarPaths.size());
        for (size_t jarIndex = 0; jarIndex < jarPaths.size(); ++jarIndex) {
            std::error_code error;
            const auto bytes = std::filesystem::file_size(jarPaths[jarIndex], error);
            const size_t ranges = !error && bytes >= 8 * 1024 * 1024
                ? std::max<size_t>(1, rangesPerLargeJar) : 1;
            for (size_t range = 0; range < ranges; ++range) {
                scanTasks.push_back({jarIndex, range, ranges});
            }
        }
        const size_t taskCount = scanTasks.size();

        // Results stay in jar priority order, then ascending entry range order.
        std::vector<TaskResult> taskResults(taskCount);
        std::atomic<size_t> atomicIndex{ 0 };
        // 配置里的原版 jar（jarOrder 里 id 为 "minecraft"）打不开时必须失败退出，
        // 不能只警告后继续导出无材质/无模型的空壳网格。
        std::atomic<bool> vanillaJarFailed{ false };

        // 工作线程函数：处理JAR文件并提取资源
        auto worker = [&]() {
            while (true) {
                size_t idx = atomicIndex.fetch_add(1);
                if (idx >= taskCount)
                    break;  // 所有任务已分配完毕

                const auto& task = scanTasks[idx];
                const std::wstring& jarPath = jarPaths[task.jarIndex];
                const std::string& currentModId = GlobalCache::jarOrder[task.jarIndex];

                JarReader reader(jarPath);
                if (!reader.open()) {
                    if (currentModId == "minecraft") {
                        vanillaJarFailed.store(true, std::memory_order_release);
                        std::cerr << "Error: Failed to open vanilla jarPath, aborting: "
                                  << config.jarPath << std::endl;
                    } else {
                        std::cerr << "Warning: Failed to open jar, skipping resources for: "
                                  << currentModId << std::endl;
                    }
                    continue;  // 跳过此JAR文件
                }
                
                try {
                    // 读取所有资源类型并存入结果
                    reader.cacheAllResources(
                        taskResults[idx].localTextures,
                        taskResults[idx].localBlockstates,
                        taskResults[idx].localModels,
                        taskResults[idx].localModelAssets,
                        taskResults[idx].localMcmetas,
                        taskResults[idx].localBiomes,
                        taskResults[idx].localColormaps,
                        taskResults[idx].localCtmProperties,
                        taskResults[idx].localCtmTextures,
                        taskResults[idx].localAtlases,
                        taskResults[idx].localPaintingVariants,
                        task.rangeIndex, task.rangeCount
                    );
                } catch (const std::exception& e) {
                    std::cerr << "Error processing jar file for " << currentModId 
                              << ": " << e.what() << std::endl;
                }
            }
            };

        // Do not start idle workers when there are fewer scan tasks than CPUs.
        const unsigned numThreads = static_cast<unsigned>(std::min<size_t>(hardwareThreads, taskCount));
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        GlobalCache::stopFlag.store(false);
        
        // 启动工作线程
        for (unsigned i = 0; i < numThreads; ++i) {
            threads.emplace_back(worker);
        }
        
        // 等待所有线程完成
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        GlobalCache::stopFlag.store(true);

        if (vanillaJarFailed.load(std::memory_order_acquire)) {
            throw std::runtime_error(std::string("Failed to open vanilla jarPath: ") + config.jarPath);
        }

        const auto scanEnd = std::chrono::high_resolution_clock::now();

        // 按照加载顺序(优先级)合并资源到全局缓存
        // Preserve the existing first-insert-wins cache/index precedence.
        // Earlier entry ranges win collisions within the same jar.
        {
            std::lock_guard<std::shared_mutex> lock(GlobalCache::cacheMutex);
            for (size_t i = 0; i < taskCount; ++i) {
                const std::string& currentModId = GlobalCache::jarOrder[scanTasks[i].jarIndex];
                TaskResult& result = taskResults[i];

                // 合并材质
                for (auto& pair : result.localTextures) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::textures.try_emplace(cacheKey, std::move(pair.second));
                    // 构建快速查找索引: "textures:<namespace>:<path>" -> cacheKey
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("textures:") + pair.first;
                        GlobalCache::textureIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并方块状态
                for (auto& pair : result.localBlockstates) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::blockstates.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("blockstates:") + pair.first;
                        GlobalCache::blockstateIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并模型
                for (auto& pair : result.localModels) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::models.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("models:") + pair.first;
                        GlobalCache::modelIndex.emplace(indexKey, cacheKey);
                    }
                }
                for (auto& pair : result.localModelAssets) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::modelAssets.emplace(cacheKey, std::move(pair.second));
                    GlobalCache::modelAssetIndex.emplace("modelassets:" + pair.first, cacheKey);
                }
                // 合并生物群系
                for (auto& pair : result.localBiomes) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::biomes.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("biomes:") + pair.first;
                        GlobalCache::biomeIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并画变体 (data/<ns>/painting_variant)
                for (auto& pair : result.localPaintingVariants) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::paintingVariants.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("painting_variants:") + pair.first;
                        GlobalCache::paintingVariantIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并颜色映射
                for (auto& pair : result.localColormaps) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::colormaps.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("colormaps:") + pair.first;
                        GlobalCache::colormapIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并材质元数据
                for (auto& pair : result.localMcmetas) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::mcmetaCache.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("mcmetas:") + pair.first;
                        GlobalCache::mcmetaIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并 CTM properties
                for (auto& pair : result.localCtmProperties) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::ctmProperties.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("ctmproperties:") + pair.first;
                        GlobalCache::ctmPropertiesIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并 CTM 贴图
                for (auto& pair : result.localCtmTextures) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::ctmTextures.try_emplace(cacheKey, std::move(pair.second));
                    size_t firstColon = pair.first.find(':');
                    if (firstColon != std::string::npos) {
                        std::string indexKey = std::string("ctmtextures:") + pair.first;
                        GlobalCache::ctmTexturesIndex.emplace(indexKey, cacheKey);
                    }
                }
                // 合并图集定义
                for (auto& pair : result.localAtlases) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    GlobalCache::atlases.try_emplace(cacheKey, std::move(pair.second));
                }
            }
        }
        // 输出加载统计信息
        auto end = std::chrono::high_resolution_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::cout << "Parallel Cache Initialization Complete\n"
            << " - Used threads: " << numThreads << "\n"
            << " - Textures: " << GlobalCache::textures.size() << "\n"
            << " - Mcmetas: " << GlobalCache::mcmetaCache.size() << "\n"
            << " - Blockstates: " << GlobalCache::blockstates.size() << "\n"
            << " - Models: " << GlobalCache::models.size() << "\n"
            << " - Biomes: " << GlobalCache::biomes.size() << "\n"
            << " - Painting variants: " << GlobalCache::paintingVariants.size() << "\n"
            << " - Colormaps: " << GlobalCache::colormaps.size() << "\n"
            << " - CTM properties: " << GlobalCache::ctmProperties.size() << "\n"
            << " - CTM textures: " << GlobalCache::ctmTextures.size() << "\n"
            << " - Atlases: " << GlobalCache::atlases.size() << "\n"
            << " - Time: " << ms << "ms" << std::endl;

        const auto postStart = std::chrono::high_resolution_clock::now();

        // 展开各命名空间的图集目录映射(供 CTM matchTiles 解析虚拟贴图路径)
        LoadAtlasDirPrefixes(GlobalCache::atlases);
        // 建贴图名解析表(InitializeCtmRules 会查它来判定候选路径是否真实存在)
        BuildTileNameAliasTable();

        // 解析 CTM 规则并构建索引(在所有资源合并完成后)
        InitializeCtmRules();
        InitializeModCT();
        InitializeCableMeshers();
        const auto postEnd = std::chrono::high_resolution_clock::now();
        auto elapsedMs = [](auto from, auto to) {
            return std::chrono::duration<double, std::milli>(to - from).count();
        };
        std::cout << "[Cache timing] jar scan ms=" << elapsedMs(scanStart, scanEnd)
                  << " merge ms=" << elapsedMs(scanEnd, end)
                  << " CTM-post ms=" << elapsedMs(postStart, postEnd) << '\n';
        });
}
