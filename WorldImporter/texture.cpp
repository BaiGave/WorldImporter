#include "texture.h"
#include "fileutils.h"
#include <Windows.h>   
#include <iostream>
#include <fstream>

// 获取目标路径对应的纹理文件的二进制内容
std::vector<unsigned char> GetTextureData(const std::string& namespaceName, const std::string& blockId) {
    // 获取当前整合包的版本
    std::string currentVersion = currentSelectedGameVersion;

    // 在 modListCache 中查找对应的模组列表
    if (modListCache.find(currentVersion) == modListCache.end()) {
        std::cerr << "Mod list for version " << currentVersion << " not found!" << std::endl;
        return {};  // 返回空的二进制数据
    }


    // 获取该版本的游戏文件夹路径
    std::string gameFolderPath = config.versionConfigs[currentVersion].gameFolderPath;


    // 遍历 modListCache 中该版本的模组列表，查找对应的纹理文件
    for (const auto& folderData : modListCache[currentVersion]) {
        // 获取当前模组的命名空间
        std::string modNamespace = folderData.namespaceName;

       

        // 如果是 Vanilla (minecraft)
        if (modNamespace == "vanilla") {
            // 从 VersionCache 中获取 minecraft 的路径
            if (VersionCache.find(currentVersion) != VersionCache.end()) {
                for (const auto& folderData : VersionCache[currentVersion]) {
                    // 获取 minecraft 文件夹路径
                    std::wstring minecraftPath = string_to_wstring(folderData.path);
                    JarReader minecraftReader(minecraftPath);

                    // 构造纹理文件的路径
                    std::string textureFilePath = "assets/" + namespaceName + "/textures/" + blockId + ".png";

                    // 获取二进制内容
                    std::vector<unsigned char> textureData = minecraftReader.getBinaryFileContent(textureFilePath);

                    // 如果找到了文件，返回二进制数据
                    if (!textureData.empty()) {
                        return textureData;
                    }
                }
            }
            else {
                std::cerr << "Minecraft version not found in VersionCache!" << std::endl;
                return {};  // 返回空的二进制数据
            }
        }
        // 如果是 ResourcePack
        else if (modNamespace == "resourcePack") {
            // 遍历 resourcePacksCache 查找对应的 resourcePack 文件
            for (const auto& folderData : resourcePacksCache[currentVersion]) {
                std::wstring resourcePackPath = string_to_wstring(folderData.path);
                JarReader resourcePackReader(resourcePackPath);

                // 构造纹理文件的路径
                std::string textureFilePath = "assets/" + namespaceName + "/textures/" + blockId + ".png";

                // 获取二进制内容
                std::vector<unsigned char> textureData = resourcePackReader.getBinaryFileContent(textureFilePath);

                // 如果找到了文件，返回二进制数据
                if (!textureData.empty()) {
                    return textureData;
                }
            }
        }
        // 如果是普通 Mod 模组，继续按原逻辑处理
        else {
            // 构造纹理文件的路径
            std::string textureFilePath = "assets/" + namespaceName + "/textures/" + blockId + ".png";
            std::wstring jarFilePath = string_to_wstring(folderData.path);
            JarReader jarReader(jarFilePath);

            // 获取二进制内容
            std::vector<unsigned char> textureData = jarReader.getBinaryFileContent(textureFilePath);

            // 如果找到了文件，返回二进制数据
            if (!textureData.empty()) {
                return textureData;
            }
        }
    }

    // 如果没有找到文件，返回空二进制数据
    std::cerr << "Texture file not found for blockId: " << blockId << std::endl;
    return {};  // 返回空的二进制数据
}



bool SaveTextureToFile(const std::string& namespaceName, const std::string& blockId, std::string& savePath) {
    // 获取纹理数据
    std::vector<unsigned char> textureData = GetTextureData(namespaceName, blockId);

    // 检查是否找到了纹理数据
    if (!textureData.empty()) {
        std::cout << "Texture data successfully retrieved for " << blockId << std::endl;

        // 获取当前工作目录（即 exe 所在的目录）
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string exePath = std::string(buffer);

        // 获取 exe 所在目录
        size_t pos = exePath.find_last_of("\\/");
        std::string exeDir = exePath.substr(0, pos);

        // 如果传入了 savePath, 则使用 savePath 作为保存目录，否则默认使用当前 exe 目录
        if (savePath.empty()) {
            savePath = exeDir + "\\textures";  // 默认保存到 exe 目录下的 textures 文件夹
        }
        else {
            savePath = exeDir + "\\" + savePath;  // 使用提供的 savePath 路径
        }

        // 创建保存目录（如果不存在）
        if (GetFileAttributesA(savePath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            // 文件夹不存在，创建它
            if (!CreateDirectoryA(savePath.c_str(), NULL)) {
                std::cerr << "Failed to create directory: " << savePath << std::endl;
                return false;
            }
        }

        // 处理 blockId，去掉路径部分，保留最后的文件名
        size_t lastSlashPos = blockId.find_last_of("/\\");
        std::string fileName = (lastSlashPos == std::string::npos) ? blockId : blockId.substr(lastSlashPos + 1);

        // 构建保存路径，使用处理后的 blockId 作为文件名
        std::string filePath = savePath + "\\" + fileName + ".png";
        std::ofstream outputFile(filePath, std::ios::binary);

        //返回savePath，作为value
        savePath = filePath;

        if (outputFile.is_open()) {
            outputFile.write(reinterpret_cast<const char*>(textureData.data()), textureData.size());
            std::cout << "Texture saved as '" << filePath << "'" << std::endl;
        }
        else {
            std::cerr << "Failed to open output file!" << std::endl;
            return false;
        }
    }
    else {
        std::cerr << "Failed to retrieve texture for " << blockId << std::endl;
        return false;
    }

    return true;
}