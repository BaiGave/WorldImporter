#ifndef DECOMPRESSOR_H
#define DECOMPRESSOR_H

#include <vector>
#include <string> 



//zlib解压方法
bool DecompressData(const std::vector<char>& chunkData, std::vector<char>& decompressedData);
bool SaveDecompressedData(const std::vector<char>& decompressedData, const std::string& outputFileName);

//gzip解压方法
std::vector<char> DecompressGzip(const std::wstring& filePath);
#endif // DECOMPRESSOR_H
