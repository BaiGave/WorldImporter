/**
 * @file chunk.h
 * @brief 处理Minecraft世界区块数据的相关函数
 * 
 * 本文件提供了读取和解析Minecraft世界区块数据的函数。
 * 区块(Chunk)是Minecraft世界的基本单位，大小为16x16x256方块。
 * 区域文件(Region File)通常包含32x32个区块的数据，以mca格式存储。
 */
#pragma once
#include <vector>

/**
 * @brief 从区域文件数据中读取特定区块的NBT数据
 * 
 * @param fileData 区域文件的完整二进制数据
 * @param x 区块的X坐标(全局坐标)
 * @param z 区块的Z坐标(全局坐标)
 * @return std::vector<char> 解压后的区块NBT数据，如果提取失败则返回空vector
 */
std::vector<char> GetChunkNBTData(const std::vector<char>& fileData, int x, int z);

/**
 * @brief 解析区块的高度图数据
 * 
 * 高度图数据表示区块中每个列(x,z位置)的最高非空气方块的y坐标
 * 
 * @param data 从NBT数据中提取的原始高度图数据(通常是一组Long值)
 * @return std::vector<int> 由256个高度值组成的数组，对应区块内16x16个列
 */
std::vector<int> DecodeHeightMap(const std::vector<int64_t>& data);