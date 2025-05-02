#include "fileutils.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include <locale>
#include <random>
#include "block.h"   // 需要 Block 相关逻辑
#include "blockstate.h" // 需要 ProcessBlockstateJson
using namespace std;

std::vector<std::string> GetParentPaths(const std::string& modelNamespace, const std::string& modelBlockId) {
    std::vector<std::string> parentPaths;
    nlohmann::json currentModelJson = GetModelJson(modelNamespace, modelBlockId);
    while (true) {
        if (currentModelJson.contains("parent")) {
            const std::string parentModelId = currentModelJson["parent"].get<std::string>();
            parentPaths.push_back(parentModelId);

            size_t parentColonPos = parentModelId.find(':');
            std::string parentNamespace = (parentColonPos != std::string::npos) ? parentModelId.substr(0, parentColonPos) : "minecraft";
            std::string parentBlockId = (parentColonPos != std::string::npos) ? parentModelId.substr(parentColonPos + 1) : parentModelId;

            currentModelJson = GetModelJson(parentNamespace, parentBlockId);
        }
        else {
            break;
        }
    }
    return parentPaths;
}

std::vector<std::vector<std::string>> GetAllParentPaths(const std::string& blockFullName) {
    size_t colonPos = blockFullName.find(':');
    std::string namespaceName = (colonPos != std::string::npos) ? blockFullName.substr(0, colonPos) : "minecraft";
    std::string blockId = (colonPos != std::string::npos) ? blockFullName.substr(colonPos + 1) : blockFullName;

    size_t statePos = blockId.find('[');
    std::string baseBlockId = (statePos != std::string::npos) ? blockId.substr(0, statePos) : blockId;

    nlohmann::json blockstateJson = GetBlockstateJson(namespaceName, baseBlockId);
    std::vector<std::vector<std::string>> allParentPaths;

    if (blockstateJson.contains("variants")) {
        std::vector<std::string> modelIds;
        for (const auto& variant : blockstateJson["variants"].items()) {
            if (variant.value().is_array()) {
                for (const auto& item : variant.value()) {
                    if (item.contains("model")) {
                        modelIds.push_back(item["model"].get<std::string>());
                    }
                }
            }
            else {
                if (variant.value().contains("model")) {
                    modelIds.push_back(variant.value()["model"].get<std::string>());
                }
            }
        }

        for (const auto& modelId : modelIds) {
            size_t modelColonPos = modelId.find(':');
            std::string modelNamespace = (modelColonPos != std::string::npos) ? modelId.substr(0, modelColonPos) : "minecraft";
            std::string modelBlockId = (modelColonPos != std::string::npos) ? modelId.substr(modelColonPos + 1) : modelId;

            std::vector<std::string> parentPaths = GetParentPaths(modelNamespace, modelBlockId);
            parentPaths.push_back(modelId);
            allParentPaths.push_back(parentPaths);
        }
    }
    else if (blockstateJson.contains("multipart")) {
        allParentPaths.push_back({ blockFullName });
    }
    else {
        // 其他情况,可能直接返回当前模型的路径
        allParentPaths.push_back({ blockFullName });
    }

    return allParentPaths;
}
std::string ExtractModelName(const std::string& modelId) {
    size_t colonPos = modelId.find(':');
    return (colonPos != std::string::npos) ? modelId.substr(colonPos + 1) : modelId;
}

std::vector<char> ReadFileToMemory(const std::string& directoryPath, int regionX, int regionZ) {
    // 构造区域文件的路径
    std::ostringstream filePathStream;
    filePathStream << directoryPath << "/region/r." << regionX << "." << regionZ << ".mca";
    std::string filePath = filePathStream.str();

    // 打开文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "错误: 打开文件失败！" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 将文件内容读取到文件数据中
    std::vector<char> fileData;
    fileData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (fileData.empty()) {
        std::cerr << "错误: 文件为空或读取失败！" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 返回读取到的文件数据
    return fileData;
}

unsigned CalculateOffset(const vector<char>& fileData, int x, int z) {
    // 直接使用 x 和 z,不需要进行模运算
    unsigned index = 4 * (x + z * 32);

    // 检查是否越界
    if (index + 3 >= fileData.size()) {
        cerr << "错误: 无效的索引或文件大小。" << endl;
        return 0; // 返回 0 表示计算失败
    }

    // 读取字节
    unsigned char byte1 = fileData[index];
    unsigned char byte2 = fileData[index + 1];
    unsigned char byte3 = fileData[index + 2];

    // 根据字节计算偏移位置 (假设按大端字节序)
    unsigned offset = (byte1 * 256 * 256 + byte2 * 256 + byte3) * 4096;

    return offset;
}


unsigned ExtractChunkLength(const vector<char>& fileData, unsigned offset) {
    // 确保以无符号字节的方式进行计算
    unsigned byte1 = (unsigned char)fileData[offset];
    unsigned byte2 = (unsigned char)fileData[offset + 1];
    unsigned byte3 = (unsigned char)fileData[offset + 2];
    unsigned byte4 = (unsigned char)fileData[offset + 3];

    // 计算区块长度
    return (byte1 << 24) | (byte2 << 16) | (byte3 << 8) | byte4;
}

// 导出区块 NBT 数据到指定的文件
bool ExportChunkNBTDataToFile(const vector<char>& data, const string& filePath) {
    // 打开文件进行二进制写入
    ofstream outFile(filePath, ios::binary);
    if (!outFile) {
        cerr << "无法打开文件: " << filePath << endl;
        return false;  // 无法打开文件,返回 false
    }

    // 写入数据到文件
    outFile.write(data.data(), data.size());
    if (!outFile) {
        cerr << "写入文件失败: " << filePath << endl;
        return false;  // 写入失败,返回 false
    }

    outFile.close();  // 关闭文件
    return true;  // 成功保存数据到文件,返回 true
}


void GenerateSolidsJson(const std::string& outputPath, const std::vector<std::string>& targetParentPaths) {
    std::unordered_set<std::string> solidBlocks;

    // 遍历所有已缓存的方块状态
    std::unordered_map<std::string, nlohmann::json> allBlockstates;
    {
        std::lock_guard<std::mutex> lock(GlobalCache::cacheMutex);
        allBlockstates = GlobalCache::blockstates;
    }

    for (const auto& blockstatePair : allBlockstates) {
        const std::string& blockFullName = blockstatePair.first; // 格式如 "minecraft:stone"
        const nlohmann::json& blockstateJson = blockstatePair.second;

        std::vector<std::vector<std::string>> allParentPaths = GetAllParentPaths(blockFullName);

        bool allSolid = true;
        for (const auto& parentPaths : allParentPaths) {
            bool currentSolid = false;
            for (const auto& parentPath : parentPaths) {
                std::string normalizedParent = ExtractModelName(parentPath);
                for (const auto& target : targetParentPaths) {
                    if (normalizedParent.find(target) != std::string::npos) {
                        currentSolid = true;
                        break;
                    }
                }
                if (currentSolid) break;
            }

            if (!currentSolid) {
                allSolid = false;
                break;
            }
        }

        if (allSolid) {
            std::string fullId = blockFullName.substr(0, blockFullName.find('[')); // 移除状态
            solidBlocks.insert(fullId);
        }
    }

    nlohmann::json j;
    j["solid_blocks"] = solidBlocks;
    std::ofstream file(outputPath);
    if (file.is_open()) {
        file << j.dump(4);
        std::cout << "成功生成 solids.json" << std::endl;
    }
    else {
        std::cerr << "错误: 无法写入文件 " << outputPath << std::endl;
    }
}
// 设置全局 locale 为支持中文,支持 UTF-8 编码
void SetGlobalLocale() {
    std::setlocale(LC_ALL, "zh_CN.UTF-8");  // 使用 UTF-8 编码
}

void LoadSolidBlocks(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open solid_blocks file: " + filepath);
    }

    nlohmann::json j;
    file >> j;

    if (j.contains("solid_blocks")) {
        for (auto& block : j["solid_blocks"]) {
            solidBlocks.insert(block.get<std::string>());
        }
    }
    else {
        throw std::runtime_error("solid_blocks file missing 'solid_blocks' array");
    }
}

void LoadFluidBlocks(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to load fluid config: " + filepath);
    }

    nlohmann::json j;
    file >> j;

    fluidDefinitions.clear();

    if (j.contains("fluids")) {
        for (auto& entry : j["fluids"]) {
            // 简写格式处理
            if (entry.is_string()) {
                std::string name = entry.get<std::string>();
                fluidDefinitions[name] = {
                    "block"
                    "",       // 默认无属性
                    "level",  // 默认level属性
                    {},      // 空列表
                    
                };
                continue;
            }

            // 完整对象格式
            FluidInfo info;
            info.level_property = "level"; // 设置默认值
            info.still_texture = "_still";
            info.flow_texture = "_flow";
            info.folder = "block";
            // 解析必填字段
            if (!entry.contains("name")) {
                throw std::runtime_error("Fluid entry missing 'name' field");
            }
            std::string name = entry["name"].get<std::string>();

            // 解析可选字段
            if (entry.contains("property")) {
                info.property = entry["property"].get<std::string>();
            }
            if (entry.contains("folder")) {
                info.folder = entry["folder"].get<std::string>();
            }
            if (entry.contains("flow_texture")) {
                info.flow_texture = entry["flow_texture"].get<std::string>();
            }
            if (entry.contains("still_texture")) {
                info.still_texture = entry["still_texture"].get<std::string>();
            }
            if (entry.contains("level_property")) {
                info.level_property = entry["level_property"].get<std::string>();
            }
            if (entry.contains("liquid_blocks")) {
                for (auto& block : entry["liquid_blocks"]) {
                    info.liquid_blocks.insert(block.get<std::string>());
                }
            }
            fluidDefinitions[name] = info;
        }
    }
    else {
        throw std::runtime_error("Config missing 'fluids' array");
    }
}

void RegisterFluidTextures() {
    for (const auto& entry : fluidDefinitions) {
        const std::string& fluidName = entry.first; // 完整流体名(如"minecraft:water")
        const FluidInfo& info = entry.second;

        // 解析命名空间和基础名称
        size_t colonPos = fluidName.find(':');
        std::string ns = (colonPos != std::string::npos) ?
            fluidName.substr(0, colonPos) : "minecraft";
        std::string baseName = (colonPos != std::string::npos) ?
            fluidName.substr(colonPos + 1) : fluidName;
        // 自动生成默认材质路径(如果未指定)
        std::string stillPath =baseName + info.still_texture;
        std::string flowPath = baseName + info.flow_texture;

        std::string pathPart1 =info.folder + "/" + stillPath;
        std::string pathPart2 = info.folder + "/" + flowPath;

        std::string textureSavePath1 = "textures/" + ns + "/" + pathPart1 + ".png";
        std::string textureSavePath2 = "textures/" + ns + "/" + pathPart2 + ".png";
        // 注册材质(带命名空间)
        std::string Dir = "textures";
        std::string Dir2 = "textures";
        SaveTextureToFile(ns, pathPart1, Dir);
        RegisterTexture(ns, pathPart1, textureSavePath1);
        SaveTextureToFile(ns, pathPart2, Dir2);
        RegisterTexture(ns, pathPart2, textureSavePath2);
    }
}
// 打印字节数据
void printBytes(const std::vector<char>& data) {
    std::cout << "文件字节数据: " << std::endl;
    for (size_t i = 0; i < data.size(); ++i) {
        // 打印每个字节的十六进制表示
        printf("%02X ", static_cast<unsigned char>(data[i]));
        if ((i + 1) % 16 == 0) {
            std::cout << std::endl;
        }
    }
    std::cout << std::endl;
}

// 使用Windows API实现UTF-8转换,替换弃用的codecvt
std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    // 计算所需的缓冲区大小
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return "";
    
    // 创建输出字符串
    std::string result(size_needed, 0);
    
    // 执行转换
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, nullptr, nullptr);
    
    return result;
}

// 使用Windows API实现UTF-8转换,替换弃用的codecvt
std::wstring string_to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    
    // 计算所需的缓冲区大小
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (size_needed <= 0) return L"";
    
    // 创建输出宽字符串
    std::wstring result(size_needed, 0);
    
    // 执行转换
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], size_needed);
    
    return result;
}

// 将std::wstring转换为Windows系统默认的多字节编码(通常为 GBK 或 ANSI)
std::string wstring_to_system_string(const std::wstring& wstr) {
    int size_needed = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), (int)wstr.size(), &str[0], size_needed, nullptr, nullptr);
    return str;
}

// 获取文件夹名(路径中的最后一部分)
std::wstring GetFolderNameFromPath(const std::wstring& folderPath) {
    size_t pos = folderPath.find_last_of(L"\\");
    if (pos != std::wstring::npos) {
        return folderPath.substr(pos + 1);
    }
    return folderPath;
}

void DeleteFiles(const std::wstring& path, const std::wstring& pattern) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFile((path + L"\\" + pattern).c_str(), &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        std::vector<std::wstring> filesToDelete;

        do {
            if (wcscmp(findFileData.cFileName, L".") != 0 && wcscmp(findFileData.cFileName, L"..") != 0) {
                filesToDelete.push_back(path + L"\\" + findFileData.cFileName);
            }
        } while (FindNextFile(hFind, &findFileData) != 0);

        FindClose(hFind);

        // 使用C++20 ranges和视图
        auto deleteView = filesToDelete
            | std::views::filter([](const std::wstring& file) {
            return GetFileAttributes(file.c_str()) != INVALID_FILE_ATTRIBUTES &&
                !(GetFileAttributes(file.c_str()) & FILE_ATTRIBUTE_DIRECTORY);
                });

        for (const auto& file : deleteView) {
            DeleteFile(file.c_str());
        }
    }
}

void DeleteDirectory(const std::wstring& path) {
    WIN32_FIND_DATA findFileData;
    HANDLE hFind = FindFirstFile((path + L"\\*").c_str(), &findFileData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(findFileData.cFileName, L".") != 0 && wcscmp(findFileData.cFileName, L"..") != 0) {
                std::wstring filePath = path + L"\\" + findFileData.cFileName;
                if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    DeleteDirectory(filePath); // 递归删除子目录
                }
                else {
                    // 解除文件占用并删除文件
                    if (!DeleteFile(filePath.c_str())) {
                        // 如果删除失败,尝试解除占用
                        MoveFileEx(filePath.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
                    }
                }
            }
        } while (FindNextFile(hFind, &findFileData) != 0);
        FindClose(hFind);
    }
    // 删除空目录
    RemoveDirectory(path.c_str());
}

void DeleteTexturesFolder() {
    // 获取当前可执行文件所在的目录
    wchar_t cwd[MAX_PATH];
    if (GetModuleFileName(NULL, cwd, MAX_PATH) == 0) {
        return;
    }

    // 提取目录路径
    std::wstring exePath(cwd);
    size_t lastSlash = exePath.find_last_of(L"\\/");
    std::wstring exeDir = exePath.substr(0, lastSlash);

    // 构建textures文件夹的路径
    std::wstring texturesPath = exeDir + L"\\textures";
    std::wstring biomeTexPath = exeDir + L"\\biomeTex";

    // 删除textures文件夹
    if (GetFileAttributes(texturesPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteDirectory(texturesPath);
    }

    // 删除biomeTex文件夹
    if (GetFileAttributes(biomeTexPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DeleteDirectory(biomeTexPath);
    }
}