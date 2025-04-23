// GlobalCache.cpp
#include "GlobalCache.h"
#include "JarReader.h"
#include "fileutils.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <thread>
#include <future>
#include <queue>
#include <atomic>
#include "include/json.hpp"
#include <fstream>
// ========= 全局变量定义 =========
namespace GlobalCache {
    // 缓存数据
    std::unordered_map<std::string, std::vector<unsigned char>> textures;
    std::unordered_map<std::string, nlohmann::json> mcmetaCache;
    std::unordered_map<std::string, nlohmann::json> blockstates;
    std::unordered_map<std::string, nlohmann::json> models;
    std::unordered_map<std::string, nlohmann::json> biomes;
    std::unordered_map<std::string, std::vector<unsigned char>> colormaps;

    // 同步原语
    std::once_flag initFlag;
    std::mutex cacheMutex;
    std::mutex queueMutex;

    // 线程控制
    std::atomic<bool> stopFlag{ false };
    std::queue<std::wstring> jarQueue;
    std::vector<std::string> jarOrder; // 记录 JAR 文件的加载顺序和对应的模组 ID
}

// 定义用于保存每个 jar 文件读取结果的结构体
struct TaskResult {
    std::unordered_map<std::string, std::vector<unsigned char>> localTextures;
    std::unordered_map<std::string, nlohmann::json> localBlockstates;
    std::unordered_map<std::string, nlohmann::json> localModels;
    std::unordered_map<std::string, nlohmann::json> localMcmetas;
    std::unordered_map<std::string, nlohmann::json> localBiomes;
    std::unordered_map<std::string, std::vector<unsigned char>> localColormaps;
};
//========== 补充部分函数 ==========
std::vector<std::wstring> listdir(const std::wstring& path) {
    std::vector<std::wstring> files;

    std::wstring wpath = path + L"\\*";

    WIN32_FIND_DATAW findFileData;
    HANDLE hFind = FindFirstFileW(wpath.c_str(), &findFileData);

    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (std::wstring(findFileData.cFileName) != L"." && std::wstring(findFileData.cFileName) != L"..") {
                files.push_back(findFileData.cFileName);
            }
        } while (FindNextFileW(hFind, &findFileData));
        FindClose(hFind);
    }

    return files;
}

std::string GetLoadType(const std::wstring versionJsonPath) {
    std::string modLoaderType;
    // 以二进制模式打开文件
    std::ifstream versionFile(versionJsonPath, std::ios::binary);
    // 设置文件流的区域设置为 UTF-8
    versionFile.imbue(std::locale("en_US.UTF-8"));

    // 检查文件是否成功打开
    bool hasVersionJson = versionFile.is_open();
    // 用于存储解析后的 JSON 数据
    nlohmann::json versionData;
    // 标识是否为 Forge、Fabric 或 NeoForge 整合包
    bool isForgePack = false, isFabricPack = false, isNeoForgePack = false;

    if (hasVersionJson) {
        try {
            // 从文件中读取 JSON 数据
            versionFile >> versionData;
            // 关闭文件
            versionFile.close();

            // 解析 mod 加载器类型
            if (versionData.contains("arguments") && versionData["arguments"].contains("game")) {
                // 遍历 "game" 参数列表
                for (const auto& arg : versionData["arguments"]["game"]) {
                    if (arg.is_string()) {
                        // 获取参数值
                        std::string argStr = arg.get<std::string>();
                        // 判断是否为 Forge 或 NeoForge 整合包
                        if (argStr == "forgeclient") isForgePack = true;
                        else if (argStr == "neoforgeclient") isNeoForgePack = true;
                    }
                }
            }

            // 检查 "mainClass" 字段以确定 mod 加载器类型
            if (versionData.contains("mainClass")) {
                std::string mainClass = versionData["mainClass"];
                // 判断是否为 Fabric 或 Quilt 整合包
                if (mainClass == "net.fabricmc.loader.impl.launch.knot.KnotClient" ||
                    mainClass == "org.quiltmc.loader.impl.launch.knot.KnotClient") {
                    isFabricPack = true;
                }
                // 判断是否为 Forge 整合包
                else if (mainClass == "net.minecraftforge.bootstrap.ForgeBootstrap") {
                    isForgePack = true;
                }
            }

            // 根据解析结果设置 modLoaderType
            if (isForgePack)       modLoaderType = "Forge";
            else if (isNeoForgePack) modLoaderType = "NeoForge";
            else if (isFabricPack)  modLoaderType = "Fabric";
            else                   modLoaderType = "Vanilla";
        }
        catch (...) {
            // 捕获解析异常并输出错误信息
            std::cerr << "Failed to parse version.json, using fallback" << std::endl;
            hasVersionJson = false;
        }
    }
    return modLoaderType;
}
std::string GetModIdFromJar(std::wstring jarPath , std::string modLoaderType){
    std::string modId;
    try
    {
        // 使用 JarReader 处理 .jar 文件
        JarReader jarReader(jarPath);

        if (modLoaderType == "Fabric") {
            modId = jarReader.getFabricModId();
        }
        else if (modLoaderType == "Forge") {
            modId = jarReader.getForgeModId();
        }
        else if (modLoaderType == "NeoForge") {
            modId = jarReader.getNeoForgeModId();
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error occurred: " << e.what() << std::endl;
    }
    return modId;
}
// ========= 初始化实现 =========
void InitializeAllCaches() {


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
            //获取modLoaderType
            std::string modLoaderType = GetLoadType(string_to_wstring(config.versionJsonPath));
            //从config中读取并添加到GlobalCache::jarQueue和GlobalCache::jarOrder
            std::cout << "正在初始化缓存..." << std::endl;
            for (const auto& resourcepack : config.resourcepacksPaths){
                std::string resourcepackname = wstring_to_string(GetFolderNameFromPath(string_to_wstring(resourcepack)));
                std::string resourcepackid = resourcepackname.substr(0, resourcepackname.rfind("."));
                GlobalCache::jarQueue.push(string_to_wstring(resourcepack));
                GlobalCache::jarOrder.push_back(resourcepackid);
            }
            for (const auto& mod : listdir(string_to_wstring(config.modsPath))) {
                //判断是否以.jar结尾
                if (wstring_to_string(mod).substr(wstring_to_string(mod).length() - 4) == ".jar") {
                    std::wstring modPath = string_to_wstring(config.modsPath + "\\") + mod;
                    std::string modid = GetModIdFromJar(modPath , modLoaderType);
                    GlobalCache::jarQueue.push(modPath);
                    GlobalCache::jarOrder.push_back(modid);
                }
            }
            GlobalCache::jarQueue.push(string_to_wstring(config.jarPath));
            GlobalCache::jarOrder.push_back("minecraft");
            };

        prepareQueue();

        // 将 jarQueue 中的 jar 路径复制到 vector 中，保证顺序与 GlobalCache::jarOrder 保持一致
        std::vector<std::wstring> jarPaths;
        {
            std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
            while (!GlobalCache::jarQueue.empty()) {
                jarPaths.push_back(GlobalCache::jarQueue.front());
                GlobalCache::jarQueue.pop();
            }
        }

        size_t taskCount = jarPaths.size();

        

        // 用 vector 保存所有任务的结果，顺序与 jarPaths 和 jarOrder 对应
        std::vector<TaskResult> taskResults(taskCount);
        std::atomic<size_t> atomicIndex{ 0 };

        // 工作线程：按索引读取 jar 文件，并将结果存入 taskResults 对应位置
        auto worker = [&]() {
            while (true) {
                size_t idx = atomicIndex.fetch_add(1);
                if (idx >= taskCount)
                    break;

                std::wstring jarPath = jarPaths[idx];
                // 根据索引，从 GlobalCache::jarOrder 中获取对应的模组 ID
                std::string currentModId = GlobalCache::jarOrder[idx];

                JarReader reader(jarPath);
                if (reader.open()) {
                    // 读取 jar 文件的各类资源，存入对应的任务结果中
                    reader.cacheAllResources(taskResults[idx].localTextures,
                        taskResults[idx].localBlockstates,
                        taskResults[idx].localModels,
                        taskResults[idx].localMcmetas);
                    reader.cacheAllBiomes(taskResults[idx].localBiomes);
                    reader.cacheAllColormaps(taskResults[idx].localColormaps);

                }
            }
            };

        // 根据硬件并发数创建线程池
        const unsigned numThreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        GlobalCache::stopFlag.store(false);
        for (unsigned i = 0; i < numThreads; ++i) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        GlobalCache::stopFlag.store(true);

        // 按照 GlobalCache::jarOrder 顺序合并各个 jar 的数据到全局缓存中
        {
            std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
            for (size_t i = 0; i < taskCount; ++i) {
                std::string currentModId = GlobalCache::jarOrder[i];
                TaskResult& result = taskResults[i];

                // 合并 textures
                for (auto& pair : result.localTextures) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::textures.find(cacheKey) == GlobalCache::textures.end()) {
                        GlobalCache::textures.insert({ cacheKey, pair.second });
                    }
                }
                // 合并 blockstates
                for (auto& pair : result.localBlockstates) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::blockstates.find(cacheKey) == GlobalCache::blockstates.end()) {
                        GlobalCache::blockstates.insert({ cacheKey, pair.second });
                    }
                }
                // 合并 models
                for (auto& pair : result.localModels) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::models.find(cacheKey) == GlobalCache::models.end()) {
                        GlobalCache::models.insert({ cacheKey, pair.second });
                    }
                }
                // 合并 biomes
                for (auto& pair : result.localBiomes) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::biomes.find(cacheKey) == GlobalCache::biomes.end()) {
                        GlobalCache::biomes.insert({ cacheKey, pair.second });
                    }
                }
                // 合并 colormaps
                for (auto& pair : result.localColormaps) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::colormaps.find(cacheKey) == GlobalCache::colormaps.end()) {
                        GlobalCache::colormaps.insert({ cacheKey, pair.second });
                    }
                }
                // 合并 mcmeta
                for (auto& pair : result.localMcmetas) {
                    std::string cacheKey = currentModId + ":" + pair.first;
                    if (GlobalCache::mcmetaCache.find(cacheKey) == GlobalCache::mcmetaCache.end()) {
                        GlobalCache::mcmetaCache.insert({ cacheKey, pair.second });
                    }
                }
            }
        }

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
