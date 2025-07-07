#ifndef TEXTURE_H
#define TEXTURE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"
// 添加材质类型枚举
enum MaterialType {
    NORMAL,      // 普通材质
    ANIMATED,    // 动态材质
    CTM          // 连接纹理材质
};

// 纹理缓存和互斥锁
extern std::unordered_map<std::string, std::string> texturePathCache; 
static std::mutex texturePathCacheMutex;

// 新增：纹理尺寸缓存（保存图片的宽高比）
struct TextureDimension {
    int width;
    int height;
    float aspectRatio; // 高/宽比例
    
    TextureDimension() : width(0), height(0), aspectRatio(1.0f) {}
    TextureDimension(int w, int h) : width(w), height(h), aspectRatio(h > 0 && w > 0 ? static_cast<float>(h) / w : 1.0f) {}
};
extern std::unordered_map<std::string, TextureDimension> textureDimensionCache;
static std::mutex textureDimensionMutex;

// 材质注册方法
void RegisterTexture(const std::string& namespaceName, const std::string& pathPart, const std::string& savePath);

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath);

// 从PNG数据中读取图像尺寸
bool GetPNGDimensions(const std::vector<unsigned char>& pngData, int& width, int& height);

// 检测材质类型（修改后，支持获取长宽比）
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath);
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath, float& outAspectRatio);

// 从缓存中读取.mcmeta数据并解析（修改后，支持获取长宽比）
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType);
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType, float& outAspectRatio);

#endif // TEXTURE_H
