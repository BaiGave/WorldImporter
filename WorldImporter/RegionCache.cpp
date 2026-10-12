#include "RegionCache.h"
#include "fileutils.h"
#include "config.h"
#include <sstream>
#include <fstream>
#include <iostream>
#include <filesystem> // 用于检测目录是否存在
#include <cstdint> // 用于 uint8_t, uint32_t
#include <vector>
#include "locutil.h"
std::map<std::pair<int, int>, std::shared_ptr<const std::vector<char>>> regionCache;
std::mutex regionCacheMutex;

// 区域缓存总字节数(受 regionCacheMutex 保护)
static size_t regionCacheBytes = 0;
// 区域缓存上限:1 GiB。超过时整体清空,避免大范围导入时内存持续膨胀。
// 安全性:清空只释放缓存持有的数据,调用方通过 shared_ptr 已持有的数据仍然有效。
static const size_t REGION_CACHE_MAX_BYTES = 1024ull * 1024ull * 1024ull;

// 将维度ID(如 minecraft:overworld)拆分为命名空间和名称两部分
static void SplitDimensionId(const std::string& sel, std::string& ns, std::string& name) {
    auto pos = sel.find(':');
    if (pos == std::string::npos) {
        ns = "minecraft";
        name = sel;
    } else {
        ns = sel.substr(0, pos);
        name = sel.substr(pos + 1);
    }
}

// 在存档根目录下扫描 DIM<number>/region 形式的目录
// 用于支持 pre-1.16 Forge 模组维度的遗留布局
static std::string FindLegacyDimRegion(const std::string& base) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(base, ec)) {
        if (!entry.is_directory()) continue;
        std::string name = entry.path().filename().string();
        // 匹配 DIM 后跟数字(可带负号),如 DIM2、DIM-1、DIM7
        if (name.rfind("DIM", 0) != 0) continue;
        bool numeric = true;
        for (size_t i = 3; i < name.size(); ++i) {
            if (!(name[i] == '-' || (name[i] >= '0' && name[i] <= '9'))) {
                numeric = false;
                break;
            }
        }
        if (!numeric || name.size() <= 3) continue;
        std::string regionDir = entry.path().string() + "/region";
        if (std::filesystem::exists(regionDir)) {
            return regionDir;
        }
    }
    return "";
}

// 根据配置的选择维度和存档路径，返回对应的 region 目录路径
// 支持新版MC(1.21.4+)的 dimensions/<ns>/<dim>/region/ 结构,
// 同时兼容旧版的 /region、/DIM-1/region、/DIM1/region 结构
static std::string GetRegionDirectory() {
    const std::string& sel = config.selectedDimension;
    const std::string& base = config.worldPath;

    std::string ns, dimName;
    SplitDimensionId(sel, ns, dimName);

    // 候选路径:新布局在前,旧布局在后
    std::vector<std::string> candidates;

    // 26.1+ 新布局:所有维度(含模组)统一放在 dimensions/<ns>/<name>/region
    // 注意:1.16+ 的自定义/模组维度也使用此路径,因此新旧版本通用
    candidates.push_back(base + "/dimensions/" + ns + "/" + dimName + "/region");

    // 旧布局:原版维度使用各自约定位置
    if (sel == "minecraft:overworld") {
        candidates.push_back(base + "/region");
    } else if (sel == "minecraft:the_nether") {
        candidates.push_back(base + "/DIM-1/region");
    } else if (sel == "minecraft:the_end") {
        candidates.push_back(base + "/DIM1/region");
    } else {
        // 模组/自定义维度:扫描 DIM<number>/region(pre-1.16 Forge 遗留布局)
        std::string legacyDim = FindLegacyDimRegion(base);
        if (!legacyDim.empty()) {
            candidates.push_back(legacyDim);
        }
    }

    for (const auto& dir : candidates) {
        if (std::filesystem::exists(dir)) {
            return dir;
        }
    }

    // 所有候选路径都不存在,打印警告并列出尝试过的路径
    std::cerr << "警告: 维度目录不存在,已尝试以下路径:" << std::endl;
    for (const auto& dir : candidates) {
        std::cerr << "  - " << dir << std::endl;
    }
    // 回退到主世界 region(保持原有兜底行为)
    return base + "/region";
}

// 维度目录在一次运行内不变,缓存结果避免每个区块都重复扫描文件系统
// (旧实现:HasChunk 每区块调用一次 GetRegionDirectory,含多次 stat/目录遍历)
static std::string GetRegionDirectoryCached() {
    static std::mutex dirMutex;
    static std::string cachedDir;
    static std::string cachedDimension;
    static bool cached = false;
    std::lock_guard<std::mutex> lock(dirMutex);
    if (!cached || cachedDimension != config.selectedDimension) {
        cachedDir = GetRegionDirectory();
        cachedDimension = config.selectedDimension;
        cached = true;
    }
    return cachedDir;
}

//读取.mca文件到内存
std::vector<char> ReadFileToMemory(const std::string& regionDirPath, int regionX, int regionZ) {
    // 构造区域文件的路径
    std::ostringstream filePathStream;
    filePathStream << regionDirPath << "/r." << regionX << "." << regionZ << ".mca";
    std::string filePath = filePathStream.str();

    // 打开文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "错误: 打开文件失败!" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 将文件内容读取到文件数据中
    std::vector<char> fileData;
    fileData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (fileData.empty()) {
        std::cerr << "错误: 文件为空或读取失败!" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 返回读取到的文件数据
    return fileData;
}


std::shared_ptr<const std::vector<char>> GetRegionFromCache(int regionX, int regionZ) {
    auto regionKey = std::make_pair(regionX, regionZ);
    std::lock_guard<std::mutex> lock(regionCacheMutex);
    auto it = regionCache.find(regionKey);
    if (it != regionCache.end()) {
        return it->second;
    }

    std::string regionDir = GetRegionDirectoryCached();
    std::vector<char> fileData = ReadFileToMemory(regionDir, regionX, regionZ);

    // 超过上限时整体清空(调用方持有的 shared_ptr 不受影响)
    if (!fileData.empty() && regionCacheBytes + fileData.size() > REGION_CACHE_MAX_BYTES) {
        std::cout << "[RegionCache] 缓存达到上限(" << (REGION_CACHE_MAX_BYTES / 1024 / 1024)
                  << " MB),已清空 " << regionCache.size() << " 个区域文件缓存" << std::endl;
        regionCache.clear();
        regionCacheBytes = 0;
    }

    auto dataPtr = std::make_shared<const std::vector<char>>(std::move(fileData));
    regionCacheBytes += dataPtr->size();
    regionCache.emplace(regionKey, dataPtr);
    return dataPtr;
}

// 新增:判断指定 chunk 是否存在于 region 文件中
bool HasChunk(int chunkX, int chunkZ) {
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);
    std::string regionDir = GetRegionDirectoryCached();
    std::ostringstream filePathStream;
    filePathStream << regionDir << "/r." << regionX << "." << regionZ << ".mca";
    std::string filePath = filePathStream.str();
    if (!std::filesystem::exists(filePath)) {
        return false;
    }
    auto dataPtr = GetRegionFromCache(regionX, regionZ);
    const auto& data = *dataPtr;
    int localX = chunkX - regionX * 32;
    int localZ = chunkZ - regionZ * 32;
    if (localX < 0 || localX >= 32 || localZ < 0 || localZ >= 32) {
        return false;
    }
    size_t index = (localX + localZ * 32) * 4;
    if (data.size() < index + 4) {
        return false;
    }
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    uint32_t offset = ((uint32_t)bytes[index] << 16) | ((uint32_t)bytes[index + 1] << 8) | (uint32_t)bytes[index + 2];
    return offset != 0;
}