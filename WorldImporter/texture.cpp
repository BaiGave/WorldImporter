#include "texture.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <chrono>

std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId) {

    // 构造缓存键
    std::string cacheKey = namespaceName + ":" + blockId;

    // 加锁保护对全局缓存的访问
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    // 查找缓存
    auto it = GlobalCache::textures.find(cacheKey);

    if (it != GlobalCache::textures.end()) {
        return it->second;
    }

    std::cerr << "Texture not found: " << cacheKey << std::endl;
    return {};
}

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath) {
    // 获取纹理数据
    std::vector<unsigned char> textureData = GetTextureData(namespaceName, blockId);

    // 检查是否找到了纹理数据
    if (!textureData.empty()) {
        // 获取当前工作目录（即 exe 所在的目录）
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string exePath = std::string(buffer);

        // 获取 exe 所在目录
        size_t pos = exePath.find_last_of("\\/");
        std::string exeDir = exePath.substr(0, pos);

        // 如果传入了 savePath, 则使用 savePath 作为保存目录，否则默认使用当前 exe 目录
        if (savePath.empty()) {
            savePath = exeDir + "\\textures";  // 默认保存到 exe 目录下的 textures 文件夹
        }
        else {
            savePath = exeDir + "\\" + savePath;  // 使用提供的 savePath 路径
        }

        // 创建保存目录（如果不存在）
        if (GetFileAttributesA(savePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 文件夹不存在，创建它
            if (!CreateDirectoryA(savePath.c_str(), NULL)) {
                std::cerr << "Failed to create directory: " << savePath << std::endl;
                return false;
            }
        }

        // 处理 blockId，去掉路径部分，保留最后的文件名
        size_t lastSlashPos = blockId.find_last_of("/\\");
        std::string fileName = (lastSlashPos == std::string::npos) ? blockId : blockId.substr(lastSlashPos + 1);

        // 构建保存路径，使用处理后的 blockId 作为文件名
        std::string filePath = savePath + "\\" + fileName + ".png";

        // 检查目标路径是否已有文件存在
        //if (GetFileAttributesA(filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        //    // 文件已经存在，输出消息
        //    std::cout << "Texture file already exists at " << filePath << std::endl;
        //    return false;
        //}

        // 保存纹理文件
        std::ofstream outputFile(filePath, std::ios::binary);

        //返回savePath，作为value
        savePath = filePath;

        if (outputFile.is_open()) {
            outputFile.write(reinterpret_cast<const char*>(textureData.data()), textureData.size());
        }
        else {
            std::cerr << "Failed to open output file!" << std::endl;
            return false;
        }
    }
    else {
        std::cerr << "Failed to retrieve texture for " << blockId << std::endl;
        return false;
    }

    return true;
}


// 打印 textureCache 的内容
void PrintTextureCache(const std::unordered_map<std::string, std::vector<unsigned char>>& textureCache) {
    std::cout << "Texture Cache Contents:" << std::endl;
    std::cout << "----------------------------------------" << std::endl;

    if (textureCache.empty()) {
        std::cout << "Cache is empty." << std::endl;
        return;
    }

    for (const auto& entry : textureCache) {
        const std::string& cacheKey = entry.first;  // 缓存键（命名空间:资源路径）
        const std::vector<unsigned char>& textureData = entry.second;  // 纹理数据

        std::cout << "Key: " << cacheKey << std::endl;
        std::cout << "Data Size: " << textureData.size() << " bytes" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
    }

    std::cout << "Total Cached Textures: " << textureCache.size() << std::endl;
}