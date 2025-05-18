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

// 材质注册方法
void RegisterTexture(const std::string& namespaceName, const std::string& pathPart, const std::string& savePath);

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath);

// 新增:检测材质类型
MaterialType DetectMaterialType(const std::string& namespaceName, const std::string& texturePath);

// 新增:从缓存中读取.mcmeta数据并解析
bool ParseMcmetaFile(const std::string& cacheKey, MaterialType& outType);
#endif // TEXTURE_H
