#ifndef DECOMPRESSOR_H
#define DECOMPRESSOR_H

#include <vector>
#include <string> 

bool DecompressData(const std::vector<char>& chunkData, std::vector<char>& decompressedData);
bool SaveDecompressedData(const std::vector<char>& decompressedData, const std::string& outputFileName);

#endif // DECOMPRESSOR_H
