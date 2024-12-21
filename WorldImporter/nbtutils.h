#ifndef NBTUTILS_H
#define NBTUTILS_H

#include <string>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <cstdint>
#include <type_traits>  // ���� std::is_integral

// NBT Tag Types ö�٣���ʾ��ͬ��NBT��ǩ����
enum class TagType : uint8_t {
    END = 0,         // TAG_End����ʾû�и���ı�ǩ
    BYTE = 1,        // TAG_Byte��8λ�з�������
    SHORT = 2,       // TAG_Short��16λ�з�������
    INT = 3,         // TAG_Int��32λ�з�������
    LONG = 4,        // TAG_Long��64λ�з�������
    FLOAT = 5,       // TAG_Float��32λ����������
    DOUBLE = 6,      // TAG_Double��64λ����������
    BYTE_ARRAY = 7,  // TAG_Byte_Array���ֽ�����
    STRING = 8,      // TAG_String��UTF-8�����ַ���
    LIST = 9,        // TAG_List����ǩ�б�
    COMPOUND = 10,   // TAG_Compound�����ϱ�ǩ��������һ������������
    INT_ARRAY = 11,  // TAG_Int_Array����������
    LONG_ARRAY = 12  // TAG_Long_Array������������
};

// NbtTag ǰ������
struct NbtTag;
using NbtTagPtr = std::shared_ptr<NbtTag>;  // ʹ��shared_ptr�Ա�����ڴ�

// NbtTag �ṹ�壬��ʾһ��NBT��ǩ
struct NbtTag {
    TagType type;  // ��ǩ������
    std::string name;  // ��ǩ������
    std::vector<char> payload;  // ��ǩ�����ݸ���
    std::vector<NbtTagPtr> children;  // �ӱ�ǩ�б�
    TagType listType;  // LIST��ǩ��Ԫ�ص����ͣ�Ĭ��ΪEND

    NbtTag(TagType t, const std::string& n)
        : type(t), name(n), listType(TagType::END) {
    }

    // �������ͻ�ȡֵ
    template <typename T>
    T getValue() const {
        // �������Ǵ�������Ϳ���ת��Ϊ T
        T value;
        std::memcpy(&value, payload.data(), sizeof(T));
        return value;
    }
};


// �����ֽ�˳��ת������˵������ֽ���
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
    return value; // �������Ͳ�����
}

// ��������������ǩ����ת��Ϊ�ַ��������ڵ���
std::string tagTypeToString(TagType type);

// ������������ȡUTF-8�ַ�������������λ��
std::string readUtf8String(const std::vector<char>& data, size_t& index);

// ������������ȡһ��NBT��ǩ����������λ��
NbtTagPtr readTag(const std::vector<char>& data, size_t& index);

// ������������ȡLIST���ͱ�ǩ����������λ��
NbtTagPtr readListTag(const std::vector<char>& data, size_t& index);

// ������������ȡCOMPOUND���ͱ�ǩ����������λ��
NbtTagPtr readCompoundTag(const std::vector<char>& data, size_t& index);


// �������������ֽ�����ת��Ϊ�ɶ����ַ���
std::string bytesToString(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ�ֽ����ͣ�8λ�з���������
int8_t bytesToByte(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ�����ͣ�16λ�з���������
int16_t bytesToShort(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ���ͣ�32λ�з���������
int32_t bytesToInt(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ�����ͣ�64λ�з���������
int64_t bytesToLong(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ������������32λ������������
float bytesToFloat(const std::vector<char>& payload);

// �������������ֽ�����ת��Ϊ˫���ȸ�������64λ������������
double bytesToDouble(const std::vector<char>& payload);

//����Ⱥϵ���ձ�
void loadBiomeMapping(const std::string& filepath);

// ͨ�����ֻ�ȡ�Ӽ���ǩ
NbtTagPtr getChildByName(const NbtTagPtr& tag, const std::string& childName);

//��ȡ�Ӽ���ǩ
std::vector<NbtTagPtr> getChildren(const NbtTagPtr& tag);

//��ȡTag����
TagType getTagType(const NbtTagPtr& tag);

// ͨ��������ȡLIST��ǩ�е�Ԫ��
NbtTagPtr getListElementByIndex(const NbtTagPtr& tag, size_t index);

// ��ȡ����� tag �洢��ֵ
void getTagValue(const NbtTagPtr& tag, int depth);

// ��ȡ section �µ� biomes ��ǩ��TAG_Compound��
NbtTagPtr getBiomes(const NbtTagPtr& sectionTag);

//��ȡ�������Ⱥϵ����
std::vector<int> getBiomeData(const NbtTagPtr& biomesTag, const std::unordered_map<std::string, int>& biomeMapping);

// ��ȡ biomes �µ� palette ��ǩ��LIST �����ַ�����
std::vector<std::string> getBiomePalette(const NbtTagPtr& biomesTag);

// ��ȡ section �µ� block_states ��ǩ��TAG_Compound��
NbtTagPtr getBlockStates(const NbtTagPtr& sectionTag);

// ��ȡ block_states �� palette ����
std::vector<std::string> getBlockPalette(const NbtTagPtr& blockStatesTag);

// ���� block_states �� data ����
std::vector<int> getBlockStatesData(const NbtTagPtr& blockStatesTag, const std::vector<std::string>& blockPalette);

NbtTagPtr getSectionByIndex(const NbtTagPtr& rootTag, int sectionIndex);
#endif // NBTUTILS_H
