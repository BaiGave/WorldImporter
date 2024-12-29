#include "dat.h"
#include "fileutils.h"
#include "decompressor.h"
#include <fstream>
#include <iostream>


// 读取文件内容到字符数组
std::vector<char> DatFileReader::readFile(const std::string& filePath) {
    // 判断文件是否是 Gzip 压缩的
    if (filePath.substr(filePath.find_last_of('.') + 1) == "dat") {
        std::wstring path = string_to_wstring(filePath);
        // 如果是 .gz 文件，先解压
        return DecompressGzip(path);
    }

    // 打开文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("无法打开文件: " + filePath);
    }

    // 获取文件大小
    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // 读取文件内容到字符数组
    std::vector<char> data(fileSize);
    file.read(data.data(), fileSize);

    return data;
}


// 读取 .dat 文件，并返回 NBT 标签
NbtTagPtr DatFileReader::readDatFile(const std::string& filePath) {
    // 读取文件内容
    std::vector<char> data = readFile(filePath);

    // 读取 NBT 数据
    size_t index = 0;
    return readTag(data, index);
}
