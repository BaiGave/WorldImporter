#include "texture.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <chrono>

std::unordered_map<std::string, std::string> texturePathCache; // 定义材质路径缓存

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath) {
    std::vector<unsigned char> textureData;
    nlohmann::json mcmetaData;

    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        // 按照 JAR 文件的加载顺序逐个查找主纹理数据
        for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
            const std::string& modId = GlobalCache::jarOrder[i];
            std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
            auto textureIt = GlobalCache::textures.find(cacheKey);
            if (textureIt != GlobalCache::textures.end()) {
                textureData = textureIt->second;

                // 获取动态材质数据(如果存在)
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

    // 提取文件名(不含路径)
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

        // 保存 .mcmeta 文件(如果存在)
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

        // 保存 PBR 贴图,后缀分别为 _n、_a、_s
        std::vector<std::string> pbrSuffixes = { "_n", "_a", "_s" };
        for (const auto& suffix : pbrSuffixes) {
            std::vector<unsigned char> pbrTextureData;
            nlohmann::json pbrMcmetaData;

            // 按照 GlobalCache::jarOrder 顺序查找 PBR 贴图数据
            {
                std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
                for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
                    const std::string& modId = GlobalCache::jarOrder[i];
                    std::string pbrCacheKey = modId + ":" + namespaceName + ":" + blockId + suffix;
                    auto pbrTextureIt = GlobalCache::textures.find(pbrCacheKey);
                    if (pbrTextureIt != GlobalCache::textures.end()) {
                        pbrTextureData = pbrTextureIt->second;

                        // 获取 PBR 贴图的 .mcmeta 数据(如果存在)
                        auto pbrMcmetaIt = GlobalCache::mcmetaCache.find(pbrCacheKey);
                        if (pbrMcmetaIt != GlobalCache::mcmetaCache.end()) {
                            pbrMcmetaData = pbrMcmetaIt->second;
                        }
                        break;
                    }
                }
            }

            if (!pbrTextureData.empty()) {
                // 保存 PBR 贴图
                std::string pbrFilePath = finalDir + "\\" + fileName + suffix + ".png";
                std::ofstream pbrOutputFile(pbrFilePath, std::ios::binary);
                if (pbrOutputFile.is_open()) {
                    pbrOutputFile.write(reinterpret_cast<const char*>(pbrTextureData.data()), pbrTextureData.size());
                    pbrOutputFile.close();

                    // 保存 PBR 贴图的 .mcmeta 文件(如果存在)
                    if (!pbrMcmetaData.empty()) {
                        std::string pbrMcmetaFilePath = pbrFilePath + ".mcmeta";
                        std::ofstream pbrMcmetaFile(pbrMcmetaFilePath);
                        if (pbrMcmetaFile.is_open()) {
                            pbrMcmetaFile << pbrMcmetaData.dump(4); // 格式化输出 JSON
                            pbrMcmetaFile.close();
                        }
                        else {
                            std::cerr << "Failed to save PBR .mcmeta file: " << pbrMcmetaFilePath << std::endl;
                        }
                    }
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

void RegisterTexture(const std::string& namespaceName, const std::string& pathPart, const std::string& savePath) {
    std::string cacheKey = namespaceName + ":" + pathPart;

    std::lock_guard<std::mutex> lock(texturePathCacheMutex); // 加锁保护对缓存的访问
    // 检查缓存中是否已存在该材质
    auto cacheIt = texturePathCache.find(cacheKey);
    if (cacheIt != texturePathCache.end()) {
        // 如果存在,直接返回
        return;
    }

    // 保存材质路径到缓存
    texturePathCache[cacheKey] = savePath;
}

// 解析.mcmeta文件并确定材质类型
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType) {
    // 默认为普通材质
    outType = NORMAL;
    
    auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
    if (mcmetaIt == GlobalCache::mcmetaCache.end() || mcmetaIt->second.empty()) {
        // 没有找到.mcmeta数据,视为普通材质
        return false;
    }
    
    const nlohmann::json& mcmetaData = mcmetaIt->second;
    
    // 检查是否为动态材质
    if (mcmetaData.contains("animation")) {
        outType = ANIMATED;
        return true;
    }
    
    // 检查是否为CTM材质
    if (mcmetaData.contains("ctm")) {
        outType = CTM;
        return true;
    }
    
    // 其他类型的.mcmeta文件,保持为普通材质
    return true;
}

// 检测材质类型
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath) {
    MaterialType type = NORMAL;
    
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
    
    // 按照JAR文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + texturePath;
        
        // 检查.mcmeta文件是否存在
        if (ParseMcmetaFile(cacheKey, type)) {
            break;
        }
    }
    
    return type;
}