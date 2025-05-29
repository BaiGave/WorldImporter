#include "RegionCache.h"
#include "fileutils.h"
#include "config.h"
#include <sstream>
#include <fstream>
#include <filesystem> // 用于检测目录是否存在
#include <cstdint> // 用于 uint8_t, uint32_t
#include "locutil.h"
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache(1024);

// 根据配置的选择维度和存档路径，返回对应的 region 目录路径
static std::string GetRegionDirectory() {
    const std::string& sel = config.selectedDimension;
    const std::string& base = config.worldPath;
    std::string dir;
    if (sel == "minecraft:overworld") {
        dir = base + "/region";
    } else if (sel == "minecraft:the_nether") {
        dir = base + "/DIM-1/region";
    } else if (sel == "minecraft:the_end") {
        dir = base + "/DIM1/region";
    } else {
        auto pos = sel.find(':');
        if (pos == std::string::npos) {
            dir = base + "/region";
        } else {
            std::string ns = sel.substr(0, pos);
            std::string dimName = sel.substr(pos + 1);
            dir = base + "/dimensions/" + ns + "/" + dimName + "/region";
        }
    }
    if (!std::filesystem::exists(dir)) {
        std::cerr << "警告: 维度目录不存在: " << dir << std::endl;
        // 回退到主世界 region
        return base + "/region";
    }
    return dir;
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