#include "decompressor.h"
#include <zlib.h>
#include <iostream>
#include <fstream>

using namespace std;

// ��ѹ��������
bool DecompressData(const vector<char>& chunkData, vector<char>& decompressedData) {
    // ���ѹ�����ݵĴ�С
    //cout << "ѹ�����ݴ�С: " << chunkData.size() << " �ֽ�" << endl;

    uLongf decompressedSize = chunkData.size() * 10;  // �����ѹ������ݴ�СΪѹ�����ݵ� 10 ��
    decompressedData.resize(decompressedSize);

    // ���ý�ѹ����
    int result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
        reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

    // ������������̫С����̬��չ������
    while (result == Z_BUF_ERROR) {
        decompressedSize *= 2;  // ���ӻ�������С
        decompressedData.resize(decompressedSize);
        result = uncompress(reinterpret_cast<Bytef*>(decompressedData.data()), &decompressedSize,
            reinterpret_cast<const Bytef*>(chunkData.data()), chunkData.size());

        //cout << "�������ӻ�������С��: " << decompressedSize << " �ֽ�" << endl;
    }

    // ���ݽ�ѹ����ṩ��ͬ����־��Ϣ
    if (result == Z_OK) {
        decompressedData.resize(decompressedSize);  // ������ѹ���ݵ�ʵ�ʴ�С
        //cout << "��ѹ�ɹ�����ѹ�����ݴ�С: " << decompressedSize << " �ֽ�" << endl;
        return true;
    }
    else {
        cerr << "����: ��ѹʧ�ܣ��������: " << result << endl;
        cerr << "����: ���������̫С" << endl;
        return false;
    }
}

// ����ѹ������ݱ��浽�ļ�
bool SaveDecompressedData(const vector<char>& decompressedData, const string& outputFileName) {
    ofstream outFile(outputFileName, ios::binary);
    if (outFile) {
        outFile.write(decompressedData.data(), decompressedData.size());
        outFile.close();
        cout << "��ѹ�����ѱ��浽�ļ�: " << outputFileName << endl;
        return true;
    }
    else {
        cerr << "����: �޷���������ļ�: " << outputFileName << endl;
        return false;
    }
}
