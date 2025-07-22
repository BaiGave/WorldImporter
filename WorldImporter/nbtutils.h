#ifndef NBTUTILS_H
#define NBTUTILS_H

#include <string>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <cstdint>
#include <cstring>
#include <vector>
#include <type_traits>  // 用于 std::is_integral

// NBT Tag Types 枚举,表示不同的NBT标签类型
enum class TagType : uint8_t {
    END = 0,         // TAG_End,表示没有更多的标签
    BYTE = 1,        // TAG_Byte,8位有符号整数
    SHORT = 2,       // TAG_Short,16位有符号整数
    INT = 3,         // TAG_Int,32位有符号整数
    LONG = 4,        // TAG_Long,64位有符号整数
    FLOAT = 5,       // TAG_Float,32位浮动精度数
    DOUBLE = 6,      // TAG_Double,64位浮动精度数
    BYTE_ARRAY = 7,  // TAG_Byte_Array,字节数组
    STRING = 8,      // TAG_String,UTF-8编码字符串
    LIST = 9,        // TAG_List,标签列表
    COMPOUND = 10,   // TAG_Compound,复合标签,类似于一个命名的容器
    INT_ARRAY = 11,  // TAG_Int_Array,整数数组
    LONG_ARRAY = 12  // TAG_Long_Array,长整型数组
};

// NbtTag 前向声明
struct NbtTag;
using NbtTagPtr = std::shared_ptr<NbtTag>;  // 使用shared_ptr以便管理内存

// NbtTag 结构体,表示一个NBT标签
struct NbtTag {
    TagType type;  // 标签的类型
    std::string name;  // 标签的名称
    std::vector<char> payload;  // 标签的数据负载
    std::vector<NbtTagPtr> children;  // 子标签列表
    TagType listType;  // LIST标签中元素的类型,默认为END

    NbtTag(TagType t, const std::string& n)
        : type(t), name(n), listType(TagType::END) {
    }

    // 根据类型获取值
    template <typename T>
    T getValue() const {
        // 假设我们处理的类型可以转换为 T
        T value;
        std::memcpy(&value, payload.data(), sizeof(T));
        return value;
    }
};


// 用于字节顺序转换(大端到主机字节序)
template <typename T>
T byteSwap(T value) {
    static_assert(std::is_integral<T>::value, "Only integral types are supported");

    if constexpr (sizeof(T) == 2) {
        return static_cast<T>((value << 8) | (value >> 8));
    }
    else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(
            ((value & 0xFF000000) >> 24) |
            ((value & 0x00FF0000) >> 8) |
            ((value & 0x0000FF00) << 8) |
            ((value & 0x000000FF) << 24));
    }
    else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(
            ((value & 0xFF00000000000000) >> 56) |
            ((value & 0x00FF000000000000) >> 40) |
            ((value & 0x0000FF0000000000) >> 24) |
            ((value & 0x000000FF00000000) >> 8) |
            ((value & 0x00000000FF000000) << 8) |
            ((value & 0x0000000000FF0000) << 24) |
            ((value & 0x000000000000FF00) << 40) |
            ((value & 0x00000000000000FF) << 56));
    }
    return value; // 其他类型不处理
}

// 函数声明:将标签类型转换为字符串,便于调试
std::string tagTypeToString(TagType type);

// 函数声明:读取UTF-8字符串并更新索引位置
std::string readUtf8String(const std::vector<char>& data, size_t& index);

// 函数声明:读取一个NBT标签并更新索引位置
NbtTagPtr readTag(const std::vector<char>& data, size_t& index);

// 函数声明:读取LIST类型标签并更新索引位置
NbtTagPtr readListTag(const std::vector<char>& data, size_t& index);

// 函数声明:读取COMPOUND类型标签并更新索引位置
NbtTagPtr readCompoundTag(const std::vector<char>& data, size_t& index);

std::vector<int> readIntArray(const std::vector<char>& payload);
// 帮助函数:将字节数组转换为可读的字符串
std::string bytesToString(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为字节类型(8位有符号整数)
int8_t bytesToByte(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为短整型(16位有符号整数)
int16_t bytesToShort(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为整型(32位有符号整数)
int32_t bytesToInt(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为长整型(64位有符号整数)
int64_t bytesToLong(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为浮动精度数(32位浮动精度数)
float bytesToFloat(const std::vector<char>& payload);

// 帮助函数:将字节数组转换为双精度浮动数(64位浮动精度数)
double bytesToDouble(const std::vector<char>& payload);

long long reverseEndian(long long value);

// 通过名字获取子级标签
NbtTagPtr getChildByName(const NbtTagPtr& tag, const std::string& childName);

//获取子级标签
std::vector<NbtTagPtr> getChildren(const NbtTagPtr& tag);

//获取Tag类型
TagType getTagType(const NbtTagPtr& tag);

// 通过索引获取LIST标签中的元素
NbtTagPtr getListElementByIndex(const NbtTagPtr& tag, size_t index);

//获取并输出String类型tag存储的值
std::string getStringTag(const NbtTagPtr& tag);

// 获取并输出 tag 存储的值
void getTagValue(const NbtTagPtr& tag, int depth);

// 获取 section 下的 biomes 标签(TAG_Compound)
NbtTagPtr getBiomes(const NbtTagPtr& sectionTag);


// 获取 biomes 下的 palette 标签(LIST 包含字符串)
std::vector<std::string> getBiomePalette(const NbtTagPtr& biomesTag);

// 获取 section 下的 block_states 标签(TAG_Compound)
NbtTagPtr getBlockStates(const NbtTagPtr& sectionTag);

// 读取 block_states 的 palette 数据
std::vector<std::string> getBlockPalette(const NbtTagPtr& blockStatesTag);

// 解析 block_states 的 data 数据
std::vector<int> getBlockStatesData(const NbtTagPtr& blockStatesTag, const std::vector<std::string>& blockPalette);

NbtTagPtr getSectionByIndex(const NbtTagPtr& rootTag, int sectionIndex);
#endif // NBTUTILS_H
