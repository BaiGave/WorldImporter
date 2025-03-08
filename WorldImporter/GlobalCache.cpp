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


// ========= 初始化实现 =========
void InitializeAllCaches() {
    std::call_once(GlobalCache::initFlag, []() {
        auto start = std::chrono::high_resolution_clock::now();

        // 准备JAR文件队列
        auto prepareQueue = []() {
            std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);

            // 清空旧队列
            while (!GlobalCache::jarQueue.empty()) {
                GlobalCache::jarQueue.pop();
            }

            // 添加模组
            if (modListCache.count(currentSelectedGameVersion)) {
                for (const auto& fd : modListCache[currentSelectedGameVersion]) {
                    if (fd.namespaceName == "resourcePack") {
                        // 添加资源包
                        if (resourcePacksCache.count(currentSelectedGameVersion)) {
                            for (const auto& fd : resourcePacksCache[currentSelectedGameVersion]) {
                                GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                                // 使用命名空间作为模组 ID
                                GlobalCache::jarOrder.push_back(fd.namespaceName);
                            }
                        }
                    }
                    else if (fd.namespaceName == "vanilla") {
                        // 添加原版JAR
                        if (VersionCache.count(currentSelectedGameVersion)) {
                            for (const auto& fd : VersionCache[currentSelectedGameVersion]) {
                                GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                                // 使用命名空间作为模组 ID
                                GlobalCache::jarOrder.push_back(fd.namespaceName);
                            }
                        }
                    }
                    else
                    {
                        GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                        // 使用命名空间作为模组 ID
                        GlobalCache::jarOrder.push_back(fd.namespaceName);
                    }

                }
            }
            };

        prepareQueue();
        // 工作线程函数
        auto worker = []() {
            while (!GlobalCache::stopFlag.load()) {
                std::wstring jarPath;

                // 获取任务
                {
                    std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
                    if (GlobalCache::jarQueue.empty()) return;
                    jarPath = GlobalCache::jarQueue.front();
                    GlobalCache::jarQueue.pop();
                }

                // 处理JAR
                JarReader reader(jarPath);
                if (reader.open()) {

                    // 获取当前 JAR 文件的模组 ID（从 jarOrder 中获取）
                    static std::atomic<size_t> jarOrderIndex{ 0 }; // 用于记录当前处理到的 jarOrder 索引
                    std::string currentModId;
                    {
                        std::lock_guard<std::mutex> lock(GlobalCache::queueMutex);
                        // 根据 jarOrderIndex 获取对应的模组 ID
                        if (jarOrderIndex < GlobalCache::jarOrder.size()) {
                            currentModId = GlobalCache::jarOrder[jarOrderIndex];
                            jarOrderIndex++;
                        }
                    }

                    // 局部缓存临时存储
                    std::unordered_map<std::string, std::vector<unsigned char>> localTextures;
                    std::unordered_map<std::string, nlohmann::json> localBlockstates;
                    std::unordered_map<std::string, nlohmann::json> localModels;
                    std::unordered_map<std::string, nlohmann::json> localBiomes;
                    std::unordered_map<std::string, nlohmann::json> localMcmetas;
                    std::unordered_map<std::string, std::vector<unsigned char>> localColormaps;

                    reader.cacheAllResources(localTextures, localBlockstates, localModels, localMcmetas);
                    reader.cacheAllBiomes(localBiomes);
                    reader.cacheAllColormaps(localColormaps);
                    // 合并到全局缓存
                    {
                        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

                        // 手动合并 textures
                        for (auto& pair : localTextures) {
                            // 使用 insert 的提示版本提升性能
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::textures.find(cacheKey) == GlobalCache::textures.end()) {
                                GlobalCache::textures.insert({ cacheKey, pair.second });
                            }
                        }

                        // 合并 blockstates
                        for (auto& pair : localBlockstates) {
                            // 检测键是否已存在
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::blockstates.find(cacheKey) == GlobalCache::blockstates.end()) {
                                GlobalCache::blockstates.insert({ cacheKey, pair.second });
                            }
                        }

                        // 合并 models（带冲突检测）
                        for (auto& pair : localModels) {
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::models.find(cacheKey) == GlobalCache::models.end()) {
                                GlobalCache::models.insert({ cacheKey, pair.second });
                            }
                            
                        }

                        // 合并生物群系
                        for (auto& pair : localBiomes) {
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::biomes.find(cacheKey) == GlobalCache::biomes.end()) {
                                GlobalCache::biomes.insert({ cacheKey, pair.second });
                            }
                        }

                        // 合并色图
                        for (auto& pair : localColormaps) {
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::colormaps.find(cacheKey) == GlobalCache::colormaps.end()) {
                                GlobalCache::colormaps.insert({ cacheKey, pair.second });
                            }
                        }
                        for (auto& pair : localMcmetas) {
                            std::string cacheKey = currentModId + ":" + pair.first;
                            if (GlobalCache::mcmetaCache.find(cacheKey) == GlobalCache::mcmetaCache.end()) {
                                GlobalCache::mcmetaCache.insert({ cacheKey, pair.second });
                            }
                        }
                    }
                }
            }
            };

        // 根据硬件并发数创建线程池
        const unsigned numThreads = std::max<unsigned>(1, std::thread::hardware_concurrency());
        std::vector<std::future<void>> futures;

        GlobalCache::stopFlag.store(false);

        // 启动工作线程
        for (unsigned i = 0; i < numThreads; ++i) {
            futures.emplace_back(std::async(std::launch::async, worker));
        }

        // 等待所有线程完成
        for (auto& f : futures) {
            try {
                f.get();
            }
            catch (const std::exception& e) {
                std::cerr << "Thread error: " << e.what() << std::endl;
            }
        }

        GlobalCache::stopFlag.store(true);

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