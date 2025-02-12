#ifndef TEXTURE_H
#define TEXTURE_H

#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>
#include "config.h"
#include "version.h"
#include "JarReader.h"
#include "GlobalCache.h"


// 函数声明
std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId);

bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath);

void PrintTextureCache(const std::unordered_map<std::string, std::vector<unsigned char>>& textureCache);
#endif // TEXTURE_H
