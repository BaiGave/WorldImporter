#include <cstdint>
#include "chunk.h"
#include "decompressor.h"
#include "biome.h"
#include <algorithm>
#include <zlib.h>
#include <iostream>
#include <stdexcept>
#include <vector>

// nbtutils supplies reverseEndian to chunk.cpp. Biome parsing is outside
// this I/O test; fail loudly if it is accidentally reached.
int Biome::GetId(const std::string&) {
    throw std::runtime_error("unexpected biome lookup in chunk I/O test");
}

static void check(bool ok, const char* name) {
    if (!ok) throw std::runtime_error(name);
    std::cout << "PASS " << name << '\n';
}
static void put32(std::vector<char>& b, size_t pos, uint32_t n) {
    for (int i = 0; i < 4; ++i) b[pos + i] = static_cast<char>(n >> (24 - i * 8));
}
static std::vector<char> compress(const std::vector<char>& input, int bits) {
    z_stream s{};
    if (deflateInit2(&s, Z_BEST_SPEED, Z_DEFLATED, bits, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("deflateInit2");
    std::vector<char> out(deflateBound(&s, static_cast<uLong>(input.size())));
    s.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.data()));
    s.avail_in = static_cast<uInt>(input.size());
    s.next_out = reinterpret_cast<Bytef*>(out.data());
    s.avail_out = static_cast<uInt>(out.size());
    int result = deflate(&s, Z_FINISH);
    size_t size = s.total_out;
    deflateEnd(&s);
    if (result != Z_STREAM_END) throw std::runtime_error("deflate");
    out.resize(size);
    return out;
}
static std::vector<char> region(const std::vector<char>& data, int type) {
    size_t sectors = (data.size() + 5 + 4095) / 4096;
    std::vector<char> b(8192 + sectors * 4096);
    put32(b, 0, static_cast<uint32_t>((2 << 8) | sectors));
    put32(b, 8192, static_cast<uint32_t>(data.size() + 1));
    b[8196] = static_cast<char>(type);
    std::copy(data.begin(), data.end(), b.begin() + 8197);
    return b;
}
int main() {
    try {
        // Minimal valid NBT compound, including its end tag.
        const std::vector<char> raw{10, 0, 0, 0};
        auto z = compress(raw, 15);
        auto g = compress(raw, 31);
        check(GetChunkNBTData(region(z, 2), 0, 0) == raw, "zlib chunk");
        check(GetChunkNBTData(region(g, 1), 0, 0) == raw, "gzip chunk");
        check(GetChunkNBTData(region(raw, 3), 0, 0) == raw, "uncompressed chunk");
        check(GetChunkNBTData(region(raw, 99), 0, 0).empty(), "unknown compression");
        check(GetChunkNBTData(region(raw, 130), 0, 0).empty(), "external chunk rejected explicitly");
        check(GetChunkNBTData({}, 0, 0).empty(), "empty region");
        check(GetChunkNBTData(std::vector<char>(8192), 0, 0).empty(), "absent chunk");
        auto b = region(z, 2);
        put32(b, 0, 0xffffff01);
        check(GetChunkNBTData(b, 0, 0).empty(), "offset outside file");
        b = region(z, 2); b.resize(8195);
        check(GetChunkNBTData(b, 0, 0).empty(), "truncated length header");
        b.resize(8196);
        check(GetChunkNBTData(b, 0, 0).empty(), "missing compression byte");
        b = region(z, 2); put32(b, 8192, 0);
        check(GetChunkNBTData(b, 0, 0).empty(), "zero length");
        put32(b, 8192, 1);
        check(GetChunkNBTData(b, 0, 0).empty(), "empty payload");
        put32(b, 8192, 0xffffffff);
        check(GetChunkNBTData(b, 0, 0).empty(), "oversized length");
        b = region(z, 2); b[3] = 0;
        check(GetChunkNBTData(b, 0, 0).empty(), "zero sector count");
        b = region(z, 2); put32(b, 0, 257);
        check(GetChunkNBTData(b, 0, 0).empty(), "offset inside region header");
        b = region(z, 2); b.resize(16384); put32(b, 8192, 4093);
        check(GetChunkNBTData(b, 0, 0).empty(), "length exceeds allocated sectors");
        b = region(z, 2);
        std::copy(b.begin(), b.begin() + 4, b.begin() + 4092);
        check(GetChunkNBTData(b, -1, -1) == raw, "negative chunk coordinates");
        std::vector<char> out{1, 2, 3};
        check(!DecompressData({}, out) && out.empty(), "empty input clears output");
        z.pop_back();
        check(!DecompressData(z, out) && out.empty(), "truncated zlib rejected");
        g.pop_back();
        check(!DecompressGzip(g, out) && out.empty(), "truncated gzip rejected");
        std::vector<char> large(64 * 1024 * 1024, 'x');
        auto packed = compress(large, 15);
        check(DecompressData(packed, out) && out == large, "64 MiB boundary accepted");
        large.push_back('x'); packed = compress(large, 15);
        check(!DecompressData(packed, out) && out.empty(), "above 64 MiB rejected");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
