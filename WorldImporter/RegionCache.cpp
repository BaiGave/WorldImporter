#include "RegionCache.h"
#include "fileutils.h"
#include "config.h"
#include <sstream>
#include <fstream>
#include <filesystem> // 用于检测目录是否存在
#include <cstdint> // 用于 uint8_t, uint32_t
#include <vector>
#include "locutil.h"
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache(1024);

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
// 同时支持 26.1+ 新布局(dimensions/<ns>/<name>/region)与旧布局(region/DIM-1/DIM1/DIM<number>)
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


const std::vector<char>& GetRegionFromCache(int regionX, int regionZ) {
    auto regionKey = std::make_pair(regionX, regionZ);
    auto it = regionCache.find(regionKey);
    if (it == regionCache.end()) {
        std::string regionDir = GetRegionDirectory();
        std::vector<char> fileData = ReadFileToMemory(regionDir, regionX, regionZ);
        auto result = regionCache.emplace(regionKey, std::move(fileData));
        it = result.first;
    }
    return it->second;
}

// 新增:判断指定 chunk 是否存在于 region 文件中
bool HasChunk(int chunkX, int chunkZ) {
    int regionX, regionZ;
    chunkToRegion(chunkX, chunkZ, regionX, regionZ);
    std::string regionDir = GetRegionDirectory();
    std::ostringstream filePathStream;
    filePathStream << regionDir << "/r." << regionX << "." << regionZ << ".mca";
    std::string filePath = filePathStream.str();
    if (!std::filesystem::exists(filePath)) {
        return false;
    }
    const auto& data = GetRegionFromCache(regionX, regionZ);
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