#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <string>
#include <vector>
#include <iostream>


/**
 * @brief 设置全局区域设置(locale)为支持中文,使用UTF-8编码
 */
void SetGlobalLocale();

/**
 * @brief 将宽字符串(wstring)转换为UTF-8编码的字符串
 * 
 * @param wstr 输入的宽字符串
 * @return std::string 转换后的UTF-8字符串
 */
std::string wstring_to_string(const std::wstring& wstr);

/**
 * @brief 将UTF-8字符串转换为宽字符串(wstring)
 * 
 * @param str 输入的UTF-8字符串
 * @return std::wstring 转换后的宽字符串
 */
std::wstring string_to_wstring(const std::string& str);


/**
 * @brief 从配置文件加载流体方块定�?
 * 
 * @param filepath 配置文件路径
 */
void LoadFluidBlocks(const std::string& filepath);

/**
 * @brief 注册流体相关的材质
 */
void RegisterFluidTextures();


/**
 * @brief 删除材质文件夹
 * 删除程序目录下的textures和biomeTex文件夹
 */
void DeleteTexturesFolder();


/**
 * @brief Native path separator: '\\' on _WIN32, '/' elsewhere.
 */
inline char PathSep() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

inline wchar_t PathSepW() {
#ifdef _WIN32
    return L'\\';
#else
    return L'/';
#endif
}

inline const char* PathSepStr() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

inline const wchar_t* PathSepWStr() {
#ifdef _WIN32
    return L"\\";
#else
    return L"/";
#endif
}

/**
 * @brief Join path components with the native separator.
 * Empty parts are skipped; redundant leading/trailing separators on
 * intermediate parts are trimmed. Does not rewrite Minecraft resource
 * paths (those stay '/'-separated by convention).
 */
inline std::string JoinPath(std::initializer_list<std::string> parts) {
    std::string out;
    bool first = true;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (first) {
            // Preserve absolute roots (leading '/', '\\', or drive letters).
            out = part;
            while (out.size() > 1 && (out.back() == '/' || out.back() == '\\')) out.pop_back();
            first = false;
            continue;
        }
        size_t start = 0;
        size_t end = part.size();
        while (start < end && (part[start] == '/' || part[start] == '\\')) ++start;
        while (end > start && (part[end - 1] == '/' || part[end - 1] == '\\')) --end;
        if (start >= end) continue;
        if (!out.empty() && out.back() != '/' && out.back() != '\\') {
            out.push_back(PathSep());
        }
        out.append(part, start, end - start);
    }
    return out;
}

inline std::string JoinPath(const std::string& a, const std::string& b) {
    return JoinPath({a, b});
}

inline std::string JoinPath(const std::string& a, const std::string& b, const std::string& c) {
    return JoinPath({a, b, c});
}

inline std::string JoinPath(const std::string& a, const std::string& b, const std::string& c, const std::string& d) {
    return JoinPath({a, b, c, d});
}

#endif // FILEUTILS_H
