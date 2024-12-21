#include "coord_conversion.h"
#include <cmath>
#include <iostream>
#include <string>

// ���� YZX ����������
int toYZX(int x, int y, int z) {
    int encoded = (y << 8) | (z << 4) | x;
    //std::cout << "Encoded YZX (" << x << ", " << y << ", " << z << ") -> " << encoded << std::endl;
    return encoded;
}

// �������Ǹ��������16��ʹ���Ϊ������ȷ�����ص������� [0, 15] ��Χ��
int mod16(int value) {
    int result = value % 16;  // ��������
    if (result < 0) {
        result += 16;  // ����Ǹ�����ת��Ϊ����
    }
    return result;
}


// �������Ǹ��������32��ʹ���Ϊ������ȷ�����ص������� [0, 31] ��Χ��
int mod32(int value) {
    int result = value % 32;
    return result < 0 ? result + 32 : result;
}

// ��������ת��Ϊ��������
void chunkToRegion(int chunkX, int chunkZ, int& regionX, int& regionZ) {
    // ʹ��λ�ƽ���ת��
    regionX = chunkX >> 5;  // �൱�� chunkX / 32
    regionZ = chunkZ >> 5;  // �൱�� chunkZ / 32
}

// ��������ת��Ϊ��������
void blockToChunk(int blockX, int blockZ, int& chunkX, int& chunkZ) {
    // ʹ��λ�ƽ���ת��
    chunkX = blockX >> 4;  // �൱�� chunkX / 16
    chunkZ = blockZ >> 4;  // �൱�� chunkZ / 16
}

// ����Y����ת��Ϊ������Y����
void blockYToSectionY(int blockY, int& chunkY) {
    chunkY = blockY >> 4; // �൱�� chunkY / 16
}
