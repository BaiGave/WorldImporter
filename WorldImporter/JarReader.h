#ifndef JARREADER_H
#define JARREADER_H

#include <zip.h>
#include <string>
#include <vector>
#include "fileutils.h"
#include <nlohmann/json.hpp>  // 用于解析JSON

#ifdef _WIN32
#include <windows.h>
#endif

class JarReader {
public:
    bool open() {
        if (zipFile) return true;
        int error;
        zipFile = zip_open(wstring_to_string(jarFilePath).c_str(), 0, &error);
        return zipFile != nullptr;
    }

    // 枚举类型，用于表示 mod 的类型
    enum class ModType {
        Unknown,  // 未知类型
        Vanilla,  // 原版 Minecraft
        Mod
    };

    void cacheAllResources(
        std::unordered_map<std::string, std::vector<unsigned char>>& textureCache,
        std::unordered_map<std::string, nlohmann::json>& blockstateCache,
        std::unordered_map<std::string, nlohmann::json>& modelCache);
    // 构造函数，接受 .jar 文件路径
    JarReader(const std::wstring& jarFilePath);

    // 析构函数，关闭 .jar 文件
    ~JarReader();


    // 去除注释
    std::string cleanUpContent(const std::string& content);

    // 获取 .jar 文件中指定路径的文件内容（文本）
    std::string getFileContent(const std::string& filePathInJar);

    // 获取 .jar 文件中指定路径的文件内容（二进制）
    std::vector<unsigned char> getBinaryFileContent(const std::string& filePathInJar);

    void cacheAllBlockstates(std::unordered_map<std::string, nlohmann::json>& cache);

    void cacheAllModels(std::unordered_map<std::string, nlohmann::json>& cache);

    void cacheAllTextures(std::unordered_map<std::string, std::vector<unsigned char>>& textureCache);

    // 获取 .jar 文件中指定子目录下的所有文件
    std::vector<std::string> getFilesInSubDirectory(const std::string& subDir);

    // 获取 mod 的类型
    ModType getModType() const { return modType; }

    // 获取命名空间
    std::string getNamespace() const { return modNamespace; }

    // 获取原版 Minecraft 版本 ID
    std::string getVanillaVersionId();

    // 获取 Fabric 模组的 modId
    std::string getFabricModId();

    // 获取 Forge 模组的 modId
    std::string getForgeModId();

    // 获取 NeoForge 模组的 modId
    std::string getNeoForgeModId();


private:
    // 判断 .jar 是否为原版
    bool isVanilla();

    // 判断 .jar 是否为 Fabric mod
    bool isFabric();

    // 判断 .jar 是否为 Forge mod
    bool isForge();

    // 判断 .jar 是否为 NeoForge mod
    bool isNeoForge();

    // 手动解析 NeoForge 的 modId 和 displayName
    std::string extractModId(const std::string& content);

    // 在 Windows 上，将宽字符路径转换为 UTF-8 路径
    std::string convertWStrToStr(const std::wstring& wstr);


    // 获取对应 modType 的命名空间
    std::string getNamespaceForModType(ModType type);

    // 缓存已读取的命名空间和路径
    static std::vector<std::wstring> cachedPaths;  // 缓存所有已读取的 .jar 文件路径
    static std::vector<std::string> cachedNamespaces;  // 缓存对应的命名空间

    zip_t* zipFile;  // 用于处理 .jar 文件的 zip 对象
    std::wstring jarFilePath;  // .jar 文件的路径
    ModType modType;  // 当前 mod 的类型
    std::string modNamespace; // 当前 mod 的命名空间

};

#endif // JARREADER_H
