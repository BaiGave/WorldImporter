#include "MemoryMonitor.h"
#include "EntityBlock.h" // 需要包含 EntityBlock 的定义来计算其大小
#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <numeric> // For std::accumulate

// 辅助函数，估算 vector<T> 的深层内存占用
template <typename T>
size_t estimate_vector_memory(const std::vector<T>& vec) {
    size_t memory = sizeof(std::vector<T>); // vector 对象本身的大小
    if (vec.capacity() > 0) {
        memory += vec.capacity() * sizeof(T); // 已分配的内存
    }
    // 如果 T 是复杂类型，可能需要进一步估算每个元素的大小
    // 例如，如果 T 是 std::string 或其他容器
    if constexpr (std::is_same_v<T, std::string>) {
        for (const auto& str : vec) {
            memory += sizeof(std::string); // string 对象本身
            memory += str.capacity();      // string 内部 buffer
        }
    }
    else if constexpr (std::is_same_v<T, std::vector<int>>) {
        for (const auto& inner_vec : vec) {
            memory += estimate_vector_memory(inner_vec);
        }
    }
    // 对于 std::shared_ptr<EntityBlock>，我们估算 EntityBlock 对象本身的大小
    // 注意：这不包括 EntityBlock 内部可能动态分配的更深层内存
    else if constexpr (std::is_same_v<T, std::shared_ptr<EntityBlock>>) {
        for (const auto& ptr : vec) {
            if (ptr) {
                // 尝试获取派生类的大小，如果 EntityBlock 是多态基类
                // 这是一个简化的例子，实际中可能需要更复杂的RTTI或虚函数机制
                if (auto yuushya_ptr = std::dynamic_pointer_cast<YuushyaShowBlockEntity>(ptr)) {
                    memory += sizeof(YuushyaShowBlockEntity);
                    // 进一步估算 YuushyaShowBlockEntity 内部 vector<YuushyaBlockEntry> 的大小
                    memory += estimate_vector_memory(yuushya_ptr->blocks);
                }
                else if (auto littletiles_ptr = std::dynamic_pointer_cast<LittleTilesTilesEntity>(ptr)) {
                    memory += sizeof(LittleTilesTilesEntity);
                    // 进一步估算 LittleTilesTilesEntity 内部 vector<LittleTilesTileEntry> 的大小
                    memory += estimate_vector_memory(littletiles_ptr->tiles);
                }
                else {
                    memory += sizeof(EntityBlock); // 基类大小
                }
            }
        }
    }
    else if constexpr (std::is_same_v<T, YuushyaBlockEntry>) {
        for (const auto& entry : vec) {
            memory += sizeof(YuushyaBlockEntry); // 结构体本身
            memory += estimate_vector_memory(entry.showPos);
            memory += estimate_vector_memory(entry.showRotation);
            memory += estimate_vector_memory(entry.showScales);
        }
    }
    else if constexpr (std::is_same_v<T, LittleTilesTileEntry>) {
         for (const auto& entry : vec) {
            memory += sizeof(LittleTilesTileEntry);
            memory += sizeof(std::string) + entry.blockName.capacity();
            memory += estimate_vector_memory(entry.color);
            for(const auto& box_data : entry.boxDataList) {
                memory += estimate_vector_memory(box_data);
            }
        }
    }

    return memory;
}

// 辅助函数，估算 SectionCacheEntry 的深层内存占用
size_t estimate_section_cache_entry_memory(const SectionCacheEntry& entry) {
    size_t memory = sizeof(SectionCacheEntry);
    memory += estimate_vector_memory(entry.skyLight);
    memory += estimate_vector_memory(entry.blockLight);
    memory += estimate_vector_memory(entry.blockData);
    memory += estimate_vector_memory(entry.biomeData);
    memory += estimate_vector_memory(entry.blockPalette); // vector<string>
    return memory;
}

// 辅助函数，估算 unordered_map<K, V> 的深层内存占用
template <typename K, typename V, typename Hash>
size_t estimate_unordered_map_memory(const std::unordered_map<K, V, Hash>& map) {
    size_t memory = sizeof(std::unordered_map<K, V, Hash>); // map 对象本身
    // 估算桶的内存
    memory += map.bucket_count() * (sizeof(void*) + sizeof(size_t)); // 简化估算

    for (const auto& pair : map) {
        memory += sizeof(K) + sizeof(V); // 键和值对象本身的大小
        // 如果 K 或 V 是复杂类型，需要进一步估算
        if constexpr (std::is_same_v<K, std::string>) {
            memory += pair.first.capacity();
        }
        // 对于 SectionCacheEntry
        if constexpr (std::is_same_v<V, SectionCacheEntry>) {
            memory += estimate_section_cache_entry_memory(pair.second);
        }
        // 对于 vector<shared_ptr<EntityBlock>>
        else if constexpr (std::is_same_v<V, std::vector<std::shared_ptr<EntityBlock>>>) {
            memory += estimate_vector_memory(pair.second);
        }
        // 对于 unordered_map<string, vector<int>>
        else if constexpr (std::is_same_v<V, std::unordered_map<std::string, std::vector<int>>>) {
            memory += estimate_unordered_map_memory(pair.second);
        }
        else if constexpr (std::is_same_v<V, std::vector<int>>) { // heightMapCache 的内层 map 的 V
             memory += estimate_vector_memory(pair.second);
        }
         else if constexpr (std::is_same_v<K, std::string> && std::is_same_v<V, std::vector<int>>) { // heightMapCache 的内层 map
            memory += pair.first.capacity(); // string key
            memory += estimate_vector_memory(pair.second); // vector<int> value
        }
    }
    return memory;
}

namespace MemoryMonitor {

static std::atomic<bool> monitoring_active = false;
static std::thread monitor_thread;

void MonitorTask(
    const SectionCacheType* sectionCache,
    std::shared_mutex* sectionCacheMutex,
    const EntityBlockCacheType* entityBlockCache,
    std::shared_mutex* entityBlockCacheMutex,
    const HeightMapCacheType* heightMapCache,
    std::shared_mutex* heightMapCacheMutex
) {
    while (monitoring_active) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        if (!monitoring_active) break;

        size_t section_cache_size_bytes = 0;
        size_t entity_block_cache_size_bytes = 0;
        size_t height_map_cache_size_bytes = 0;

        {
            std::shared_lock<std::shared_mutex> lock(*sectionCacheMutex);
            section_cache_size_bytes = estimate_unordered_map_memory(*sectionCache);
        }
        {
            std::shared_lock<std::shared_mutex> lock(*entityBlockCacheMutex);
            entity_block_cache_size_bytes = estimate_unordered_map_memory(*entityBlockCache);
        }
        {
            std::shared_lock<std::shared_mutex> lock(*heightMapCacheMutex);
            height_map_cache_size_bytes = estimate_unordered_map_memory(*heightMapCache);
        }

        std::cout << "--- Memory Usage --- (Approximate)" << std::endl;
        std::cout << "sectionCache:     " << section_cache_size_bytes / 1024.0 / 1024.0 << " MB" << std::endl;
        std::cout << "EntityBlockCache: " << entity_block_cache_size_bytes / 1024.0 / 1024.0 << " MB" << std::endl;
        std::cout << "heightMapCache:   " << height_map_cache_size_bytes / 1024.0 / 1024.0 << " MB" << std::endl;
        std::cout << "----------------------" << std::endl;
    }
}

void StartMonitoring(
    const SectionCacheType& sectionCache,
    std::shared_mutex& sectionCacheMutexRef,
    const EntityBlockCacheType& entityBlockCache,
    std::shared_mutex& entityBlockCacheMutexRef,
    const HeightMapCacheType& heightMapCache,
    std::shared_mutex& heightMapCacheMutexRef
) {
    if (monitoring_active) {
        return; // 已经在监控
    }
    monitoring_active = true;
    // 传递指针和引用给线程函数
    monitor_thread = std::thread(MonitorTask, &sectionCache, &sectionCacheMutexRef, &entityBlockCache, &entityBlockCacheMutexRef, &heightMapCache, &heightMapCacheMutexRef);
    std::cout << "Memory monitoring started." << std::endl;
}

void StopMonitoring() {
    if (monitoring_active) {
        monitoring_active = false;
        if (monitor_thread.joinable()) {
            monitor_thread.join();
        }
        std::cout << "Memory monitoring stopped." << std::endl;
    }
}

} // namespace MemoryMonitor 