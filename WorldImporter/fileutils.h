#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <string>
#include <vector>
#include <iostream>

std::vector<char> ReadFileToMemory(const std::string& directoryPath, int regionX, int regionZ);
unsigned CalculateOffset(const std::vector<char>& fileData, int x, int z);
unsigned ExtractChunkLength(const std::vector<char>& fileData, unsigned offset);

bool ExportChunkNBTDataToFile(const std::vector<char>& data, const std::string& filePath);

void GenerateSolidsJson(const std::string& outputPath, const std::vector<std::string>& targetParentPaths);

// 设置全局 locale 为支持中文，支持 UTF-8 编码
void SetGlobalLocale();

// 将 wstring 转换为 UTF-8 编码的 string
std::string wstring_to_string(const std::wstring& wstr);

// 将 string 转换为 UTF-8 编码的 wstring
std::wstring string_to_wstring(const std::string& str);

// 将std::wstring转换为Windows系统默认的多字节编码（通常为 GBK 或 ANSI）
std::string wstring_to_system_string(const std::wstring& wstr);

// 获取文件夹名（路径中的最后一部分）
std::wstring GetFolderNameFromPath(const std::wstring& folderPath);

void printBytes(const std::vector<char>& data);


#endif // FILEUTILS_H
