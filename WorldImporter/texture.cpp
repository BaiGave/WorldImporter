#include "texture.h"
#include "fileutils.h"
#include "include/stb_image.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>
#include <chrono>
#include <shared_mutex>
#include <unordered_map>

std::unordered_map<std::string, std::string> texturePathCache; // 定义材质路径缓存
std::mutex texturePathCacheMutex;
std::unordered_map<std::string, TextureDimension> textureDimensionCache; // 定义材质尺寸缓存
std::mutex textureDimensionMutex;

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
    std::string foundCacheKey;

    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        // 使用快速查找索引(O(1))
        std::string indexKey = std::string("textures:") + namespaceName + ":" + blockId;
        auto idxIt = GlobalCache::textureIndex.find(indexKey);
        if (idxIt != GlobalCache::textureIndex.end()) {
            const std::string& cacheKey = idxIt->second;
            auto textureIt = GlobalCache::textures.find(cacheKey);
            if (textureIt != GlobalCache::textures.end()) {
                textureData = textureIt->second;
                foundCacheKey = cacheKey;

                if (GetPNGDimensions(textureData, width, height)) {
                    std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                    textureDimensionCache[cacheKey] = TextureDimension(width, height);
                }

                auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
                if (mcmetaIt != GlobalCache::mcmetaCache.end()) {
                    mcmetaData = mcmetaIt->second;
                    if (mcmetaData.contains("animation")) {
                        isDynamic = true;
                    }
                }
            }
        }

        if (textureData.empty()) {
            // 回退: 线性扫描
            for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
                const std::string& modId = GlobalCache::jarOrder[i];
                std::string cacheKey = modId + ":" + namespaceName + ":" + blockId;
                auto textureIt = GlobalCache::textures.find(cacheKey);
                if (textureIt != GlobalCache::textures.end()) {
                    textureData = textureIt->second;
                    foundCacheKey = cacheKey;

                    if (GetPNGDimensions(textureData, width, height)) {
                        std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                        textureDimensionCache[cacheKey] = TextureDimension(width, height);
                    }

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
        }

        if (textureData.empty()) {
            std::cerr << "Texture not found: " << namespaceName << ":" << blockId << std::endl;
            return false;
        }

        // PNG 与 .mcmeta 可能分属不同 jar/资源包(pack 只改贴图、mcmeta 仍用原版)。
        // 上面只查了与 PNG 同一 cacheKey 的 mcmeta；这里按路径再取一份,保证写出 .mcmeta
        // 供 Crafter 动态纹理节点使用。
        if (mcmetaData.empty()) {
            std::string metaIndexKey = std::string("mcmetas:") + namespaceName + ":" + blockId;
            auto metaIdx = GlobalCache::mcmetaIndex.find(metaIndexKey);
            if (metaIdx != GlobalCache::mcmetaIndex.end()) {
                auto mcmetaIt = GlobalCache::mcmetaCache.find(metaIdx->second);
                if (mcmetaIt != GlobalCache::mcmetaCache.end()) {
                    mcmetaData = mcmetaIt->second;
                    if (mcmetaData.contains("animation")) {
                        isDynamic = true;
                    }
                }
            }
        }
    }

    // 处理保存路径
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring ws(buffer);
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string exePath;
    if (len > 0) { exePath.resize(len - 1); WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &exePath[0], len, nullptr, nullptr); }
    size_t pos = exePath.find_last_of("\\/");
    std::string exeDir = exePath.substr(0, pos);

    if (savePath.empty()) {
        savePath = JoinPath(exeDir, "textures");
    }
    else {
        savePath = JoinPath(exeDir, savePath);
    }

    // 创建保存目录（用 W 版支持中文路径）
    std::wstring wSavePath = string_to_wstring(savePath);
    if (GetFileAttributesW(wSavePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CreateDirectoryW(wSavePath.c_str(), NULL);
    }

    // 提取文件名(不含路径)
    size_t lastSlashPos = blockId.find_last_of("/\\");
    std::string fileName = (lastSlashPos == std::string::npos) ? blockId : blockId.substr(lastSlashPos + 1);

    // 创建 namespace 目录
    std::wstring wNsDir = string_to_wstring(JoinPath(savePath, namespaceName));
    CreateDirectoryW(wNsDir.c_str(), NULL);

    // 递归创建子目录
    std::string pathPart = blockId.substr(0, lastSlashPos);
    std::wstring wCurrentPath = wNsDir;
    size_t start = 0;
    size_t end;
    while ((end = pathPart.find('/', start)) != std::string::npos) {
        std::string dir = pathPart.substr(start, end - start);
        wCurrentPath += PathSepWStr() + string_to_wstring(dir);
        CreateDirectoryW(wCurrentPath.c_str(), NULL);
        start = end + 1;
    }
    std::wstring wFinalDir = wCurrentPath + PathSepWStr() + string_to_wstring(pathPart.substr(start));
    CreateDirectoryW(wFinalDir.c_str(), NULL);

    // 保存主 PNG 文件
    std::string filePath = JoinPath(wstring_to_string(wFinalDir), fileName + ".png");
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

            // 使用快速查找索引 + 回退线性扫描
            {
                std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
                std::string pbrIndexKey = std::string("textures:") + namespaceName + ":" + blockId + suffix;
                auto pbrIdxIt = GlobalCache::textureIndex.find(pbrIndexKey);
                if (pbrIdxIt != GlobalCache::textureIndex.end()) {
                    const std::string& pbrCacheKey = pbrIdxIt->second;
                    auto pbrTextureIt = GlobalCache::textures.find(pbrCacheKey);
                    if (pbrTextureIt != GlobalCache::textures.end()) {
                        pbrTextureData = pbrTextureIt->second;

                        if (GetPNGDimensions(pbrTextureData, pbrWidth, pbrHeight)) {
                            std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                            textureDimensionCache[pbrCacheKey] = TextureDimension(pbrWidth, pbrHeight);
                        }

                        auto pbrMcmetaIt = GlobalCache::mcmetaCache.find(pbrCacheKey);
                        if (pbrMcmetaIt != GlobalCache::mcmetaCache.end()) {
                            pbrMcmetaData = pbrMcmetaIt->second;
                        }
                    }
                }

                if (pbrTextureData.empty()) {
                    for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
                        const std::string& modId = GlobalCache::jarOrder[i];
                        std::string pbrCacheKey = modId + ":" + namespaceName + ":" + blockId + suffix;
                        auto pbrTextureIt = GlobalCache::textures.find(pbrCacheKey);
                        if (pbrTextureIt != GlobalCache::textures.end()) {
                            pbrTextureData = pbrTextureIt->second;

                            if (GetPNGDimensions(pbrTextureData, pbrWidth, pbrHeight)) {
                                std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
                                textureDimensionCache[pbrCacheKey] = TextureDimension(pbrWidth, pbrHeight);
                            }

                            auto pbrMcmetaIt = GlobalCache::mcmetaCache.find(pbrCacheKey);
                            if (pbrMcmetaIt != GlobalCache::mcmetaCache.end()) {
                                pbrMcmetaData = pbrMcmetaIt->second;
                            }
                            break;
                        }
                    }
                }
            }

            if (!pbrTextureData.empty()) {
                // 保存 PBR 贴图
                std::string pbrFilePath = JoinPath(wstring_to_string(wFinalDir), fileName + suffix + ".png");
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

// 简化ParseMcmetaFile函数，直接使用图片宽高比
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType, float& outAspectRatio) {
    // 默认为普通材质，默认长宽比为1.0
    outType = NORMAL;
    outAspectRatio = 1.0f;
    
    // 首先尝试从尺寸缓存中获取图片实际尺寸
    {
        std::lock_guard<std::mutex> lock(textureDimensionMutex);
        auto dimIt = textureDimensionCache.find(cacheKey);
        if (dimIt != textureDimensionCache.end()) {
            // 直接使用图片的高宽比作为UV缩放因子
            int width = dimIt->second.width;
            int height = dimIt->second.height;
            
            if (width > 0 && height > 0) {
                // 如果高度是宽度的整数倍，可能是垂直排列的动画帧
                if (height > width && height % width == 0) {
                    // 直接计算帧数
                    int frames = height / width;
                    outAspectRatio = static_cast<float>(frames);
                } else {
                    // 非标准动画纹理，使用实际高宽比
                    outAspectRatio = static_cast<float>(height) / width;
                }
            }
        }
    }
    
    // 然后检查mcmeta数据以确定材质类型
    auto mcmetaIt = GlobalCache::mcmetaCache.find(cacheKey);
    if (mcmetaIt == GlobalCache::mcmetaCache.end() || mcmetaIt->second.empty()) {
        // 没有找到.mcmeta数据,视为普通材质
        return false;
    }
    
    const nlohmann::json& mcmetaData = mcmetaIt->second;
    
    // 检查是否为动态材质
    if (mcmetaData.contains("animation")) {
        outType = ANIMATED;
        // 注意：我们已经从图片尺寸计算了帧数/比例，不需要再从mcmeta中提取
        return true;
    }
    
    // 检查是否为CTM材质
    if (mcmetaData.contains("ctm")) {
        outType = CTM;
        return true;
    }

    // Fusion/ConnectedTextures 风格的连接材质：
    // {
    //   "fusion": { "type": "connecting", "layout": "full" }
    // }
    if (mcmetaData.contains("fusion") && mcmetaData["fusion"].is_object()) {
        const auto& fusionData = mcmetaData["fusion"];
        if (fusionData.value("type", "") == "connecting") {
            outType = CTM;
            return true;
        }
    }
    
    // 其他类型的.mcmeta文件,保持为普通材质
    return true;
}

// 向后兼容的原始函数版本
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType) {
    float dummyAspectRatio;
    return ParseMcmetaFile(cacheKey, outType, dummyAspectRatio);
}

// 按与 SaveTextureToFile 相同的优先级,取"最终生效"贴图的尺寸来算 UV 帧数缩放。
// mcmeta 可能来自原版 jar,而 PNG 被资源包覆盖(或反过来),两者 cacheKey 不同:
// 若只用 mcmeta 的 key 查 textureDimensionCache 会 miss,aspectRatio 停在 1,
// 于是 ANIMATED 材质的 V 不裁到第 0 帧 → 灯笼等 16xN 条带贴图 UV 整体偏到后续帧(框采到发光区)。
static float AspectRatioFromTextureDimensions(int width, int height) {
    if (width <= 0 || height <= 0) return 1.0f;
    if (height > width && height % width == 0) {
        return static_cast<float>(height / width);
    }
    return static_cast<float>(height) / static_cast<float>(width);
}

static float LookupTextureAspectRatio(const std::string& namespaceName, const std::string& texturePath) {
    // 1) 已写出的贴图尺寸(SaveTextureToFile 写入, key 是实际命中的 cacheKey)
    {
        std::string indexKey = std::string("textures:") + namespaceName + ":" + texturePath;
        std::string cacheKey;
        {
            std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
            auto idxIt = GlobalCache::textureIndex.find(indexKey);
            if (idxIt != GlobalCache::textureIndex.end()) {
                cacheKey = idxIt->second;
            }
        }
        if (!cacheKey.empty()) {
            std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
            auto dimIt = textureDimensionCache.find(cacheKey);
            if (dimIt != textureDimensionCache.end()) {
                return AspectRatioFromTextureDimensions(dimIt->second.width, dimIt->second.height);
            }
        }
    }
    // 2) 退回: 任意 jar 下同路径的尺寸缓存
    {
        std::lock_guard<std::mutex> dimLock(textureDimensionMutex);
        const std::string suffix = ":" + namespaceName + ":" + texturePath;
        for (const auto& entry : textureDimensionCache) {
            if (entry.first.size() >= suffix.size() &&
                entry.first.compare(entry.first.size() - suffix.size(), suffix.size(), suffix) == 0) {
                float ar = AspectRatioFromTextureDimensions(entry.second.width, entry.second.height);
                if (ar > 1.0f) return ar;
            }
        }
    }
    return 1.0f;
}

// 检测材质类型
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath, float& outAspectRatio) {
    MaterialType type = NORMAL;
    // 先按实际贴图尺寸定 UV 缩放,再查 mcmeta 定类型。两者解耦,避免 pack PNG / 原版 mcmeta
    // 分属不同 cacheKey 时 aspectRatio 被丢成 1。
    outAspectRatio = LookupTextureAspectRatio(namespaceName, texturePath);
    
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        // 使用快速查找索引(O(1))
        std::string indexKey = std::string("mcmetas:") + namespaceName + ":" + texturePath;
        auto it = GlobalCache::mcmetaIndex.find(indexKey);
        if (it != GlobalCache::mcmetaIndex.end()) {
            MaterialType parsedType = NORMAL;
            float ignoredAspect = 1.0f;
            if (ParseMcmetaFile(it->second, parsedType, ignoredAspect)) {
                type = parsedType;
                // 若尺寸缓存尚空(极少),退回 mcmeta 路径算出的比例
                if (outAspectRatio <= 1.0f && ignoredAspect > 1.0f) {
                    outAspectRatio = ignoredAspect;
                }
                return type;
            }
        }

        // 回退: 线性扫描
        for (size_t i = 0; i < GlobalCache::jarOrder.size(); ++i) {
            const std::string& modId = GlobalCache::jarOrder[i];
            std::string cacheKey = modId + ":" + namespaceName + ":" + texturePath;
            MaterialType parsedType = NORMAL;
            float ignoredAspect = 1.0f;
            if (ParseMcmetaFile(cacheKey, parsedType, ignoredAspect)) {
                type = parsedType;
                if (outAspectRatio <= 1.0f && ignoredAspect > 1.0f) {
                    outAspectRatio = ignoredAspect;
                }
                break;
            }
        }
    }
    
    return type;
}

// 原始函数的重载版本，保持向后兼容性
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath) {
    float aspectRatio;
    return DetectMaterialType(namespaceName, texturePath, aspectRatio);
}

// --------------------------------------------------------------------------------
// 纹理像素解码 / 不透明判定（运行时遮挡表使用）
// --------------------------------------------------------------------------------

// 程序所在目录（供 PNG 磁盘回退使用）
static std::string GetTextureExeDir() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    std::wstring ws(buffer);
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string exePath;
    if (len > 0) {
        exePath.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &exePath[0], len, nullptr, nullptr);
    }
    size_t pos = exePath.find_last_of("\\/");
    return (pos == std::string::npos) ? std::string() : exePath.substr(0, pos);
}

bool LoadTexturePixels(const std::string& namespaceName, const std::string& texturePath,
    std::vector<unsigned char>& outPixels, int& outWidth, int& outHeight) {
    outPixels.clear();
    outWidth = 0;
    outHeight = 0;

    std::vector<unsigned char> pngData;
    {
        std::shared_lock<std::shared_mutex> lock(GlobalCache::cacheMutex);
        auto indexIt = GlobalCache::textureIndex.find("textures:" + namespaceName + ":" + texturePath);
        if (indexIt != GlobalCache::textureIndex.end()) {
            auto textureIt = GlobalCache::textures.find(indexIt->second);
            if (textureIt != GlobalCache::textures.end()) {
                pngData = textureIt->second;
            }
        }
    }

    unsigned char* pixels = nullptr;
    int channels = 0;
    if (!pngData.empty()) {
        pixels = stbi_load_from_memory(
            pngData.data(), static_cast<int>(pngData.size()), &outWidth, &outHeight, &channels, 4);
    }

    // 回退：读取程序目录下已导出的 PNG（CTM 合成图、内存缓存未命中时）
    if (!pixels) {
        const std::string exeDir = GetTextureExeDir();
        if (!exeDir.empty()) {
            std::string texFs = texturePath;
            for (char& ch : texFs) { if (ch == '/') ch = PathSep(); }
            const std::string filePath =
                JoinPath(exeDir, "textures", namespaceName, texFs + ".png");
            pixels = stbi_load(filePath.c_str(), &outWidth, &outHeight, &channels, 4);
        }
    }

    if (!pixels || outWidth <= 0 || outHeight <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    outPixels.assign(pixels, pixels + static_cast<size_t>(outWidth) * outHeight * 4);
    stbi_image_free(pixels);
    return true;
}

bool IsTextureFullyOpaque(const std::string& namespaceName, const std::string& texturePath) {
    static std::mutex opaqueCacheMutex;
    static std::unordered_map<std::string, bool> opaqueCache;

    const std::string key = namespaceName + ":" + texturePath;
    {
        std::lock_guard<std::mutex> lock(opaqueCacheMutex);
        auto it = opaqueCache.find(key);
        if (it != opaqueCache.end()) return it->second;
    }

    bool opaque = false;
    std::vector<unsigned char> pixels;
    int w = 0, h = 0;
    if (LoadTexturePixels(namespaceName, texturePath, pixels, w, h)) {
        opaque = true;
        const size_t count = static_cast<size_t>(w) * h;
        for (size_t i = 0; i < count; ++i) {
            if (pixels[i * 4 + 3] < 250) {
                opaque = false;
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(opaqueCacheMutex);
        opaqueCache[key] = opaque;
    }
    return opaque;
}