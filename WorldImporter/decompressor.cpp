#include <zlib.h>
#include <array>
#include <limits>
#include <iostream>
#include "decompressor.h"

namespace {
    // 每个区块的硬上限，与压缩输入大小无关。
    constexpr size_t MaxOutputSize = 64ull * 1024 * 1024;

    // 通用 zlib 流式解压：windowBits=15 为 zlib 包装，15+16 为 gzip 包装。
    bool InflateBuffer(const std::vector<char>& chunkData, std::vector<char>& decompressedData, int windowBits) {
        decompressedData.clear();
        if (chunkData.empty() || chunkData.size() > std::numeric_limits<uInt>::max()) {
            return false;
        }
        z_stream strm{};
        if (inflateInit2(&strm, windowBits) != Z_OK) {
            std::cerr << "错误: inflateInit2 初始化失败" << std::endl;
            return false;
        }

        struct InflateGuard {
            z_stream* stream;
            ~InflateGuard() { inflateEnd(stream); }
        } guard{&strm};

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(chunkData.data()));
        strm.avail_in = static_cast<uInt>(chunkData.size());

        std::array<char, 64 * 1024> buffer{};
        int result = Z_OK;
        while (true) {
            strm.next_out = reinterpret_cast<Bytef*>(buffer.data());
            strm.avail_out = static_cast<uInt>(buffer.size());

            result = inflate(&strm, Z_NO_FLUSH);
            size_t produced = buffer.size() - strm.avail_out;

            if (produced > 0) {
                if (produced > MaxOutputSize - decompressedData.size()) {
                    result = Z_MEM_ERROR; // 超出安全上限,按失败处理
                    break;
                }
                decompressedData.insert(decompressedData.end(), buffer.begin(), buffer.begin() + produced);
            }

            if (result == Z_STREAM_END) break;
            if (result != Z_OK) break; // Z_BUF_ERROR/Z_DATA_ERROR 等:数据截断或损坏
            if (produced == 0 && strm.avail_in == 0) break; // 无进展,防死循环
        }

        if (result == Z_STREAM_END) {
            return true;
        }
        std::cerr << "错误: 解压失败,错误代码: " << result << std::endl;
        decompressedData.clear();
        return false;
    }
}

// zlib 解压(.mca 压缩类型 2,Minecraft 默认)
bool DecompressData(const std::vector<char>& chunkData, std::vector<char>& decompressedData) {
    return InflateBuffer(chunkData, decompressedData, 15);
}

// gzip 解压(.mca 压缩类型 1,第三方工具导出的区块)
bool DecompressGzip(const std::vector<char>& chunkData, std::vector<char>& decompressedData) {
    return InflateBuffer(chunkData, decompressedData, 15 + 16);
}
