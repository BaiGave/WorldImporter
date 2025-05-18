#ifndef DECOMPRESSOR_H
#define DECOMPRESSOR_H

#include <vector>
#include <string> 

//zlib解压方法
bool DecompressData(const std::vector<char>& chunkData, std::vector<char>& decompressedData);

#endif // DECOMPRESSOR_H
