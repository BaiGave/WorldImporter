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
#ifdef _WIN32
#include <windows.h>
#endif

// 全局变量定义
std::unordered_set<std::string> solidBlocks;  // 固体方块集合
std::unordered_set<std::string> fluidBlocks;  // 流体方块集合

// ========= 全局缓存命名空间 =========
namespace GlobalCache {
    // 缓存数据结构
    std::unordered_map<std::string, std::vector<unsigned char>> textures;    // 材质缓存
    std::unordered_map<std::string, nlohmann::json> mcmetaCache;             // 材质元数据缓存
    std::unordered_map<std::string, nlohmann::json> blockstates;             // 方块状态缓存
    std::unordered_map<std::string, nlohmann::json> models;                  // 模型缓存
    std::unordered_map<std::string, nlohmann::json> biomes;                  // 生物群系缓存
    std::unordered_map<std::string, std::vector<unsigned char>> colormaps;   // 颜色映射缓存

    // 同步原语
    std::once_flag initFlag;       // 确保初始化只执行一次
    std::mutex cacheMutex;         // 保护缓存数据的互斥锁
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
    std::unordered_map<std::string, nlohmann::json> localMcmetas;                 // 本地材质元数据
    std::unordered_map<std::string, nlohmann::json> localBiomes;                  // 本地生物群系
    std::unordered_map<std::string, std::vector<unsigned char>> localColormaps;   // 本地颜色映射
};

//========== 辅助函数 ==========

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
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error occurred during mod ID extraction: " << e.what() << std::endl;
    }
    return modId;
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
    // 设置控制台输出编码为UTF-8
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
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
                    WIN32_FIND_DATAW fdFile;
                    HANDLE hFind = FindFirstFileW(modsPathW.c_str(), &fdFile);
                    if (hFind != INVALID_HANDLE_VALUE && (fdFile.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        // 目录存在,处理mod文件
                        FindClose(hFind);
                        
                        for (const auto& entry : std::filesystem::directory_iterator(modsPathW)) {
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
                        if (hFind != INVALID_HANDLE_VALUE) {
                            FindClose(hFind);
                        }
                        std::cerr << "Warning: Mods directory not found or not accessible: " << config.modsPath << std::endl;
                    }
                }
            }
            
            // 步骤3: 最后加载主JAR文件(优先级最高)
            GlobalCache::jarQueue.push(string_to_wstring(config.jarPath));
            GlobalCache::jarOrder.push_back("minecraft");
            };

        prepareQueue();

        // 将 jarQueue 中的 jar 路径复制到 vector 中,保证顺序与 GlobalCache::jarOrder 保持一致
        std::vector<std::wstring> jarPaths;
        {
            std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
            while (!GlobalCache::jarQueue.empty()) {
                jarPaths.push_back(GlobalCache::jarQueue.front());
                GlobalCache::jarQueue.pop();
            }
        }

        size_t taskCount = jarPaths.size();

        // 用 vector 保存所有任务的结果,顺序与 jarPaths 和 jarOrder 对应
        std::vector<TaskResult> taskResults(taskCount);
        std::atomic<size_t> atomicIndex{ 0 };

        // 工作线程函数：处理JAR文件并提取资源
        auto worker = [&]() {
            while (true) {
                size_t idx = atomicIndex.fetch_add(1);
                if (idx >= taskCount)
                    break;  // 所有任务已分配完毕

                std::wstring jarPath = jarPaths[idx];
                std::string currentModId = GlobalCache::jarOrder[idx];

                JarReader reader(jarPath);
                if (!reader.open()) {
                    std::cerr << "Warning: Failed to open jar, skipping resources for: " << currentModId << std::endl;
                    continue;  // 跳过此JAR文件
                }
                
                try {
                    // 读取所有资源类型并存入结果
                    reader.cacheAllResources(taskResults[idx].localTextures,
                        taskResults[idx].localBlockstates,
                        taskResults[idx].localModels,
                        taskResults[idx].localMcmetas);
                    reader.cacheAllBiomes(taskResults[idx].localBiomes);
                    reader.cacheAllColormaps(taskResults[idx].localColormaps);
                } catch (const std::exception& e) {
                    std::cerr << "Error processing jar file for " << currentModId 
                              << ": " << e.what() << std::endl;
                }
            }
            };

        // 创建工作线程池
        const unsigned numThreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
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

        // 按照加载顺序(优先级)合并资源到全局缓存
        // 后加载的资源优先级更高(minecraft原版资源优先级最高)
        {
            std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
            for (size_t i = 0; i < taskCount; ++i) {
                std::string currentModId = GlobalCache::jarOrder[i];
                TaskResult& result = taskResults[i];

                // 合并各类资源，为每个资源添加命名空间前缀
                // 例如: "block/stone" -> "minecraft:block/stone"
                
                // 合并材质
                for (auto& pair : result.localTextures) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::textures.find(cacheKey) == GlobalCache::textures.end()) {
                        GlobalCache::textures.insert({ cacheKey, pair.second });
                    }
                }
                // 合并方块状态
                for (auto& pair : result.localBlockstates) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::blockstates.find(cacheKey) == GlobalCache::blockstates.end()) {
                        GlobalCache::blockstates.insert({ cacheKey, pair.second });
                    }
                }
                // 合并模型
                for (auto& pair : result.localModels) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::models.find(cacheKey) == GlobalCache::models.end()) {
                        GlobalCache::models.insert({ cacheKey, pair.second });
                    }
                }
                // 合并生物群系
                for (auto& pair : result.localBiomes) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::biomes.find(cacheKey) == GlobalCache::biomes.end()) {
                        GlobalCache::biomes.insert({ cacheKey, pair.second });
                    }
                }
                // 合并颜色映射
                for (auto& pair : result.localColormaps) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::colormaps.find(cacheKey) == GlobalCache::colormaps.end()) {
                        GlobalCache::colormaps.insert({ cacheKey, pair.second });
                    }
                }
                // 合并材质元数据
                for (auto& pair : result.localMcmetas) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::mcmetaCache.find(cacheKey) == GlobalCache::mcmetaCache.end()) {
                        GlobalCache::mcmetaCache.insert({ cacheKey, pair.second });
                    }
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
            << " - Colormaps: " << GlobalCache::colormaps.size() << "\n"
            << " - Time: " << ms << "ms" << std::endl;
        });
}
