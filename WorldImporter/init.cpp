#include "init.h"
#include "RegionModelExporter.h"
#include <thread>
#include <iostream>

#ifdef _WIN32
extern "C" {
    __declspec(dllimport) void* __stdcall GetCurrentProcess();
    __declspec(dllimport) int __stdcall SetPriorityClass(void* hProcess, unsigned int dwPriorityClass);
}
#ifndef REALTIME_PRIORITY_CLASS
#define REALTIME_PRIORITY_CLASS 0x00000100
#endif
#elif defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#include <sys/resource.h>
#endif

void SetHighPriority() {
#ifdef _WIN32
    // Windows实现
    if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS)) {
    }
#elif defined(__linux__) || defined(__APPLE__)
    // Linux/MacOS实现
    if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
        std::cerr << "警告：无法设置进程优先级。" << std::endl;
    }
#endif
}

void init() {
    // 尝试设置高优先级
    SetHighPriority();
    
    // 配置必须先加载
    SetGlobalLocale();
    // 删除纹理文件夹
    DeleteTexturesFolder();
    config = LoadConfig("config\\config.json");
    
    // 配置加载完成后，再初始化缓存
    InitializeAllCaches();
    LoadSolidBlocks(config.solidBlocksFile);
    LoadFluidBlocks(config.fluidsFile);
    RegisterFluidTextures();
    InitializeGlobalBlockPalette();
}