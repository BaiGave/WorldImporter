#ifndef CONFIG_H
#define CONFIG_H

#include <string>

struct Config {
    std::string directoryPath;  // Minecraft 世界路径
    std::string biomeMappingFile; // 生物群系映射文件路径
    int minX, minY, minZ, maxX, maxY, maxZ; // 坐标范围
    int status; // 运行状态：0=未运行，1=运行中，2=已完成

    Config()
        : directoryPath(""), biomeMappingFile(""),
        minX(0), minY(0), minZ(0), maxX(0), maxY(0), maxZ(0), status(0) {
    }
};

// 声明写入配置函数
void WriteConfig(const Config& config, const std::string& configFile);
// 声明读取配置函数
Config LoadConfig(const std::string& configFile);

#endif // CONFIG_H
