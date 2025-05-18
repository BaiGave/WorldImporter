#include "RegionCache.h"
#include "fileutils.h"
#include "config.h"
#include <sstream>
#include <fstream>
std::unordered_map<std::pair<int, int>, std::vector<char>, pair_hash> regionCache(1024);

//读取.mca文件到内存
std::vector<char> ReadFileToMemory(const std::string& directoryPath, int regionX, int regionZ) {
    // 构造区域文件的路径
    std::ostringstream filePathStream;
    filePathStream << directoryPath << "/region/r." << regionX << "." << regionZ << ".mca";
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
        std::vector<char> fileData = ReadFileToMemory(config.worldPath, regionX, regionZ);
        auto result = regionCache.emplace(regionKey, std::move(fileData));
        it = result.first;
    }
    return it->second;
}