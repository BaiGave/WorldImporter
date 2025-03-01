#include "nbtutils.h"
#include <iostream>
#include <cstring>
#include <iomanip>
#include <cmath>
#include <unordered_map>
#include "biome.h"

// 将 TagType 转换为字符串的辅助函数
std::string tagTypeToString(TagType type) {
    switch (type) {
    case TagType::END: return "TAG_End";
    case TagType::BYTE: return "TAG_Byte";
    case TagType::SHORT: return "TAG_Short";
    case TagType::INT: return "TAG_Int";
    case TagType::LONG: return "TAG_Long";
    case TagType::FLOAT: return "TAG_Float";
    case TagType::DOUBLE: return "TAG_Double";
    case TagType::BYTE_ARRAY: return "TAG_Byte_Array";
    case TagType::STRING: return "TAG_String";
    case TagType::LIST: return "TAG_List";
    case TagType::COMPOUND: return "TAG_Compound";
    case TagType::INT_ARRAY: return "TAG_Int_Array";
    case TagType::LONG_ARRAY: return "TAG_Long_Array";
    default: return "Unknown";
    }
}

// 将字节转换为值的辅助函数
std::string bytesToString(const std::vector<char>& payload) {
    return std::string(payload.begin(), payload.end());
}

int8_t bytesToByte(const std::vector<char>& payload) {
    return static_cast<int8_t>(payload[0]);
}

int16_t bytesToShort(const std::vector<char>& payload) {
    return (static_cast<uint8_t>(payload[0]) << 8) | static_cast<uint8_t>(payload[1]);
}

int32_t bytesToInt(const std::vector<char>& payload) {
    return (static_cast<uint8_t>(payload[0]) << 24) |
        (static_cast<uint8_t>(payload[1]) << 16) |
        (static_cast<uint8_t>(payload[2]) << 8) |
        static_cast<uint8_t>(payload[3]);
}

int64_t bytesToLong(const std::vector<char>& payload) {
    int64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint8_t>(payload[i]);
    }
    return value;
}

float bytesToFloat(const std::vector<char>& payload) {
    uint32_t asInt = bytesToInt(std::vector<char>(payload.begin(), payload.begin() + 4));
    float value;
    std::memcpy(&value, &asInt, sizeof(float));
    return value;
}

double bytesToDouble(const std::vector<char>& payload) {
    int64_t asLong = bytesToLong(std::vector<char>(payload.begin(), payload.begin() + 8));
    double value;
    std::memcpy(&value, &asLong, sizeof(double));
    return value;
}

// 从索引处开始的数据读取 UTF-8 字符串
std::string readUtf8String(const std::vector<char>& data, size_t& index) {
    if (index + 2 > data.size()) {
        throw std::out_of_range("Not enough data to read string length");
    }
    uint16_t length = (static_cast<uint8_t>(data[index]) << 8) | static_cast<uint8_t>(data[index + 1]);
    index += 2;
    if (index + length > data.size()) {
        return "";
        throw std::out_of_range("Not enough data to read string content");
    }
    std::string str(data.begin() + index, data.begin() + index + length);
    index += length;
    return str;
}
NbtTagPtr readTagPayload(const std::vector<char>& data, size_t& index, TagType type) {
    auto tag = std::make_shared<NbtTag>(type, "");

    switch (type) {
    case TagType::BYTE: {
        if (index + 1 > data.size()) throw std::out_of_range("Not enough data for TAG_Byte");
        tag->payload.push_back(data[index]);
        index += 1;
        break;
    }
    case TagType::SHORT: {
        if (index + 2 > data.size()) throw std::out_of_range("Not enough data for TAG_Short");
        tag->payload.push_back(data[index]);
        tag->payload.push_back(data[index + 1]);
        index += 2;
        break;
    }

    case TagType::INT: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Int");
        for (int i = 0; i < 4; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 4;
        break;
    }
    case TagType::LONG: {
        if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Long");
        for (int i = 0; i < 8; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 8;
        break;
    }
    case TagType::FLOAT: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Float");
        for (int i = 0; i < 4; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 4;
        break;
    }
    case TagType::DOUBLE: {
        if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Double");
        for (int i = 0; i < 8; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 8;
        break;
    }

    case TagType::BYTE_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Byte_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + length > data.size()) throw std::out_of_range("Not enough data for TAG_Byte_Array payload");
        tag->payload.insert(tag->payload.end(), data.begin() + index, data.begin() + index + length);
        index += length;
        break;
    }
    case TagType::STRING: {
        std::string str = readUtf8String(data, index);
        tag->payload.assign(str.begin(), str.end());
        break;
    }

    case TagType::LIST: {
        // 读取元素类型和长度
        TagType listType = static_cast<TagType>(data[index++]);
        int32_t length = 0;
        for (int i = 0; i < 4; ++i) {
            length = (length << 8) | static_cast<uint8_t>(data[index++]);
        }

        // 递归读取列表元素
        for (int32_t i = 0; i < length; ++i) {
            auto elem = readTagPayload(data, index, listType); // 需要实现该辅助函数
            tag->children.push_back(elem);
        }
        break;
    }

    case TagType::COMPOUND: {
        // 递归读取直到遇到TAG_End
        while (true) {
            auto child = readTag(data, index);
            if (!child) break; // 遇到TAG_End
            tag->children.push_back(child);
        }
        break;
    }

    case TagType::INT_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Int_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + (4 * length) > data.size()) throw std::out_of_range("Not enough data for TAG_Int_Array payload");
        for (int i = 0; i < length; ++i) {
            for (int j = 0; j < 4; ++j) {
                tag->payload.push_back(data[index++]);
            }
        }
        break;
    }
    case TagType::LONG_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Long_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + (8 * length) > data.size()) throw std::out_of_range("Not enough data for TAG_Long_Array payload");
        for (int i = 0; i < length; ++i) {
            for (int j = 0; j < 8; ++j) {
                tag->payload.push_back(data[index++]);
            }
        }
        break;
    }

    default:
        throw std::runtime_error("Unsupported tag type: " + std::to_string(static_cast<int>(type)));
    }

    return tag;
}

// 从索引开始的数据中读取单个标签
NbtTagPtr readTag(const std::vector<char>& data, size_t& index) {
    if (index >= data.size()) {
        throw std::out_of_range("Index out of bounds while reading tag type");
    }

    TagType type = static_cast<TagType>(static_cast<uint8_t>(data[index]));
    index++;

    // TAG_End没有名称和有效负载
    if (type == TagType::END) {
        return nullptr;
    }

    // 读取标签名称（大端序16位无符号长度）
    uint16_t nameLength = (static_cast<uint8_t>(data[index]) << 8) | static_cast<uint8_t>(data[index + 1]);
    index += 2;
    std::string name(data.begin() + index, data.begin() + index + nameLength);
    index += nameLength;

    auto tag = std::make_shared<NbtTag>(type, name);

    switch (type) {
    case TagType::BYTE: {
        if (index + 1 > data.size()) throw std::out_of_range("Not enough data for TAG_Byte");
        tag->payload.push_back(data[index]);
        index += 1;
        break;
    }
    case TagType::SHORT: {
        if (index + 2 > data.size()) throw std::out_of_range("Not enough data for TAG_Short");
        tag->payload.push_back(data[index]);
        tag->payload.push_back(data[index + 1]);
        index += 2;
        break;
    }

    case TagType::INT: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Int");
        for (int i = 0; i < 4; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 4;
        break;
    }
    case TagType::LONG: {
        if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Long");
        for (int i = 0; i < 8; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 8;
        break;
    }
    case TagType::FLOAT: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Float");
        for (int i = 0; i < 4; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 4;
        break;
    }
    case TagType::DOUBLE: {
        if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Double");
        for (int i = 0; i < 8; ++i) {
            tag->payload.push_back(data[index + i]);
        }
        index += 8;
        break;
    }

    case TagType::BYTE_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Byte_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + length > data.size()) throw std::out_of_range("Not enough data for TAG_Byte_Array payload");
        tag->payload.insert(tag->payload.end(), data.begin() + index, data.begin() + index + length);
        index += length;
        break;
    }
    case TagType::STRING: {
        std::string str = readUtf8String(data, index);
        tag->payload.assign(str.begin(), str.end());
        break;
    }

    case TagType::LIST: {
        // 读取元素类型和长度
        TagType listType = static_cast<TagType>(data[index++]);
        int32_t length = 0;
        for (int i = 0; i < 4; ++i) {
            length = (length << 8) | static_cast<uint8_t>(data[index++]);
        }

        // 递归读取列表元素
        for (int32_t i = 0; i < length; ++i) {
            auto elem = readTagPayload(data, index, listType); // 需要实现该辅助函数
            tag->children.push_back(elem);
        }
        break;
    }

    case TagType::COMPOUND: {
        // 递归读取直到遇到TAG_End
        while (true) {
            auto child = readTag(data, index);
            if (!child) break; // 遇到TAG_End
            tag->children.push_back(child);
        }
        break;
    }

    case TagType::INT_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Int_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + (4 * length) > data.size()) throw std::out_of_range("Not enough data for TAG_Int_Array payload");
        for (int i = 0; i < length; ++i) {
            for (int j = 0; j < 4; ++j) {
                tag->payload.push_back(data[index++]);
            }
        }
        break;
    }
    case TagType::LONG_ARRAY: {
        if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Long_Array length");
        int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
            (static_cast<uint8_t>(data[index + 1]) << 16) |
            (static_cast<uint8_t>(data[index + 2]) << 8) |
            static_cast<uint8_t>(data[index + 3]);
        index += 4;
        if (index + (8 * length) > data.size()) throw std::out_of_range("Not enough data for TAG_Long_Array payload");
        for (int i = 0; i < length; ++i) {
            for (int j = 0; j < 8; ++j) {
                tag->payload.push_back(data[index++]);
            }
        }
        break;
    }

    default:
        throw std::runtime_error("Unsupported tag type: " + std::to_string(static_cast<int>(type)));
    }

    return tag;
}

// 从索引开始的数据中读取列表标签
NbtTagPtr readListTag(const std::vector<char>& data, size_t& index) {
    if (index >= data.size()) throw std::out_of_range("Index out of bounds while reading TAG_List element type");

    TagType listType = static_cast<TagType>(static_cast<uint8_t>(data[index]));
    index++; // 跳过字节

    if (index + 4 > data.size()) throw std::out_of_range("Not enough data to read TAG_List length");
    int32_t length = (static_cast<uint8_t>(data[index]) << 24) |
        (static_cast<uint8_t>(data[index + 1]) << 16) |
        (static_cast<uint8_t>(data[index + 2]) << 8) |
        static_cast<uint8_t>(data[index + 3]);
    index += 4;

    auto listTag = std::make_shared<NbtTag>(TagType::LIST, "List");
    listTag->listType = listType; // 存储元素种类
    for (int32_t i = 0; i < length; ++i) {
        if (listType == TagType::END) {
            throw std::runtime_error("TAG_List cannot have TAG_End elements");
        }
        auto elementTag = std::make_shared<NbtTag>(listType, "");
        switch (listType) {
        case TagType::BYTE: {
            if (index + 1 > data.size()) throw std::out_of_range("Not enough data for TAG_Byte in LIST");
            elementTag->payload.push_back(data[index++]);
            break;
        }
        case TagType::SHORT: {
            if (index + 2 > data.size()) throw std::out_of_range("Not enough data for TAG_Short in LIST");
            elementTag->payload.push_back(data[index]);
            elementTag->payload.push_back(data[index + 1]);
            index += 2;
            break;
        }
        case TagType::INT: {
            if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Int in LIST");
            for (int j = 0; j < 4; ++j) {
                elementTag->payload.push_back(data[index++]);
            }
            break;
        }
        case TagType::LONG: {
            if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Long in LIST");
            for (int j = 0; j < 8; ++j) {
                elementTag->payload.push_back(data[index++]);
            }
            break;
        }
        case TagType::FLOAT: {
            if (index + 4 > data.size()) throw std::out_of_range("Not enough data for TAG_Float in LIST");
            for (int j = 0; j < 4; ++j) {
                elementTag->payload.push_back(data[index++]);
            }
            break;
        }
        case TagType::DOUBLE: {
            if (index + 8 > data.size()) throw std::out_of_range("Not enough data for TAG_Double in LIST");
            for (int j = 0; j < 8; ++j) {
                elementTag->payload.push_back(data[index++]);
            }
            break;
        }
        case TagType::STRING: {
            std::string str = readUtf8String(data, index);
            elementTag->payload.assign(str.begin(), str.end());
            break;
        }
        case TagType::COMPOUND: {
            // 复合标签的递归读取
            auto compound = readCompoundTag(data, index);
            if (compound) {
                elementTag->children = compound->children;
            }
            break;
        }
        case TagType::LIST: {
            // 嵌套列表标签的递归读取
            auto subList = readListTag(data, index);
            if (subList) {
                elementTag->children = subList->children;
            }
            break;
        }
        case TagType::BYTE_ARRAY:
        case TagType::INT_ARRAY:
        case TagType::LONG_ARRAY:
            break;
            // 如果需要，对数组实现类似的读取
            throw std::runtime_error("TAG_List with array types not implemented");
        default:
            break;
            throw std::runtime_error("Unsupported tag type inside TAG_List");
        }
        listTag->children.push_back(elementTag);
    }

    return listTag;
}

// 从索引开始的数据中读取复合标签
NbtTagPtr readCompoundTag(const std::vector<char>& data, size_t& index) {
    auto compoundTag = std::make_shared<NbtTag>(TagType::COMPOUND, "Compound");

    while (index < data.size()) {
        TagType type = static_cast<TagType>(static_cast<uint8_t>(data[index]));
        if (type == TagType::END) {
            index++; // 跳过TAG_END
            break;
        }

        auto childTag = readTag(data, index);
        if (childTag) {
            compoundTag->children.push_back(childTag);
        }
    }

    return compoundTag;
}

//-------------------------------基础方法---------------------------------------------


// 通过名字获取子级标签
NbtTagPtr getChildByName(const NbtTagPtr& tag, const std::string& childName) {
    // 确保 tag 不为空
    if (!tag) {
        //std::cerr << "Error: tag is nullptr." << std::endl;
        return nullptr;
    }

    // 确保 tag 是一个 COMPOUND 类型标签
    if (tag->type != TagType::COMPOUND) {
        std::cerr << "Error: tag is not a COMPOUND tag." << std::endl;
        return nullptr;
    }

    // 处理 COMPOUND 类型，查找名字匹配的子标签
    for (const auto& child : tag->children) {
        if (child->name == childName) {
            return child; // 找到匹配的子标签
        }
    }

    // 如果没有找到，返回空指针
    return nullptr;
}


//获取子级标签
std::vector<NbtTagPtr> getChildren(const NbtTagPtr& tag) {
    if (tag->type == TagType::LIST || tag->type == TagType::COMPOUND) {
        return tag->children;
    }
    return {}; // 如果标签不是 LIST 或 COMPOUND，返回空向量
}

//获取Tag类型
TagType getTagType(const NbtTagPtr& tag) {
    return tag->type;
}


// 通过索引获取LIST标签中的元素
NbtTagPtr getListElementByIndex(const NbtTagPtr& tag, size_t index) {
    if (tag->type == TagType::LIST) {
        if (index < tag->children.size()) {
            return tag->children[index]; // 返回指定索引的元素
        }
    }
    return nullptr; // 如果标签类型不是 LIST 或索引无效，返回空指针
}

// 获取并返回 String 类型标签的值
std::string getStringTag(const NbtTagPtr& tag) {
    if (tag->type == TagType::STRING) {
        return std::string(tag->payload.begin(), tag->payload.end());
    }
    return "";  // 如果不是 STRING 类型，则返回空字符串
}

// 获取并输出 tag 存储的值
void getTagValue(const NbtTagPtr& tag, int depth = 0) {
    // 控制输出的缩进
    std::string indent(depth * 2, ' ');

    switch (tag->type) {
    case TagType::BYTE:
        std::cout << indent << "Byte value: " << static_cast<int>(tag->payload[0]) << std::endl;
        break;
    case TagType::SHORT: {
        short value;
        std::memcpy(&value, tag->payload.data(), sizeof(short));
        value = byteSwap(value);  // 如果是大端字节序，转换为主机字节序
        std::cout << indent << "Short value: " << value << std::endl;
        break;
    }
    case TagType::INT: {
        int value;
        std::memcpy(&value, tag->payload.data(), sizeof(int));
        value = byteSwap(value);  // 如果是大端字节序，转换为主机字节序
        std::cout << indent << "Int value: " << value << std::endl;
        break;
    }
    case TagType::LONG: {
        long long value;
        std::memcpy(&value, tag->payload.data(), sizeof(long long));
        value = byteSwap(value);  // 如果是大端字节序，转换为主机字节序
        std::cout << indent << "Long value: " << value << std::endl;
        break;
    }
    case TagType::FLOAT: {
        float value;
        std::memcpy(&value, tag->payload.data(), sizeof(float));
        // 不需要进行字节顺序转换，因为浮点数通常与主机字节序一致
        std::cout << indent << "Float value: " << value << std::endl;
        break;
    }
    case TagType::DOUBLE: {
        double value;
        std::memcpy(&value, tag->payload.data(), sizeof(double));
        // 不需要进行字节顺序转换，因为浮点数通常与主机字节序一致
        std::cout << indent << "Double value: " << value << std::endl;
        break;
    }
    case TagType::STRING: {
        std::cout << indent << "String value: " << std::string(tag->payload.begin(), tag->payload.end()) << std::endl;
        break;
    }
    case TagType::BYTE_ARRAY: {
        std::cout << indent << "Byte array values: ";
        for (const auto& byte : tag->payload) {
            std::cout << static_cast<int>(byte) << " ";
        }
        std::cout << std::endl;
        break;
    }
    case TagType::INT_ARRAY: {
        std::cout << indent << "Int array values: ";
        for (size_t i = 0; i < tag->payload.size() / sizeof(int); ++i) {
            int value;
            std::memcpy(&value, &tag->payload[i * sizeof(int)], sizeof(int));
            value = byteSwap(value);  // 转换为主机字节序
            std::cout << value << " ";
        }
        std::cout << std::endl;
        break;
    }
    case TagType::LONG_ARRAY: {
        std::cout << indent << "Long array values (hex): ";
        for (size_t i = 0; i < tag->payload.size() / sizeof(long long); ++i) {
            long long value;
            std::memcpy(&value, &tag->payload[i * sizeof(long long)], sizeof(long long));
            value = byteSwap(value);  // 转换为主机字节序
            std::cout << std::hex << value << " ";  // 输出为16进制
        }
        std::cout << std::dec << std::endl;  // 重新设置输出为十进制
        break;
    }
    case TagType::COMPOUND:
        std::cout << indent << "Compound tag with " << tag->children.size() << " children:" << std::endl;
        for (const auto& child : tag->children) {
            std::cout << indent << "  Child: " << child->name << std::endl;
            getTagValue(child, depth + 1);  // 递归输出子标签
        }
        break;
    case TagType::LIST:
        std::cout << indent << "List tag with " << tag->children.size() << " elements:" << std::endl;
        for (size_t i = 0; i < tag->children.size(); ++i) {
            std::cout << indent << "  Element " << i << ":" << std::endl;
            getTagValue(tag->children[i], depth + 1);  // 递归输出列表元素
        }
        break;
    case TagType::END:
        std::cout << indent << "End tag (no value)." << std::endl;
        break;
    default:
        std::cout << indent << "Unknown tag type." << std::endl;
        break;
    }
}


//——————————————实用方法————————————————————

// 获取 section 下的 biomes 标签（TAG_Compound）
NbtTagPtr getBiomes(const NbtTagPtr& sectionTag) {
    // 确保 sectionTag 不为空
    if (!sectionTag) {
        std::cerr << "Error: sectionTag is nullptr." << std::endl;
        return nullptr;
    }

    // 确保 section 是一个 COMPOUND 类型标签
    if (sectionTag->type != TagType::COMPOUND) {
        std::cerr << "Error: section is not a COMPOUND tag." << std::endl;
        return nullptr;
    }

    // 通过名字获取 biomes 子标签
    return getChildByName(sectionTag, "biomes");
}


//获取子区块的群系数据
std::vector<int> getBiomeData(const std::shared_ptr<NbtTag>& tag) {
    std::vector<int> biomeIds;
    if (tag && tag->type == TagType::LIST) {
        for (const auto& child : tag->children) {
            if (child->type == TagType::STRING) {
                const std::string& biomeName = child->name;
                int bid = Biome::GetId(biomeName);

                // 处理未注册的生物群系
                if (bid == -1) {
                    static std::once_flag warnFlag;
                    std::call_once(warnFlag, [&]() {
                        std::cerr << "发现未注册的生物群系: " << biomeName
                            << "，将使用默认ID 0\n";
                        });
                    bid = 0; // 默认值
                }
                biomeIds.push_back(bid);
            }
        }
    }
    return biomeIds;
}

// 获取 biomes 下的 palette 标签（LIST 包含字符串）
std::vector<std::string> getBiomePalette(const NbtTagPtr& biomesTag) {
    auto paletteTag = getChildByName(biomesTag, "palette");
    if (!paletteTag || paletteTag->type != TagType::LIST) {
        throw std::runtime_error("No valid palette tag found in biomes.");
    }

    std::vector<std::string> palette;
    for (const auto& child : paletteTag->children) {
        if (child->type == TagType::STRING) {
            std::string biomeName(child->payload.begin(), child->payload.end());
            palette.push_back(biomeName);
        }
    }

    return palette;
}


//——————————————————————————————————————————
// 获取 section 下的 block_states 标签（TAG_Compound）
NbtTagPtr getBlockStates(const NbtTagPtr& sectionTag) {
    // 确保 sectionTag 不为空
    if (!sectionTag) {
        std::cerr << "Error: sectionTag is nullptr." << std::endl;
        return nullptr;
    }

    // 确保 section 是一个 COMPOUND 类型标签
    if (sectionTag->type != TagType::COMPOUND) {
        std::cerr << "Error: section is not a COMPOUND tag." << std::endl;
        return nullptr;
    }

    // 通过名字获取 block_states 子标签
    return getChildByName(sectionTag, "block_states");
}



// 读取 block_states 的 palette 数据
std::vector<std::string> getBlockPalette(const NbtTagPtr& blockStatesTag) {
    std::vector<std::string> blockPalette;

    // 获取 palette 子标签
    auto paletteTag = getChildByName(blockStatesTag, "palette");
    if (paletteTag && paletteTag->type == TagType::LIST) {
        for (const auto& blockTag : paletteTag->children) {
            if (blockTag->type == TagType::COMPOUND) {
                // 解析每个 TAG_Compound
                std::string blockName;
                auto nameTag = getChildByName(blockTag, "Name");
                if (nameTag && nameTag->type == TagType::STRING) {
                    blockName = std::string(nameTag->payload.begin(), nameTag->payload.end());
                }

                // 检查是否有 Properties，拼接后缀
                auto propertiesTag = getChildByName(blockTag, "Properties");
                if (propertiesTag && propertiesTag->type == TagType::COMPOUND) {
                    std::string propertiesStr;
                    for (const auto& property : propertiesTag->children) {
                        if (property->type == TagType::STRING) {
                            propertiesStr += property->name + ":" + std::string(property->payload.begin(), property->payload.end()) + ",";
                        }
                    }
                    if (!propertiesStr.empty()) {
                        propertiesStr.pop_back();  // 移除最后一个逗号
                        blockName += "[" + propertiesStr + "]";
                    }
                }

                blockPalette.push_back(blockName);
            }
        }
    }
    return blockPalette;
}


// 反转字节顺序
long long reverseEndian(long long value) {
    unsigned char* bytes = reinterpret_cast<unsigned char*>(&value);
    std::reverse(bytes, bytes + sizeof(long long));  // 反转字节顺序
    return value;
}



std::vector<int> getBlockStatesData(const NbtTagPtr& blockStatesTag, const std::vector<std::string>& blockPalette) {
    std::vector<int> blockStatesData;

    // 获取 data 子标签
    auto dataTag = getChildByName(blockStatesTag, "data");

    // 如果没有找到 data 标签或者标签类型不对，则直接返回空
    if (!dataTag || dataTag->type != TagType::LONG_ARRAY) {
        return blockStatesData;  // 返回空的 blockStatesData
    }

    size_t numBlocks = dataTag->payload.size() / sizeof(long long);

    // 计算编码位数
    size_t numBlockStates = blockPalette.size();
    int bitsPerState = (numBlockStates <= 16) ? 4 : static_cast<int>(std::ceil(std::log2(numBlockStates)));
    int statesPerLong = 64 / bitsPerState;  // 每个 long 中可以存储的方块状态数量

    for (size_t i = 0; i < numBlocks; ++i) {
        long long encodedState;

        // 使用正确的字节顺序读取 long 数据
        std::memcpy(&encodedState, &dataTag->payload[i * sizeof(long long)], sizeof(long long));

        // 反转字节顺序（如果需要）
        encodedState = reverseEndian(encodedState);

        // 解析每个 long 数据，根据 YZX 编码获取方块状态的索引
        for (int j = 0; j < statesPerLong; ++j) {
            // 根据位数提取对应的方块状态索引
            int blockStateIndex = (encodedState >> (j * bitsPerState)) & ((1 << bitsPerState) - 1);
            blockStatesData.push_back(blockStateIndex);
        }
    }

    return blockStatesData;
}




// 获取 section 及其子标签
NbtTagPtr getSectionByIndex(const NbtTagPtr& rootTag, int sectionIndex) {
    // 获取根标签下的 sections 列表
    auto sectionsTag = getChildByName(rootTag, "sections");
    if (!sectionsTag || sectionsTag->type != TagType::LIST) {
        std::cerr << "Error: sections tag not found or not a LIST." << std::endl;
        return nullptr;
    }

    // 遍历 LIST 标签中的每个元素
    for (size_t i = 0; i < sectionsTag->children.size(); ++i) {
        auto sectionTag = sectionsTag->children[i];

        // 通过 Y 标签获取子区块的索引
        auto yTag = getChildByName(sectionTag, "Y");
        if (yTag && yTag->type == TagType::BYTE) {
            int yValue = static_cast<int>(yTag->payload[0]);

            // 如果找到的 Y 值与给定的 sectionIndex 匹配，返回该子区块
            if (yValue == sectionIndex) {
                return sectionTag;
            }
        }
    }

    // 如果没有找到匹配的子区块，返回 nullptr
    std::cerr << "Error: No section found with index " << sectionIndex << std::endl;
    return nullptr;
}
