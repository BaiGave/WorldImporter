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


}

// ========= 外部依赖定义 =========
//std::string currentSelectedGameVersion; // 需在版本模块初始化
//std::unordered_map<std::string, std::vector<FolderData>> VersionCache;
//std::unordered_map<std::string, std::vector<FolderData>> resourcePacksCache;
//std::unordered_map<std::string, std::vector<FolderData>> modListCache;

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

            // 添加原版JAR
            if (VersionCache.count(currentSelectedGameVersion)) {
                for (const auto& fd : VersionCache[currentSelectedGameVersion]) {
                    GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                }
            }

            // 添加资源包
            if (resourcePacksCache.count(currentSelectedGameVersion)) {
                for (const auto& fd : resourcePacksCache[currentSelectedGameVersion]) {
                    GlobalCache::jarQueue.push(string_to_wstring(fd.path));
                }
            }

            // 添加模组
            if (modListCache.count(currentSelectedGameVersion)) {
                for (const auto& fd : modListCache[currentSelectedGameVersion]) {
                    if (fd.namespaceName == "vanilla" || fd.namespaceName == "resourcePack") continue;
                    GlobalCache::jarQueue.push(string_to_wstring(fd.path));
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
                    // 局部缓存临时存储
                    std::unordered_map<std::string, std::vector<unsigned char>> localTextures;
                    std::unordered_map<std::string, nlohmann::json> localBlockstates;
                    std::unordered_map<std::string, nlohmann::json> localModels;
                    std::unordered_map<std::string, nlohmann::json> localBiomes;
                    std::unordered_map<std::string, std::vector<unsigned char>> localColormaps;

                    

                    reader.cacheAllResources(localTextures, localBlockstates, localModels);
                    reader.cacheAllBiomes(localBiomes);
                    reader.cacheAllColormaps(localColormaps);
                    // 合并到全局缓存
                    {
                        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

                        // 手动合并 textures
                        for (auto& pair : localTextures) {
                            // 使用 insert 的提示版本提升性能
                            auto hint = GlobalCache::textures.end();
                            GlobalCache::textures.insert(hint, std::move(pair));
                        }

                        // 合并 blockstates
                        for (auto& pair : localBlockstates) {
                            // 检测键是否已存在
                            if (GlobalCache::blockstates.find(pair.first) == GlobalCache::blockstates.end()) {
                                GlobalCache::blockstates.insert(std::move(pair));
                            }
                        }

                        // 合并 models（带冲突检测）
                        for (auto& pair : localModels) {
                            GlobalCache::models.emplace(pair.first, std::move(pair.second));
                        }

                        // 合并生物群系
                        for (auto& pair : localBiomes) {
                            GlobalCache::biomes.insert(pair);
                        }

                        // 合并色图
                        for (auto& pair : localColormaps) {
                            GlobalCache::colormaps.insert(pair);
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
            << " - Blockstates: " << GlobalCache::blockstates.size() << "\n"
            << " - Models: " << GlobalCache::models.size() << "\n"
            << " - Biomes: " << GlobalCache::biomes.size() << "\n"
            << " - Colormaps: " << GlobalCache::colormaps.size() << "\n"
            << " - Time: " << ms << "ms" << std::endl;
        });
}

// ========= 工具方法实现 =========
bool ValidateCacheIntegrity() {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
    return !GlobalCache::textures.empty()
        && !GlobalCache::blockstates.empty()
        && !GlobalCache::models.empty();
}

void HotReloadJar(const std::wstring& jarPath) {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    JarReader reader(jarPath);
    if (reader.open()) {
        reader.cacheAllResources(
            GlobalCache::textures,
            GlobalCache::blockstates,
            GlobalCache::models
        );
        std::cout << "Hot Reloaded: " << wstring_to_string(jarPath) << "\n"
            << " - Current Textures: " << GlobalCache::textures.size() << "\n"
            << " - Current Blockstates: " << GlobalCache::blockstates.size() << "\n"
            << " - Current Models: " << GlobalCache::models.size() << std::endl;
    }
}