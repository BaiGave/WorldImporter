#ifndef TEXTURE_H
#define TEXTURE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include "config.h"
#include "JarReader.h"
#include "GlobalCache.h"


// 纹理缓存和互斥锁
extern std::unordered_map<std::string, std::string> texturePathCache; 
static std::mutex texturePathCacheMutex;

// 材质注册方法
void RegisterTexture(const std::string& namespaceName, const std::string& pathPart, const std::string& savePath);

//已弃用
std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId);

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath);

void PrintTextureCache(const std::unordered_map<std::string, std::vector<unsigned char>>& textureCache);
#endif // TEXTURE_H
