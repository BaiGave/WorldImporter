#include "decompressor.h"
#include <zlib.h>
#include <iostream>
#include <fstream>

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
        cerr << "错误: 解压失败，错误代码: " << result << endl;
        cerr << "错误: 输出缓冲区太小" << endl;
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
