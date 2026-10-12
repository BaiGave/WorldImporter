#include "ObjExporter.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <locale>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
// 仅在Windows平台需要这些函数声明
extern "C" {
    __declspec(dllimport) unsigned long __stdcall GetLastError();
    __declspec(dllimport) int __stdcall WideCharToMultiByte(unsigned int, unsigned long, const wchar_t*, int, char*, int, const char*, int*);
    __declspec(dllimport) unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
}
#define CP_UTF8 65001
#define MAX_PATH 260
// Windows平台使用sprintf_s
#define safe_sprintf sprintf_s
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
// 非Windows平台使用snprintf
#define safe_sprintf(dst, size, ...) snprintf(dst, size, __VA_ARGS__)
#endif

using namespace std::chrono;


// 跨平台获取可执行文件目录的实现
std::string getExecutableDir() {
    namespace fs = std::filesystem;
    fs::path exePath;
    
    try {
        // 获取可执行文件路径
#ifdef _WIN32
        // Windows平台
        wchar_t buffer[MAX_PATH] = { 0 };
        if (GetModuleFileNameW(nullptr, buffer, MAX_PATH) == 0) {
        }
        std::wstring ws(buffer);
        size_t pos = ws.find_last_of(L"\\/");
        if (pos != std::wstring::npos) ws.resize(pos + 1);
        int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (len > 0) {
            std::string ret(len, 0);
            WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &ret[0], len, nullptr, nullptr);
            return ret.substr(0, len - 1);
        }
        return ".";
#elif defined(__APPLE__)
        // macOS平台
        char buffer[PATH_MAX];
        uint32_t size = sizeof(buffer);
        if (_NSGetExecutablePath(buffer, &size) != 0) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        exePath = fs::canonical(fs::path(buffer));
#else
        // Linux平台
        char buffer[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len == -1) {
            throw std::runtime_error("无法获取可执行文件路径");
        }
        buffer[len] = '\0';
        exePath = fs::path(buffer);
#endif
        // 获取父目录
        return exePath.parent_path().string() + "/";
    }
    catch (const std::exception& e) {
        std::cerr << "获取可执行文件目录失败: " << e.what() << std::endl;
        return fs::current_path().string() + "/"; // 失败时返回当前目录
    }
}

// 辅助函数:计算数值转换为字符串后的长度
template <typename T>
int calculateStringLength(T value, int precision = 6) {
    char buffer[64];
    return snprintf(buffer, sizeof(buffer), "%.*f", precision, static_cast<double>(value));
}

// 快速计算整数的字符串长度(正数)
inline int calculateIntLength(int value) {
    if (value == 0) return 1;
    int length = 0;
    bool is_negative = value < 0;
    value = abs(value); // 取绝对值
    while (value != 0) {
        value /= 10;
        length++;
    }
    if (is_negative) {
        length++; // 负数需要额外的符号位
    }
    return length;
}
// 快速整数转字符串(带符号)的前置声明
inline char* fast_itoa(int value, char* ptr);
// 快速浮点转字符串的前置声明；长度计算直接复用同一实现，
// 避免数学估算在 9.9999996 -> 10.000000 进位时少算 1 字节并写穿堆缓冲区。
inline char* fast_ftoa(float value, char* ptr);

// 计算浮点数实际写出长度（与 fast_ftoa 严格一致）
inline int calculateFloatStringLength(float value) {
    char buffer[128];
    return static_cast<int>(fast_ftoa(value, buffer) - buffer);
}
// 快速整数转字符串(正数版)
inline char* fast_itoa_positive(uint32_t value, char* ptr) {
    char* start = ptr;
    do {
        *ptr++ = '0' + (value % 10);
        value /= 10;
    } while (value > 0);
    std::reverse(start, ptr);
    return ptr;
}

// 快速整数转字符串(带符号)
inline char* fast_itoa(int value, char* ptr) {
    if (value == 0) {
        *ptr++ = '0';
        return ptr;
    }

    bool negative = value < 0;
    if (negative) {
        *ptr++ = '-';
        value = -value;
    }

    // 调用无符号整数的辅助函数
    ptr = fast_itoa_positive(static_cast<uint32_t>(value), ptr);

    return ptr;
}

// 快速浮点转字符串(固定6位小数)
inline char* fast_ftoa(float value, char* ptr) {
    // 非有限值(NaN/Inf):统一写 0,避开 floor()/整数转换的未定义行为
    if (!std::isfinite(value)) {
        *ptr++ = '0';
        return ptr;
    }
    // 超出定点缩放安全范围:走 snprintf 兜底,避免 int64 溢出
    if (std::fabs(value) >= 1.0e12f) {
        int written = snprintf(ptr, 64, "%.6f", static_cast<double>(value));
        return ptr + (written > 0 ? written : 0);
    }
    if (value == floor(value)) {  // 检查是否是整数
        return fast_itoa(static_cast<int>(value), ptr);
    }
    else {
        // 原有浮点数处理逻辑
        const int64_t scale = 1000000;
        bool negative = value < 0;
        if (negative) {
            *ptr++ = '-';
            value = -value;
        }

        int64_t scaled = static_cast<int64_t>(std::round(value * scale));
        int64_t integer_part = scaled / scale;
        int64_t fractional_part = scaled % scale;

        ptr = fast_itoa(static_cast<int>(integer_part), ptr);
        *ptr++ = '.';

        for (int i = 5; i >= 0; --i) {
            ptr[i] = '0' + (fractional_part % 10);
            fractional_part /= 10;
        }
        ptr += 6;

        return ptr;
    }
}

//——————————————导出.obj/.mtl方法—————————————

void createObjFileViaMemoryMapped(const ModelData& data, const std::string& objName, const std::string& mtlFileName = "") {
    std::string exeDir = getExecutableDir();
    std::string objFilePath = exeDir + objName + ".obj";
    std::string mtlFilePath = mtlFileName.empty() ? (objName + ".mtl") : (mtlFileName + ".mtl");

    size_t totalSize = 0;
    // 文件头部分
    totalSize += snprintf(nullptr, 0, "mtllib %s\n", mtlFilePath.c_str());
    // 提取模型名称(取路径最后一段;兼容 / 与 \ 分隔符)
    std::string modelName = objName;
    size_t sepPos = objName.find_last_of("/\\");
    if (sepPos != std::string::npos) {
        modelName = objName.substr(sepPos + 1);
    }
    totalSize += snprintf(nullptr, 0, "o %s\n\n", modelName.c_str());

    // 预计算顶点注释行的长度
    totalSize += snprintf(nullptr, 0, "# Vertices (%zu)\n", data.vertices.size() / 3);

    // 预计算所有浮点数的字符串长度(顶点数据)
    std::vector<int> vertexLengths(data.vertices.size());
    for (size_t i = 0; i < data.vertices.size(); ++i) {
        vertexLengths[i] = calculateFloatStringLength(data.vertices[i]);
    }

    const size_t vertexCount = data.vertices.size() / 3;
#pragma omp parallel for reduction(+:totalSize)
    for (size_t i = 0; i < vertexCount; ++i) {
        const size_t base = i * 3;
        const int lenX = vertexLengths[base];
        const int lenY = vertexLengths[base + 1];
        const int lenZ = vertexLengths[base + 2];
        totalSize += 2 + lenX + 1 + lenY + 1 + lenZ + 1; // "v " + x + " " + y + " " + z + "\n"
    }

    //预计算UV注释行的长度
    totalSize += snprintf(nullptr, 0, "\n# UVs (%zu)\n", data.uvCoordinates.size() / 2);

    // 预计算UV数据长度
    std::vector<int> uvLengths(data.uvCoordinates.size());
    for (size_t i = 0; i < data.uvCoordinates.size(); ++i) {
        uvLengths[i] = calculateFloatStringLength(data.uvCoordinates[i]);
    }

    const size_t uvCount = data.uvCoordinates.size() / 2;
#pragma omp parallel for reduction(+:totalSize)
    for (size_t i = 0; i < uvCount; ++i) {
        const size_t base = i * 2;
        const int lenU = uvLengths[base];
        const int lenV = uvLengths[base + 1];
        totalSize += 3 + lenU + 1 + lenV + 1; // "vt " + u + " " + v + "\n"
    }

    // 面数据分组计算
    std::vector<std::vector<size_t>> materialGroups(data.materials.size());
    const size_t totalFaces = data.faces.size();
    for (size_t faceIdx = 0; faceIdx < totalFaces; ++faceIdx) {
        const int matIndex = data.faces[faceIdx].materialIndex;
        if (matIndex != -1 && matIndex < materialGroups.size()) {
            materialGroups[matIndex].push_back(faceIdx);
        }
    }

    //预计算面注释行的长度
    totalSize += snprintf(nullptr, 0, "\n# Faces (%zu)\n", totalFaces);

    // 预计算材质组内每个材质对应的usemtl行以及各个面的长度
    std::vector<size_t> usemtlLengths(data.materials.size());
#pragma omp parallel for
    for (int matIndex = 0; matIndex < data.materials.size(); ++matIndex) {
        // "usemtl "(7) + name + "\n"(1)
        usemtlLengths[matIndex] = 8 + data.materials[matIndex].name.size();
    }

#pragma omp parallel for reduction(+:totalSize)
    for (int matIndex = 0; matIndex < materialGroups.size(); ++matIndex) {
        const auto& faces = materialGroups[matIndex];
        if (faces.empty()) continue;
        size_t localSize = usemtlLengths[matIndex];
        for (const size_t faceIdx : faces) {
            size_t faceLength = 3; // "f " + '\n'
            const auto& vertexIndices = data.faces[faceIdx].vertexIndices;
            const auto& uvIndices = data.faces[faceIdx].uvIndices;
            for (int i = 0; i < 4; ++i) {
                const int vIdx = vertexIndices[i] + 1;
                const int uvIdx = uvIndices[i] + 1;
                faceLength += calculateIntLength(vIdx) + calculateIntLength(uvIdx) + 2; // 对应 '/' 和空格
            }
            localSize += faceLength;
        }
        totalSize += localSize;
    }

    // 分配缓冲区(+1为安全冗余)
    std::vector<char> buffer(totalSize + 1);
    char* ptr = buffer.data();

    // 开始填充缓冲区
    ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "mtllib %s\n", mtlFilePath.c_str());
    ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "o %s\n\n", modelName.c_str());
    ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "# Vertices (%zu)\n", data.vertices.size() / 3);
    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        memcpy(ptr, "v ", 2);
        ptr += 2;
        ptr = fast_ftoa(data.vertices[i], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.vertices[i + 1], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.vertices[i + 2], ptr);
        *ptr++ = '\n';
    }

    ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "\n# UVs (%zu)\n", data.uvCoordinates.size() / 2);
    for (size_t i = 0; i < data.uvCoordinates.size(); i += 2) {
        memcpy(ptr, "vt ", 3);
        ptr += 3;
        ptr = fast_ftoa(data.uvCoordinates[i], ptr);
        *ptr++ = ' ';
        ptr = fast_ftoa(data.uvCoordinates[i + 1], ptr);
        *ptr++ = '\n';
    }

    ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "\n# Faces (%zu)\n", totalFaces);
    for (size_t matIndex = 0; matIndex < materialGroups.size(); ++matIndex) {
        const auto& faces = materialGroups[matIndex];
        if (faces.empty()) continue;
        ptr += snprintf(ptr, buffer.size() - (ptr - buffer.data()), "usemtl %s\n", data.materials[matIndex].name.c_str());
        for (const size_t faceIdx : faces) {
            memcpy(ptr, "f ", 2);
            ptr += 2;
            for (int i = 0; i < 4; ++i) {
                const int vIdx = data.faces[faceIdx].vertexIndices[i] + 1;
                const int uvIdx = data.faces[faceIdx].uvIndices[i] + 1;
                if (vIdx <= 0 || uvIdx <= 0) {
                    throw std::invalid_argument("Invalid vertex or UV index");
                }
                ptr = fast_itoa(vIdx, ptr);
                *ptr++ = '/';
                ptr = fast_itoa(uvIdx, ptr);
                *ptr++ = ' ';
            }
            *ptr++ = '\n';
        }
    }

    const size_t writtenSize = static_cast<size_t>(ptr - buffer.data());
    if (writtenSize != totalSize) {
        throw std::runtime_error("OBJ缓冲长度计算不一致: estimated=" +
            std::to_string(totalSize) + ", actual=" + std::to_string(writtenSize));
    }

    // 使用跨平台方式写入文件
    try {
#ifdef _WIN32
        // Windows平台使用内存映射文件
        std::ofstream file(objFilePath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("无法创建文件: " + objFilePath);
        }

        // 一次性写入全部数据
        file.write(buffer.data(), totalSize);
        file.close();
#elif defined(__unix__) || defined(__APPLE__)
        // Unix/Linux/macOS平台
        int fd = open(objFilePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            throw std::runtime_error("无法创建文件: " + objFilePath + ", 错误: " + std::to_string(errno));
        }
        
        // 设置文件大小
        if (ftruncate(fd, totalSize) == -1) {
            close(fd);
            throw std::runtime_error("无法设置文件大小: " + objFilePath);
        }
        
        // 映射文件到内存
        void* mappedData = mmap(NULL, totalSize, PROT_WRITE, MAP_SHARED, fd, 0);
        if (mappedData == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("无法映射文件: " + objFilePath);
        }
        
        // 拷贝数据到映射内存
        memcpy(mappedData, buffer.data(), totalSize);
        
        // 同步并释放资源
        msync(mappedData, totalSize, MS_SYNC);
        munmap(mappedData, totalSize);
        close(fd);
#else
        // 其他平台回退到标准文件IO
        std::ofstream file(objFilePath, std::ios::binary | std::ios::trunc);
        if (!file) {
            throw std::runtime_error("无法创建文件: " + objFilePath);
        }
        
        // 一次性写入全部数据
        file.write(buffer.data(), totalSize);
        file.close();
#endif
    }
    catch (const std::exception& e) {
        std::cerr << "写入OBJ文件时出错: " << e.what() << std::endl;
    }
}

// 创建 .obj 文件并写入内容
void createObjFile(const ModelData& data, const std::string& objName, const std::string& mtlFileName = "") {
    std::string exeDir = getExecutableDir();

    std::string objFilePath = exeDir + objName + ".obj";
    std::string mtlFilePath = mtlFileName.empty() ? (objName + ".mtl") : (mtlFileName + ".mtl");

    // 提取模型名称(取路径最后一段;兼容 / 与 \ 分隔符)
    std::string name = objName;
    size_t sepPos = objName.find_last_of("/\\");
    if (sepPos != std::string::npos) {
        name = objName.substr(sepPos + 1);
    }

    // 使用流缓冲区进行拼接,减少IO操作次数
    std::ostringstream oss;
    oss.imbue(std::locale::classic());

    // 写入文件头
    oss << "mtllib " << mtlFilePath << "\n";
    oss << "o " << name << "\n\n";
    // 写入顶点数据(每3个元素一个顶点)
    oss << "# Vertices (" << data.vertices.size() / 3 << ")\n";
    for (size_t i = 0; i < data.vertices.size(); i += 3) {
        oss << "v " << data.vertices[i] << " "
            << data.vertices[i + 1] << " "
            << data.vertices[i + 2] << "\n";
    }
    oss << "\n";

    // 写入UV坐标(每2个元素一个UV)
    oss << "# UVs (" << data.uvCoordinates.size() / 2 << ")\n";
    for (size_t i = 0; i < data.uvCoordinates.size(); i += 2) {
        oss << "vt " << data.uvCoordinates[i] << " "
            << data.uvCoordinates[i + 1] << "\n";
    }
    oss << "\n";

    // 按材质分组面(优化分组算法)
    std::vector<std::vector<size_t>> materialGroups(data.materials.size());
    const size_t totalFaces = data.faces.size();
    for (size_t faceIdx = 0; faceIdx < totalFaces; ++faceIdx) {
        const int matIndex = data.faces[faceIdx].materialIndex;
        if (matIndex != -1 && matIndex < materialGroups.size()) {
            materialGroups[matIndex].push_back(faceIdx);
        }
    }

    // 写入面数据(优化内存访问模式)
    oss << "# Faces (" << totalFaces << ")\n";
    for (size_t matIndex = 0; matIndex < materialGroups.size(); ++matIndex) {
        const auto& faces = materialGroups[matIndex];
        if (faces.empty()) continue;

        oss << "usemtl " << data.materials[matIndex].name << "\n";
        for (const size_t faceIdx : faces) {
            oss << "f ";
            for (int i = 0; i < 4; ++i) {
                const int vIdx = data.faces[faceIdx].vertexIndices[i] + 1;
                const int uvIdx = data.faces[faceIdx].uvIndices[i] + 1;
                oss << vIdx << "/" << uvIdx << " ";
            }
            oss << "\n";
        }
    }

    // 将缓冲区写入文件
    std::ofstream objFile(objFilePath, std::ios::binary); // 使用二进制模式提高写入速度
    if (objFile.is_open()) {
        objFile << oss.str();
        objFile.close();
    }
    else {
        std::cerr << "Error: Failed to create " << objFilePath << "\n";
    }
}

void CreateSharedMtlFile(std::unordered_map<std::string, std::string> uniqueMaterials, const std::string& mtlFileName, const std::unordered_map<std::string, TintResult>& uniqueTints) {
    std::string exeDir = getExecutableDir();
    std::string fullMtlPath = exeDir + mtlFileName + ".mtl";

    std::ofstream mtlFile(fullMtlPath);
    if (mtlFile.is_open()) {
        for (const auto& uM : uniqueMaterials) {
            const std::string& textureName = uM.first;
            std::string texturePath = uM.second;

            mtlFile << "newmtl " << textureName << "\n";

            // 处理材质类型
            if (texturePath == "None") {
                // LIGHT材质处理
                mtlFile << "Ns 200.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ka 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.900000 0.900000 0.900000\n";
                mtlFile << "Ke 0.900000 0.900000 0.900000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 2\n";
            }
            // 缺失纹理不写虚假的 .png 引用，避免 Blender 显示紫色材质。
            else if (texturePath.empty()) {
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            // 处理纯颜色材质(支持流体格式:color#r g b-流体名 和普通格式:color#r g b=)
            else if (texturePath.find("color#") != std::string::npos) {
                std::string colorStr;
                size_t pos = texturePath.find("-");
                size_t pos_deng = texturePath.find("=");
                if (pos != std::string::npos) {
                    // 流体材质格式,提取"-"前面的颜色部分
                    colorStr = texturePath.substr(std::string("color#").size(), pos - std::string("color#").size());
                }
                else if (pos_deng != std::string::npos) {
                    colorStr = texturePath.substr(std::string("color#").size(), pos_deng - std::string("color#").size());
                }
                else {
                    colorStr = "";
                }
                std::istringstream iss(colorStr);
                float r, g, b;
                if (iss >> r >> g >> b) {

                    mtlFile << "Kd " << std::fixed << std::setprecision(config.decimalPlaces)
                        << r << " " << g << " " << b << "\n";
                }
                else {
                    mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                    std::cerr << "Error: Invalid color format in '" << texturePath << "'\n";
                }
                // 共用普通材质参数
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            else {
                // 普通纹理材质处理
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";

                // 处理纹理路径
                if (mtlFileName.find("//") != std::string::npos) {
                    texturePath = "../" + texturePath;
                }
                if (texturePath.find(".png") == std::string::npos) {
                    texturePath += ".png";
                }
                mtlFile << "map_Kd " << texturePath << "\n";
                mtlFile << "map_d " << texturePath << "\n";
            }

            mtlFile << "\n";
        }
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFileName << std::endl;
    }

    CreateTintJsonFile(uniqueTints);
}

namespace {
    float SrgbChannelToLinear(uint32_t channel) {
        float s = static_cast<float>(channel & 0xFF) / 255.0f;
        return (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
    }
}

// 输出 tint.json（材质名 -> {kind, color?}；无 kind 表示不上色），供 Blender 插件按接口接线
void CreateTintJsonFile(const std::unordered_map<std::string, TintResult>& uniqueTints) {
    if (uniqueTints.empty()) return;

    nlohmann::json root = nlohmann::json::object();
    for (const auto& entry : uniqueTints) {
        const TintResult& tint = entry.second;
        nlohmann::json value = nlohmann::json::object();
        if (tint.on()) {
            value["kind"] = TintKindName(tint.kind);
            if (tint.kind == TintKind::Fixed) {
                nlohmann::json color = nlohmann::json::array();
                color.push_back(SrgbChannelToLinear((tint.color >> 16) & 0xFF));
                color.push_back(SrgbChannelToLinear((tint.color >> 8) & 0xFF));
                color.push_back(SrgbChannelToLinear(tint.color & 0xFF));
                value["color"] = std::move(color);
            }
        }
        root[entry.first] = std::move(value);
    }

    std::string path = getExecutableDir() + "tint.json";
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file << root.dump(2);
        file.close();
    }
    else {
        std::cerr << "Failed to create tint.json: " << path << std::endl;
    }
}

// 创建 .mtl 文件,接收 textureToPath 作为参数
void createMtlFile(const ModelData& data, const std::string& mtlFileName) {
    std::string exeDir = getExecutableDir();
    std::string fullMtlPath = exeDir + mtlFileName + ".mtl";

    std::ofstream mtlFile(fullMtlPath);
    if (mtlFile.is_open()) {
        for (size_t i = 0; i < data.materials.size(); ++i) {
            const std::string& textureName = data.materials[i].name;
            std::string texturePath = data.materials[i].texturePath;

            mtlFile << "newmtl " << textureName << "\n";

            // 处理材质类型
            if (texturePath == "None") {
                // LIGHT材质处理
                mtlFile << "Ns 200.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ka 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.900000 0.900000 0.900000\n";
                mtlFile << "Ke 0.900000 0.900000 0.900000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 2\n";
            }
            // 缺失纹理不写虚假的 .png 引用，避免 Blender 显示紫色材质。
            else if (texturePath.empty()) {
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            // 处理纯颜色材质(支持流体格式:color#r g b-流体名 和普通格式:color#r g b=)
            else if (texturePath.find("color#") != std::string::npos) {
                std::string colorStr;
                size_t pos = texturePath.find("-");
                size_t pos_deng = texturePath.find("=");
                if (pos != std::string::npos) {
                    // 流体材质格式,提取"-"前面的颜色部分
                    colorStr = texturePath.substr(std::string("color#").size(), pos - std::string("color#").size());
                }
                else if (pos_deng != std::string::npos) {
                    colorStr = texturePath.substr(std::string("color#").size(), pos_deng - std::string("color#").size());
                }
                else {
                    colorStr = "";
                }
                std::istringstream iss(colorStr);
                float r, g, b;
                if (iss >> r >> g >> b) {
                    mtlFile << "Kd " << std::fixed << std::setprecision(config.decimalPlaces)
                        << r << " " << g << " " << b << "\n";
                }
                else {
                    mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                    std::cerr << "Error: Invalid color format in '" << texturePath << "'\n";
                }
                // 共用普通材质参数
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";
            }
            else {
                // 普通纹理材质处理
                mtlFile << "Ns 90.000000\n";
                mtlFile << "Kd 1.000000 1.000000 1.000000\n";
                mtlFile << "Ks 0.000000 0.000000 0.000000\n";
                mtlFile << "Ke 0.000000 0.000000 0.000000\n";
                mtlFile << "Ni 1.500000\n";
                mtlFile << "illum 1\n";

                // 处理纹理路径
                if (mtlFileName.find("//") != std::string::npos) {
                    texturePath = "../" + texturePath;
                }
                if (texturePath.find(".png") == std::string::npos) {
                    texturePath += ".png";
                }
                mtlFile << "map_Kd " << texturePath << "\n";
                mtlFile << "map_d " << texturePath << "\n";
            }

            mtlFile << "\n";
        }
        mtlFile.close();
    }
    else {
        std::cerr << "Failed to create .mtl file: " << mtlFileName << std::endl;
    }
}

// 单独的文件创建方法
void CreateModelFiles(const ModelData& data, const std::string& filename) {
    auto start = high_resolution_clock::now();  // 新增:开始时间点
    // 创建MTL文件
    try
    {
        createMtlFile(data, filename);
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error occurred: " << e.what() << std::endl;
    }
    if (data.vertices.size()>8000)
    {
        // 创建OBJ文件(内存映射)
        createObjFileViaMemoryMapped(data, filename);
    }
    else
    {
        // 创建OBJ文件
        createObjFile(data, filename);
    }
    
    
    
    auto end = high_resolution_clock::now();  // 新增:结束时间点
    auto duration = duration_cast<milliseconds>(end - start);  // 新增:计算时间差
    std::cout << "模型导出obj耗时: " << duration.count() << " ms" << std::endl;  // 新增:输出到控制台
    
    
}

//多个obj文件创建方法
void CreateMultiModelFiles(const ModelData& data, const std::string& filename,
    std::unordered_map<std::string, std::string>& uniqueMaterialsL,
    const std::string& sharedMtlName) { // 新增共享mtl名称参数
    auto start = high_resolution_clock::now();

    if (data.vertices.size() > 8000) {
        createObjFileViaMemoryMapped(data, filename, sharedMtlName); // 传递共享mtl名称
    }
    else {
        createObjFile(data, filename, sharedMtlName); // 传递共享mtl名称
    }

    // 收集材质信息
    for (size_t i = 0; i < data.materials.size(); ++i) {
        uniqueMaterialsL[data.materials[i].name] = data.materials[i].texturePath;
    }

    auto end = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end - start);
    std::cout << "模型导出obj耗时: " << duration.count() << " ms" << std::endl;
}