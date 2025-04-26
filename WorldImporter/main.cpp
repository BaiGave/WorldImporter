#include <iostream>
#include <chrono>
#include <thread>
#include "config.h"
#include "biome.h"
#include "block.h"
#include "PointCloudExporter.h"
#include <fstream> 
#include <latch>       // C++20特性
#include <barrier>     // C++20特性
#include <semaphore>   // C++20特性
#include <concepts>    // C++20特性
#include <ranges>      // C++20特性
#include <span>        // C++20特性
#include <locale> 
#include "JarReader.h"
#include "blockstate.h"
#include "model.h"
#include "texture.h"
#include "fileutils.h"
#include "GlobalCache.h"
#include "RegionModelExporter.h"
#include "include/json.hpp"
#include <windows.h>
#include <string>
#include "hash_utils.h"

Config config;  // 定义全局变量

using namespace std;
using namespace chrono;
// C++20 概念定义示例
// 使用SFINAE实现类型约束
template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

// 使用SFINAE的函数模板示例
template<typename T>
    requires Numeric<T>
T add(T a, T b) {
    return a + b;
}

// 使用C++20 ranges实现目录删除辅助函数
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
                } else {
                    // 解除文件占用并删除文件
                    if (!DeleteFile(filePath.c_str())) {
                        // 如果删除失败，尝试解除占用
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

// 使用C++20 std::latch实现的初始化函数
void init() {
    // 创建一个latch等待3个任务完成
    std::latch initialization_latch(3);
    
    // 使用线程并行初始化
    std::jthread folder_deletion_thread([&]() {
        DeleteTexturesFolder();
        initialization_latch.count_down();
    });
    
    std::jthread config_thread([&]() {
        SetGlobalLocale();
        config = LoadConfig("config\\config.json");
        initialization_latch.count_down();
    });
    
    std::jthread cache_thread([&]() {
        InitializeAllCaches();
        LoadSolidBlocks(config.solidBlocksFile);
        LoadFluidBlocks(config.fluidsFile);
        RegisterFluidTextures();
        InitializeGlobalBlockPalette();
        initialization_latch.count_down();
    });
    
    // 等待所有初始化任务完成
    initialization_latch.wait();
}

void exportPointCloud() {
    // 创建点云导出器实例，指定输出文件
    PointCloudExporter exporter("output.obj", "block_id_mapping.json");

    // 测量执行时间
    auto start_time = high_resolution_clock::now();

    // 导出点云
    exporter.ExportPointCloud(config.minX, config.maxX, config.minY, config.maxY, config.minZ, config.maxZ);

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    cout << "Total time: " << duration.count() << " milliseconds" << endl;
}

// C++20的结构化绑定和范围for循环改进
void ProcessWithModernCpp() {
    // 使用结构化绑定和初始化器
    auto [width, height] = std::pair{640, 480};
    
    // 使用C++20范围
    std::vector<int> values = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    auto even_values = values | std::views::filter([](int n) { return n % 2 == 0; })
                             | std::views::transform([](int n) { return n * n; });
    
    std::cout << "Even squared values: ";
    for (int v : even_values) {
        std::cout << v << ' ';
    }
    std::cout << std::endl;
}

int main() {
    auto start_time = high_resolution_clock::now();
    init();
    
    // 使用C++20的新特性示例
    ProcessWithModernCpp();
    
    if (config.status == 1) {
        // 如果是 1，导出区域内所有方块模型
        RegionModelExporter::ExportModels("region_models");
    }
    else if (config.status == 2) {
        // 如果是 2，执行点云导出逻辑
        exportPointCloud();
    }
    else if (config.status == 0) {
        // 如果是 0，执行整合包所有方块状态导出逻辑
        ProcessAllBlockstateVariants();
    }
    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);
    cout << "Total time: " << duration.count() << " milliseconds" << endl;
    return 0;
}
