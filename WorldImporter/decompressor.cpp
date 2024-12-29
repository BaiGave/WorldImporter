#include "decompressor.h"
#include "fileutils.h"
#include <zlib.h>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <windows.h>

using namespace std;


// 解压区块数据
bool DecompressData(const vector<char>& chunkData, vector<char>& decompressedData) {
    // 输出压缩数据的大小
    //cout << "压缩数据大小: " << chunkData.size() << " 字节" << endl;

    uLongf decompressedSize = chunkData.size() * 10;  // 假设解压后的数据大小为压缩数据的 10 倍
    decompressedData.resize(decompressedSize);

    // 调用解压函数
    int result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
        reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

    // 如果输出缓冲区太小，则动态扩展缓冲区
    while (result == Z_BUF_ERROR) {
        decompressedSize *= 2;  // 增加缓冲区大小
        decompressedData.resize(decompressedSize);
        result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
            reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

        //cout << "尝试增加缓冲区大小到: " << decompressedSize << " 字节" << endl;
    }

    // 根据解压结果提供不同的日志信息
    if (result == Z_OK) {
        decompressedData.resize(decompressedSize);  // 修正解压数据的实际大小
        //cout << "解压成功，解压后数据大小: " << decompressedSize << " 字节" << endl;
        return true;
    }
    else {
        cerr <<"错误: 解压失败，错误代码: " << result << endl;

        return false;
    }
}

// 将解压后的数据保存到文件
bool SaveDecompressedData(const vector<char>& decompressedData, const string& outputFileName) {
    ofstream outFile(outputFileName, ios::binary);
    if (outFile) {
        outFile.write(decompressedData.data(), decompressedData.size());
        outFile.close();
        cout << "解压数据已保存到文件: " << outputFileName << endl;
        return true;
    }
    else {
        cerr << "错误: 无法创建输出文件: " << outputFileName << endl;
        return false;
    }
}

// 解压缩 Gzip 数据
std::vector<char> DecompressGzip(const std::wstring& filePath) {
    // 将std::wstring转换为系统默认的多字节编码（如 GBK）
    std::string filePathStr = wstring_to_system_string(filePath);

    // 打开 Gzip 文件
    gzFile gzFilePtr = gzopen(filePathStr.c_str(), "rb");
    if (!gzFilePtr) {
        throw std::runtime_error("无法打开 Gzip 文件: " + filePathStr);
    }

    // 读取 Gzip 文件的内容
    std::vector<char> decompressedData;
    char buffer[1024];
    int bytesRead;

    while ((bytesRead = gzread(gzFilePtr, buffer, sizeof(buffer))) > 0) {
        decompressedData.insert(decompressedData.end(), buffer, buffer + bytesRead);
    }

    // 错误处理
    if (bytesRead < 0) {
        gzclose(gzFilePtr);
        throw std::runtime_error("读取 Gzip 文件时出错: " + filePathStr);
    }

    // 关闭文件
    gzclose(gzFilePtr);

    return decompressedData;
}