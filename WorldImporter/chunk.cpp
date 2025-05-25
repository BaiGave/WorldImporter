#include "nbtutils.h"
#include "fileutils.h"
#include "locutil.h"
#include "decompressor.h"
#include <vector>
#include <iostream>

using namespace std;

/**
 * @brief 计算区块在区域文件中的偏移量
 * 
 * @param fileData 区域文件数据
 * @param x 区块X坐标(相对区域)
 * @param z 区块Z坐标(相对区域)
 * @return unsigned 区块在文件中的偏移量字节位置，失败返回0
 */
unsigned CalculateChunkOffset(const std::vector<char>& fileData, int x, int z) {
    // 计算索引位置(每个索引占4字节)
    unsigned index = 4 * (x + z * 32);
    
    // 检查是否越界
    if (index + 3 >= fileData.size()) {
        cerr << "错误: 无效的索引或文件大小." << endl;
        return 0; // 返回0表示计算失败
    }
    
    // 读取前3个字节计算偏移量(大端字节序)
    unsigned char byte1 = fileData[index];
    unsigned char byte2 = fileData[index + 1];
    unsigned char byte3 = fileData[index + 2];
    
    // 将3字节转换为偏移量(单位:4KB扇区)
    return (byte1 * 256 * 256 + byte2 * 256 + byte3) * 4096;
}

/**
 * @brief 提取区块数据的长度
 * 
 * @param fileData 区域文件数据
 * @param offset 区块数据起始偏移量
 * @return unsigned 区块数据长度(字节)
 */
unsigned ExtractChunkLength(const std::vector<char>& fileData, unsigned offset) {
    // 读取4字节长度值(大端字节序)
    unsigned byte1 = (unsigned char)fileData[offset];
    unsigned byte2 = (unsigned char)fileData[offset + 1];
    unsigned byte3 = (unsigned char)fileData[offset + 2];
    unsigned byte4 = (unsigned char)fileData[offset + 3];
    
    // 合并4字节为长度值
    return (byte1 << 24) | (byte2 << 16) | (byte3 << 8) | byte4;
}

/**
 * @brief 获取区块的NBT数据
 * 
 * 该函数从区域文件中提取特定区块的NBT数据，过程包括:
 * 1. 计算区块在文件中的偏移位置
 * 2. 提取区块数据长度
 * 3. 提取并解压区块数据
 * 
 * @param fileData 区域文件的原始二进制数据
 * @param x 区块X坐标(全局)
 * @param z 区块Z坐标(全局)
 * @return std::vector<char> 解压后的区块NBT数据，失败返回空vector
 */
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z) {
    // 第1步: 计算区块在区域文件中的偏移量
    int localX = mod32(x);  // 转换为区域内相对坐标(0-31)
    int localZ = mod32(z);
    unsigned offset = CalculateChunkOffset(fileData, localX, localZ);
    
    if (offset == 0) {
        cerr << "错误: 偏移计算失败." << endl;
        return {};
    }

    // 第2步: 提取区块数据长度
    unsigned length = ExtractChunkLength(fileData, offset);
    
    // 根据 length 和 offset 检查整个区块数据是否在文件范围内
    uint64_t endOffset = static_cast<uint64_t>(offset) + 4 + length;
    if (endOffset > fileData.size()) {
        cerr << "错误: 区块数据超出了文件边界." << endl;
        return {};
    }
    unsigned startOffset = offset + 5; // 跳过4字节长度+1字节压缩类型
    vector<char> chunkData(fileData.begin() + startOffset, fileData.begin() + endOffset);
    vector<char> decompressedData;
    if (DecompressData(chunkData, decompressedData)) {
        return decompressedData;
    } else {
        cerr << "错误: 解压失败." << endl;
        return {};
    }
}

/**
 * @brief 解析区块高度图数据
 * 
 * 该函数从压缩的高度图数据中提取256个高度值
 * 支持8位和9位格式的高度图数据
 * 
 * @param data 高度图原始数据(通常是37个int64值)
 * @return std::vector<int> 256个高度值构成的数组
 */
std::vector<int> DecodeHeightMap(const std::vector<int64_t>& data) {
    // 预分配256个高度值,避免多次重分配
    std::vector<int> heights;
    heights.reserve(256);
    
    // 根据数据长度动态判断存储格式(37个long为9位格式,否则为8位)
    int bitsPerEntry = (data.size() == 37) ? 9 : 8;
    int entriesPerLong = 64 / bitsPerEntry;  // 每个long值可存储的条目数
    int mask = (1 << bitsPerEntry) - 1;      // 创建位掩码(例如8位为0xFF, 9位为0x1FF)
    
    // 从每个long值中提取多个高度值
    for (const auto& longVal : data) {
        int64_t value = reverseEndian(longVal);  // 处理字节序
        for (int i = 0; i < entriesPerLong; ++i) {
            // 提取指定位置的bits
            heights.push_back(static_cast<int>((value >> (i * bitsPerEntry)) & mask));
            if (heights.size() >= 256) break;  // 达到256个高度值后停止
        }
        if (heights.size() >= 256) break;      // 确保不超过256个值
    }
    
    // 确保返回恰好256个高度值
    heights.resize(256);
    return heights;
}
