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

            // 添加模组（根据 currentSelectedGameVersion 填充队列和 jarOrder）
            if (modListCache.count(currentSelectedGameVersion)) {
                for (const auto& fd : modListCache[currentSelectedGameVersion]) {
                    if (fd.namespaceName == "resourcePack") {
                        if (resourcePacksCache.count(currentSelectedGameVersion)) {
                            for (const auto& rfd : resourcePacksCache[currentSelectedGameVersion]) {
                                GlobalCache::jarQueue.push(string_to_wstring(rfd.path));
                                GlobalCache::jarOrder.push_back(rfd.namespaceName);
                            }
                        }
                    }
                    else if (fd.namespaceName == "vanilla") {
                        if (VersionCache.count(currentSelectedGameVersion)) {
                            for (const auto& vfd : VersionCache[currentSelectedGameVersion]) {
                                GlobalCache::jarQueue.push(string_to_wstring(vfd.path));
                                GlobalCache::jarOrder.push_back(vfd.namespaceName);
                            }
                        }
                    }
                    else {
                        GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                        GlobalCache::jarOrder.push_back(fd.namespaceName);
                    }
                }
            }
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
        std::vector<std::future<void>> futures;
        GlobalCache::stopFlag.store(false);

        for (unsigned i = 0; i < numThreads; ++i) {
            futures.emplace_back(std::async(std::launch::async, worker));
        }
        for (auto& f : futures) {
            try {
                f.get();
            }
            catch (const std::exception& e) {
                std::cerr << "Thread error: " << e.what() << std::endl;
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
