#include <fstream>
#include "block.h"  
#include "fileutils.h"
#include <filesystem> 
#include <locale>     // 为了 std::setlocale
#include <codecvt>    // 为了 wstring_convert
#include <regex>      // 为了 DeleteFiles 中的模式匹配
#include <iostream>

// 定义Windows特定函数和常量（仅在Windows平台使用）
#ifdef _WIN32
extern "C" {
    __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int CodePage, unsigned long dwFlags, 
        const wchar_t* lpWideCharStr, int cchWideChar, char* lpMultiByteStr, 
        int cbMultiByte, const char* lpDefaultChar, int* lpUsedDefaultChar);
    __declspec(dllimport) int __stdcall MultiByteToWideChar(unsigned int CodePage, unsigned long dwFlags, 
        const char* lpMultiByteStr, int cbMultiByte, wchar_t* lpWideCharStr, int cchWideChar);
    __declspec(dllimport) unsigned long __stdcall GetLastError();
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
}
// 定义Windows平台特定常量
#define CP_UTF8 65001
#define MAX_PATH 260
#endif

using namespace std;


// 设置全局 locale 为支持中文,支持 UTF-8 编码
void SetGlobalLocale() {
    // 尝试更标准的 UTF-8 locale 设置
    try {
        std::locale::global(std::locale("en_US.UTF-8"));
    }
    catch (const std::runtime_error&) {
        // 回退到之前的设置或者记录一个警告
        std::setlocale(LC_ALL, "zh_CN.UTF-8"); 
    }
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

std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    
    #ifdef _WIN32
    // Windows平台使用Windows API
    // 计算所需的缓冲区大小
    int buffer_size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (buffer_size <= 0) {
        std::cerr << "Error converting wstring to string: " << GetLastError() << std::endl;
        return "";
    }
    
    // 创建输出字符串
    std::string str(buffer_size, 0);
    if (WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], buffer_size, nullptr, nullptr) == 0) {
        std::cerr << "Error executing WideCharToMultiByte: " << GetLastError() << std::endl;
        return "";
    }
    
    // 移除字符串末尾的空字符
    if (!str.empty() && str.back() == 0) {
        str.pop_back();
    }
    #else
    // 非Windows平台使用标准C++
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        return converter.to_bytes(wstr);
    } catch(const std::exception& e) {
        std::cerr << "Error converting wstring to string: " << e.what() << std::endl;
        return "";
    }
    #endif
    
    return str;
}

std::wstring string_to_wstring(const std::string& str) {
    if (str.empty()) return L"";
    
    #ifdef _WIN32
    // Windows平台使用Windows API
    // 计算所需的缓冲区大小
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (size_needed <= 0) return L"";
    
    // 创建输出宽字符串
    std::wstring result(size_needed, 0);
    
    // 执行转换
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &result[0], size_needed);
    #else
    // 非Windows平台使用标准C++
    std::wstring result;
    try {
        std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
        result = converter.from_bytes(str);
    } catch(const std::exception& e) {
        std::cerr << "Error converting string to wstring: " << e.what() << std::endl;
    }
    #endif
    
    return result;
}

void DeleteTexturesFolder() {
    namespace fs = std::filesystem;

    // 获取当前执行文件路径
    fs::path exePath;
    
    try {
        // 尝试获取可执行文件路径（跨平台方式）
        exePath = fs::current_path();
        
        // 在某些平台，可能需要特殊处理来获取可执行文件路径
        #ifdef _WIN32
        char exePathBuffer[MAX_PATH];
        if (GetModuleFileNameA(NULL, exePathBuffer, MAX_PATH) != 0) {
            exePath = fs::path(exePathBuffer).parent_path();
        }
        #endif
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Error getting executable path: " << e.what() << std::endl;
        return;
    }

    fs::path texturesPath = exePath / "textures";
    fs::path biomeTexPath = exePath / "biomeTex";

    std::error_code ec;
    if (fs::exists(texturesPath)) {
        fs::remove_all(texturesPath, ec);
        if (ec) {
            std::cerr << "Error removing textures folder: " << ec.message() << std::endl;
        }
    }

    if (fs::exists(biomeTexPath)) {
        fs::remove_all(biomeTexPath, ec);
        if (ec) {
            std::cerr << "Error removing biomeTex folder: " << ec.message() << std::endl;
        }
    }
}