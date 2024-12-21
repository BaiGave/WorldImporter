#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <string>
#include <vector>
#include <iostream>

std::vector<char> ReadFileToMemory(const std::string& directoryPath, int regionX, int regionZ);
unsigned CalculateOffset(const std::vector<char>& fileData, int x, int z);
unsigned ExtractChunkLength(const std::vector<char>& fileData, unsigned offset);

bool ExportChunkNBTDataToFile(const std::vector<char>& data, const std::string& filePath);


#endif // FILEUTILS_H
