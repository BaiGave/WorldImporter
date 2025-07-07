#include "texture.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <chrono>

std::unordered_map<std::string, std::string> texturePathCache; // 定义材质路径缓存
std::unordered_map<std::string, TextureDimension> textureDimensionCache; // 定义材质尺寸缓存

// PNG文件头部解析，读取图像尺寸
bool GetPNGDimensions(const std::vector<unsigned char>& pngData, int& width, int& height) {
    // PNG文件至少需要24字节头部
    if (pngData.size() < 24) {
        return false;
    }
    
    // 检查PNG签名
    const unsigned char pngSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    for (int i = 0; i < 8; i++) {
        if (pngData[i] != pngSignature[i]) {
            return false; // 不是有效的PNG文件
        }
    }
    
    // IHDR块固定在签名后，包含宽度和高度信息
    // 宽度: 字节16-19
    // 高度: 字节20-23
    width = (pngData[16] << 24) | (pngData[17] << 16) | (pngData[18] << 8) | pngData[19];
    height = (pngData[20] << 24) | (pngData[21] << 16) | (pngData[22] << 8) | pngData[23];
    
    return (width > 0 && height > 0);
}

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath) {
    std::vector<unsigned char> textureData;
    nlohmann::json mcmetaData;
    int width = 0, height = 0;
    bool isDynamic = false;

    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        // 按照 JAR 文件的加载顺序逐个查找主纹理数据
        for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
            const std::string& modId = GlobalCache::jarOrder[i];
            std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
            auto textureIt = GlobalCache::textures.find(cacheKey);
            if (textureIt != GlobalCache::textures.end()) {
                textureData = textureIt->second;
                
                // 读取PNG尺寸
                if (GetPNGDimensions(textureData, width, height)) {
                    // 保存到尺寸缓存
                    std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                    textureDimensionCache[cacheKey] = TextureDimension(width, height);
                }

                // 获取动态材质数据(如果存在)
                auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
                if (mcmetaIt != GlobalCache::mcmetaCache.end()) {
                    mcmetaData = mcmetaIt->second;
                    if (mcmetaData.contains("animation")) {
                        isDynamic = true;
                    }
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
            int pbrWidth = 0, pbrHeight = 0;

            // 按照 GlobalCache::jarOrder 顺序查找 PBR 贴图数据
            {
                std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
                for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
                    const std::string& modId = GlobalCache::jarOrder[i];
                    std::string pbrCacheKey = modId + ":" + namespaceName + ":" + blockId + suffix;
                    auto pbrTextureIt = GlobalCache::textures.find(pbrCacheKey);
                    if (pbrTextureIt != GlobalCache::textures.end()) {
                        pbrTextureData = pbrTextureIt->second;
                        
                        // 读取PBR贴图的尺寸
                        if (GetPNGDimensions(pbrTextureData, pbrWidth, pbrHeight)) {
                            // 保存到尺寸缓存
                            std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                            textureDimensionCache[pbrCacheKey] = TextureDimension(pbrWidth, pbrHeight);
                        }

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
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType, float& outAspectRatio) {
    // 默认为普通材质，默认长宽比为1.0
    outType = NORMAL;
    outAspectRatio = 1.0f;
    
    // 首先尝试从尺寸缓存中获取图片实际尺寸
    {
        std::lock_guard<std::mutex> lock(textureDimensionMutex);
        auto dimIt = textureDimensionCache.find(cacheKey);
        if (dimIt != textureDimensionCache.end()) {
            // 先用图片实际尺寸初始化长宽比
            outAspectRatio = dimIt->second.aspectRatio;
        }
    }
    
    // 然后检查mcmeta数据
    auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
    if (mcmetaIt == GlobalCache::mcmetaCache.end() || mcmetaIt->second.empty()) {
        // 没有找到.mcmeta数据,视为普通材质
        return false;
    }
    
    const nlohmann::json& mcmetaData = mcmetaIt->second;
    
    // 检查是否为动态材质
    if (mcmetaData.contains("animation")) {
        outType = ANIMATED;
        
        // 计算动态材质帧数
        int frameCount = 1;
        
        // 优先从frames数组计算帧数
        if (mcmetaData["animation"].contains("frames") && mcmetaData["animation"]["frames"].is_array()) {
            frameCount = mcmetaData["animation"]["frames"].size();
        } 
        // 如果没有明确的frames数组，但图片尺寸已知且为垂直条带，则使用高/宽比估算帧数
        else {
            std::lock_guard<std::mutex> lock(textureDimensionMutex);
            auto dimIt = textureDimensionCache.find(cacheKey);
            if (dimIt != textureDimensionCache.end() && dimIt->second.width > 0) {
                const TextureDimension& dim = dimIt->second;
                // 如果高度明显大于宽度，可能是垂直排列的动画帧
                if (dim.height > dim.width) {
                    // 估算帧数，假设每帧是正方形
                    frameCount = dim.height / dim.width;
                    if (frameCount <= 0) frameCount = 1; // 安全检查
                }
            }
        }
        
        // 计算最终的长宽比（实际是帧数）
        outAspectRatio = static_cast<float>(frameCount);
        
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

// 向后兼容的原始函数版本
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType) {
    float dummyAspectRatio;
    return ParseMcmetaFile(cacheKey, outType, dummyAspectRatio);
}

// 检测材质类型
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath, float& outAspectRatio) {
    MaterialType type = NORMAL;
    outAspectRatio = 1.0f;
    
    std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
    
    // 按照JAR文件的加载顺序逐个查找
    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
        const std::string& modId = GlobalCache::jarOrder[i];
        std::string cacheKey = modId + ":" + namespaceName + ":" + texturePath;
        
        // 检查.mcmeta文件是否存在，同时获取长宽比
        if (ParseMcmetaFile(cacheKey, type, outAspectRatio)) {
            break;
        }
    }
    
    return type;
}

// 原始函数的重载版本，保持向后兼容性
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath) {
    float aspectRatio;
    return DetectMaterialType(namespaceName, texturePath, aspectRatio);
}