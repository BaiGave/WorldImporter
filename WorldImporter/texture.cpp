#include "texture.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <chrono>

std::unordered_map<std::string, std::string> texturePathCache; // 定义材质路径缓存

//已弃用
std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId) {
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

    // 按照 JAR 文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
        auto it = GlobalCache::textures.find(cacheKey);
        if (it != GlobalCache::textures.end()) {
            return it->second;
        }
    }

    std::cerr << "Texture not found: " << namespaceName << ":" << blockId << std::endl;
    return {};
}

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath) {
    std::vector<unsigned char> textureData;
    nlohmann::json mcmetaData;

    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);

        // 按照 JAR 文件的加载顺序逐个查找纹理数据
        for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
            const std::string& modId = GlobalCache::jarOrder[i];
            std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
            auto textureIt = GlobalCache::textures.find(cacheKey);
            if (textureIt != GlobalCache::textures.end()) {
                textureData = textureIt->second;

                // 获取动态材质数据（如果存在）
                auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
                if (mcmetaIt != GlobalCache::mcmetaCache.end()) {
                    mcmetaData = mcmetaIt->second;
                }
                break;
            }
        }

        if (textureData.empty()) {
            std::cerr << "Texture not found: " << namespaceName << ":" << blockId << std::endl;
            return false;
        }
    }

    if (!textureData.empty()) {
        // 处理保存路径
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string exePath(buffer);
        size_t pos = exePath.find_last_of("\\/");
        std::string exeDir = exePath.substr(0, pos);

        if (savePath.empty()) {
            savePath = exeDir + "\\textures";
        }
        else {
            savePath = exeDir + "\\" + savePath;
        }

        // 创建保存目录
        if (GetFileAttributesA(savePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            CreateDirectoryA(savePath.c_str(), NULL);
        }

        // 提取文件名（不含路径）
        size_t lastSlashPos = blockId.find_last_of("/\\");
        std::string fileName = (lastSlashPos == std::string::npos) ? blockId : blockId.substr(lastSlashPos + 1);

        // 创建 namespace 目录
        std::string namespaceDir = savePath + "\\" + namespaceName;
        CreateDirectoryA(namespaceDir.c_str(), NULL);

        // 递归创建子目录
        std::string pathPart = blockId.substr(0, lastSlashPos);
        std::string currentPath = namespaceDir;
        size_t start = 0;
        size_t end;
        while ((end = pathPart.find('/', start)) != std::string::npos) {
            std::string dir = pathPart.substr(start, end - start);
            currentPath += "\\" + dir;
            CreateDirectoryA(currentPath.c_str(), NULL);
            start = end + 1;
        }
        std::string finalDir = currentPath + "\\" + pathPart.substr(start);
        CreateDirectoryA(finalDir.c_str(), NULL);

        // 保存主 PNG 文件
        std::string filePath = finalDir + "\\" + fileName + ".png";
        std::ofstream outputFile(filePath, std::ios::binary);
        savePath = filePath;

        if (outputFile.is_open()) {
            outputFile.write(reinterpret_cast<const char*>(textureData.data()), textureData.size());
            outputFile.close();

            // 保存 .mcmeta 文件（如果存在）
            if (!mcmetaData.empty()) {
                std::string mcmetaFilePath = filePath + ".mcmeta";
                std::ofstream mcmetaFile(mcmetaFilePath);
                if (mcmetaFile.is_open()) {
                    mcmetaFile << mcmetaData.dump(4); // 格式化输出 JSON
                    mcmetaFile.close();
                }
                else {
                    std::cerr << "Failed to save .mcmeta file: " << mcmetaFilePath << std::endl;
                }
            }

            // 保存 PBR 贴图
            std::vector<std::string> pbrSuffixes = { "_n", "_a", "_s" }; // PBR 贴图的后缀
            for (const auto& suffix : pbrSuffixes) {
                std::string pbrCacheKey = namespaceName + ":" + blockId + suffix;

                std::vector<unsigned char> pbrTextureData;
                {
                    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
                    auto pbrTextureIt = GlobalCache::textures.find(pbrCacheKey);
                    if (pbrTextureIt != GlobalCache::textures.end()) {
                        pbrTextureData = pbrTextureIt->second;
                    }
                }

                if (!pbrTextureData.empty()) {
                    std::string pbrFilePath = finalDir + "\\" + fileName + suffix + ".png";
                    std::ofstream pbrOutputFile(pbrFilePath, std::ios::binary);
                    if (pbrOutputFile.is_open()) {
                        pbrOutputFile.write(reinterpret_cast<const char*>(pbrTextureData.data()), pbrTextureData.size());
                        pbrOutputFile.close();
                    }
                    else {
                        std::cerr << "Failed to save PBR texture: " << pbrFilePath << std::endl;
                    }
                }
            }

            return true;
        }
        else {
            std::cerr << "Failed to save texture: " << filePath << std::endl;
            return false;
        }
    }
    else {
        std::cerr << "Texture data is empty for " << namespaceName << ":" << blockId << std::endl;
        return false;
    }
}



void RegisterTexture(const std::string& namespaceName, const std::string& pathPart, const std::string& savePath) {
    std::string cacheKey = namespaceName + ":" + pathPart;

    std::lock_guard<std::mutex> lock(texturePathCacheMutex); // 加锁保护对缓存的访问
    // 检查缓存中是否已存在该材质
    auto cacheIt = texturePathCache.find(cacheKey);
    if (cacheIt != texturePathCache.end()) {
        // 如果存在，直接返回
        return;
    }

    // 保存材质路径到缓存
    texturePathCache[cacheKey] = savePath;
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