#include "fileutils.h"
#include <iostream>
#include <fstream>
#include <sstream>

using namespace std;


std::vector<char> ReadFileToMemory(const std::string& directoryPath, int regionX, int regionZ) {
    // 构造区域文件的路径
    std::ostringstream filePathStream;
    filePathStream << directoryPath << "/region/r." << regionX << "." << regionZ << ".mca";
    std::string filePath = filePathStream.str();

    // 打开文件
    std::ifstream file(filePath, std::ios::binary);
    if (!file) {
        std::cerr << "错误: 打开文件失败！" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 将文件内容读取到文件数据中
    std::vector<char> fileData;
    fileData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (fileData.empty()) {
        std::cerr << "错误: 文件为空或读取失败！" << std::endl;
        return {};  // 返回空vector表示失败
    }

    // 返回读取到的文件数据
    return fileData;
}

unsigned CalculateOffset(const vector<char>& fileData, int x, int z) {
    // 直接使用 x 和 z，不需要进行模运算
    unsigned index = 4 * (x + z * 32);

    // 检查是否越界
    if (index + 3 >= fileData.size()) {
        cerr << "错误: 无效的索引或文件大小。" << endl;
        return 0; // 返回 0 表示计算失败
    }

    // 读取字节
    unsigned char byte1 = fileData[index];
    unsigned char byte2 = fileData[index + 1];
    unsigned char byte3 = fileData[index + 2];

    // 根据字节计算偏移位置 (假设按大端字节序)
    unsigned offset = (byte1 * 256 * 256 + byte2 * 256 + byte3) * 4096;

    return offset;
}


unsigned ExtractChunkLength(const vector<char>& fileData, unsigned offset) {
    // 确保以无符号字节的方式进行计算
    unsigned byte1 = (unsigned char)fileData[offset];
    unsigned byte2 = (unsigned char)fileData[offset + 1];
    unsigned byte3 = (unsigned char)fileData[offset + 2];
    unsigned byte4 = (unsigned char)fileData[offset + 3];

    // 计算区块长度
    return (byte1 << 24) | (byte2 << 16) | (byte3 << 8) | byte4;
}

// 导出区块 NBT 数据到指定的文件
bool ExportChunkNBTDataToFile(const vector<char>& data, const string& filePath) {
    // 打开文件进行二进制写入
    ofstream outFile(filePath, ios::binary);
    if (!outFile) {
        cerr << "无法打开文件: " << filePath << endl;
        return false;  // 无法打开文件，返回 false
    }

    // 写入数据到文件
    outFile.write(data.data(), data.size());
    if (!outFile) {
        cerr << "写入文件失败: " << filePath << endl;
        return false;  // 写入失败，返回 false
    }

    outFile.close();  // 关闭文件
    return true;  // 成功保存数据到文件，返回 true
}