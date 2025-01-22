#include "JarReader.h"
#include <iostream>
#include <zip.h>
#include <vector>
#include <sstream>
#include <cstring>
#include <nlohmann/json.hpp>  

#ifdef _WIN32
#include <windows.h>
#endif

// 静态变量初始化移除缓存部分

std::string JarReader::convertWStrToStr(const std::wstring& wstr) {
#ifdef _WIN32
    int buffer_size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string str(buffer_size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], buffer_size, nullptr, nullptr);
    return str;
#else
    return std::string(wstr.begin(), wstr.end());
#endif
}

JarReader::JarReader(const std::wstring& jarFilePath)
    : jarFilePath(jarFilePath), zipFile(nullptr), modType(ModType::Unknown) {
    // 在 Windows 上，需要将宽字符路径转换为 UTF-8 路径
    std::string utf8Path = convertWStrToStr(jarFilePath);

    // 打开 .jar 文件（本质上是 .zip 文件）
    int error = 0;
#ifdef _WIN32
    zipFile = zip_open(utf8Path.c_str(), 0, &error);  // 使用 UTF-8 路径打开
#else
    zipFile = zip_open(jarFilePath.c_str(), 0, &error); // Linux/macOS 直接使用宽字符路径
#endif
    if (!zipFile) {
        std::cerr << "Failed to open .jar file: " << utf8Path << std::endl;
        return;
    }

    // 检查是否为原版
    if (isVanilla()) {
        modType = ModType::Vanilla;
    }
    // 检查是否为Fabric
    else if (isFabric()) {
        modType = ModType::Mod;
    }
    // 检查是否为Forge
    else if (isForge()) {
        modType = ModType::Mod;
    }
    // 检查是否为NeoForge
    else if (isNeoForge()) {
        modType = ModType::Mod;
    }

    // 获取命名空间
    modNamespace = getNamespaceForModType(modType);
}

JarReader::~JarReader() {
    // 关闭 .jar 文件
    if (zipFile) {
        zip_close(zipFile);
    }
}

std::string JarReader::getNamespaceForModType(ModType type) {
    switch (type) {
    case ModType::Vanilla:
        return "minecraft";
    case ModType::Mod: {
        std::string forgeModId = getForgeModId();
        if (!forgeModId.empty()) {
            return forgeModId;
        }
        return "";
    }
    default:
        return "";
    }
}

std::string JarReader::getFileContent(const std::string& filePathInJar) {
    if (!zipFile) {
        std::cerr << "Zip file is not open." << std::endl;
        return "";
    }

    // 查找文件在 .jar 文件中的索引
    zip_file_t* fileInJar = zip_fopen(zipFile, filePathInJar.c_str(), 0);
    if (!fileInJar) {
        return "";
    }

    // 获取文件的大小
    zip_stat_t fileStat;
    zip_stat(zipFile, filePathInJar.c_str(), 0, &fileStat);

    // 读取文件内容
    std::string fileContent;
    fileContent.resize(fileStat.size);
    zip_fread(fileInJar, &fileContent[0], fileStat.size);

    // 关闭文件
    zip_fclose(fileInJar);

    return fileContent;
}

std::vector<unsigned char> JarReader::getBinaryFileContent(const std::string& filePathInJar) {
    std::vector<unsigned char> fileContent;

    if (!zipFile) {
        std::cerr << "Zip file is not open." << std::endl;
        return fileContent;
    }

    // 查找文件在 .jar 文件中的索引
    zip_file_t* fileInJar = zip_fopen(zipFile, filePathInJar.c_str(), 0);
    if (!fileInJar) {
        //std::cerr << "Failed to open file in .jar: " << filePathInJar << std::endl;
        return fileContent;
    }

    // 获取文件的大小
    zip_stat_t fileStat;
    zip_stat(zipFile, filePathInJar.c_str(), 0, &fileStat);

    // 读取文件内容
    fileContent.resize(fileStat.size);
    zip_fread(fileInJar, fileContent.data(), fileStat.size);

    // 关闭文件
    zip_fclose(fileInJar);

    return fileContent;
}

std::vector<std::string> JarReader::getFilesInSubDirectory(const std::string& subDir) {
    std::vector<std::string> filesInSubDir;

    if (!zipFile) {
        std::cerr << "Zip file is not open." << std::endl;
        return filesInSubDir;
    }

    // 获取文件条目数量
    int numFiles = zip_get_num_entries(zipFile, 0);
    for (int i = 0; i < numFiles; ++i) {
        const char* fileName = zip_get_name(zipFile, i, 0);
        if (fileName && std::string(fileName).find(subDir) == 0) {
            filesInSubDir.push_back(fileName);
        }
    }

    return filesInSubDir;
}

bool JarReader::isVanilla() {
    return !getFileContent("version.json").empty();
}

bool JarReader::isFabric() {
    return !getFileContent("fabric.mod.json").empty();
}

bool JarReader::isForge() {
    return !getFileContent("META-INF/mods.toml").empty();
}

bool JarReader::isNeoForge() {
    return !getFileContent("META-INF/neoforge.mods.toml").empty();
}

// 获取原版 Minecraft 版本 ID
std::string JarReader::getVanillaVersionId() {
    if (modType != ModType::Vanilla) {
        return "";
    }

    std::string versionJsonContent = getFileContent("version.json");
    nlohmann::json json = nlohmann::json::parse(versionJsonContent);
    return json["id"].get<std::string>();
}

std::string JarReader::getFabricModId() {
    if (modType != ModType::Mod) {
        return "";
    }

    std::string modJsonContent = getFileContent("fabric.mod.json");
    nlohmann::json json = nlohmann::json::parse(modJsonContent);
    return json["id"].get<std::string>();
}

std::string JarReader::getForgeModId() {
    if (modType != ModType::Mod) {
        return "";
    }

    std::string modsTomlContent = getFileContent("META-INF/mods.toml");
    std::string modId = extractModId(modsTomlContent);
    return modId;
}

std::string JarReader::getNeoForgeModId() {
    if (modType != ModType::Mod) {
        return "";
    }

    std::string neoforgeTomlContent = getFileContent("META-INF/neoforge.mods.toml");
    std::string modId = extractModId(neoforgeTomlContent);
    return modId;
}

std::string JarReader::extractModId(const std::string& content) {
    // 解析 .toml 文件，提取 modId
    std::string modId;

    // 清理 content，去除多余的空格和非打印字符
    std::string cleanedContent = cleanUpContent(content);
    size_t startPos = cleanedContent.find("modId=\"");
    if (startPos != std::string::npos) {
        // 从 "modId=\"" 后开始提取，跳过 7 个字符（modId="）
        size_t endPos = cleanedContent.find("\"", startPos + 7); // 查找结束的引号位置
        if (endPos != std::string::npos) {
            modId = cleanedContent.substr(startPos + 7, endPos - (startPos + 7)); // 提取 modId 字符串
        }
    }

    // 如果没有找到 modId，则返回空字符串
    return modId;
}

std::string JarReader::cleanUpContent(const std::string& content) {
    std::string cleaned;
    bool inQuotes = false;

    for (char c : content) {
        // 跳过空格，但保留换行符
        if (std::isspace(c) && c != '\n' && !inQuotes) {
            continue; // 跳过空格，除非在引号内
        }

        // 处理引号内的内容，保留其中的所有字符
        if (c == '\"') {
            inQuotes = !inQuotes; // 切换在引号内外
        }

        // 保留可打印字符
        if (std::isprint(c) || inQuotes) {
            cleaned.push_back(c);
        }
    }

    return cleaned;
}
