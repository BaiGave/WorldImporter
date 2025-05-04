#include "nbtutils.h"
#include "fileutils.h"
#include "locutil.h"
#include "decompressor.h"
#include <vector>
#include <iostream>

using namespace std;

//获取区块的NBT数据
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z) {
    unsigned offset = CalculateOffset(fileData, mod32(x), mod32(z));

    if (offset == 0) {
        cerr << "错误: 偏移计算失败." << endl;
        return {};
    }

    unsigned length = ExtractChunkLength(fileData, offset);
    if (offset + 5 <= fileData.size()) {
        int startOffset = offset + 5;
        int endIndex = startOffset + length - 1;

        if (endIndex < fileData.size()) {
            vector<char> chunkData(fileData.begin() + startOffset, fileData.begin() + endIndex + 1);
            vector<char> decompressedData;

            if (DecompressData(chunkData, decompressedData)) {
                return decompressedData;
            } else {
                cerr << "错误: 解压失败." << endl;
                return {};
            }
        } else {
            cerr << "错误: 区块数据超出了文件边界." << endl;
            return {};
        }
    } else {
        cerr << "错误: 从偏移位置读取5个字节的数据不够." << endl;
        return {};
    }
}

// 解析区块高度图
std::vector<int> DecodeHeightMap(const std::vector<int64_t>& data) {
    // 预分配256个高度值,避免多次重分配
    std::vector<int> heights;
    heights.reserve(256);
    // 根据数据长度动态判断存储格式
    int bitsPerEntry = (data.size() == 37) ? 9 : 8;
    int entriesPerLong = 64 / bitsPerEntry;
    int mask = (1 << bitsPerEntry) - 1;
    for (const auto& longVal : data) {
        int64_t value = reverseEndian(longVal);
        for (int i = 0; i < entriesPerLong; ++i) {
            heights.push_back(static_cast<int>((value >> (i * bitsPerEntry)) & mask));
            if (heights.size() >= 256) break;
        }
        if (heights.size() >= 256) break;
    }
    heights.resize(256);
    return heights;
}
