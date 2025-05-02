// dat.h
#ifndef DAT_H
#define DAT_H

#include <vector>
#include <string>
#include "nbtutils.h" // 引入 readTag
#include <zlib.h> // 引入 zlib 库

// 声明读取 NBT 数据的方法
class DatFileReader {
public:
    // 读取 .dat 文件并返回 NBT 数据
    static NbtTagPtr readDatFile(const std::string& filePath);

private:
    // 辅助方法,用于读取文件内容为字符数组
    static std::vector<char> readFile(const std::string& filePath);

    
};

#endif // DAT_H
