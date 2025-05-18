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
 * @brief 从配置文件加载固体方块列表
 * 
 * @param filepath 配置文件路径
 */
void LoadSolidBlocks(const std::string& filepath);

/**
 * @brief 从配置文件加载流体方块定义
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

#endif // FILEUTILS_H
