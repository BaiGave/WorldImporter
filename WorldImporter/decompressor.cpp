#include <zlib.h>
#include <iostream>
#include "decompressor.h"

// 解压区块数据
bool DecompressData(const std::vector<char>& chunkData, std::vector<char>& decompressedData) {
    // 输出压缩数据的大小
    uLongf decompressedSize = chunkData.size() * 10;  // 假设解压后的数据大小为压缩数据的 10 倍
    decompressedData.resize(decompressedSize);

    // 调用解压函数
    int result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
        reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

    // 如果输出缓冲区太小,则动态扩展缓冲区
    while (result == Z_BUF_ERROR) {
        decompressedSize *= 2;  // 增加缓冲区大小
        decompressedData.resize(decompressedSize);
        result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
            reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

    }

    // 根据解压结果提供不同的日志信息
    if (result == Z_OK) {
        decompressedData.resize(decompressedSize);  // 修正解压数据的实际大小
        return true;
    }
    else {
        std::cerr <<"错误: 解压失败,错误代码: " << result << std::endl;

        return false;
    }
}

