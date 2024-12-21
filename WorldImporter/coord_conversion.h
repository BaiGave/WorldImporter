#ifndef COORD_CONVERSION_H
#define COORD_CONVERSION_H


// ���� YZX ����������
int toYZX(int x, int y, int z);

//�������Ǹ��������16��ʹ���Ϊ����
int mod16(int value);

//�������Ǹ��������32��ʹ���Ϊ����
int mod32(int value);

// ������������������ת��Ϊ��������
void chunkToRegion(int chunkX, int chunkZ, int& regionX, int& regionZ);

// ������������������ת��Ϊ��������
void blockToChunk(int blockX, int blockZ, int& chunkX, int& chunkZ);

// ��������������Y����ת��Ϊ������Y����
void blockYToSectionY(int blockY, int& chunkY);
#endif // COORD_CONVERSION_H
