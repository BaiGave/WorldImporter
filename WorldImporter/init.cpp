#include "init.h"
#include "RegionModelExporter.h"
#include "logutil.h"
#include <thread>
#include <iostream>
#include <chrono>

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
        std::cerr << "警告：无法设置进程优先级。" << std::endl;
    } else {
        std::cout << "已设置进程高优先级" << std::endl;
    }
#elif defined(__linux__) || defined(__APPLE__)
    // Linux/MacOS实现
    if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
        std::cerr << "警告：无法设置进程优先级。" << std::endl;
    } else {
        std::cout << "已设置进程高优先级" << std::endl;
    }
#endif
}

void init() {
    std::cout << "========== WorldImporter 初始化开始 ==========" << std::endl;
    std::cout.flush();
    auto init_t0 = std::chrono::high_resolution_clock::now();

    // 尝试设置高优先级
    SetHighPriority();

    // 配置必须先加载
    { CrafterLog::StageTimer t("设置全局 locale"); SetGlobalLocale(); }
    { CrafterLog::StageTimer t("清理纹理文件夹"); DeleteTexturesFolder(); }
    {
        CrafterLog::StageTimer t("加载配置 config.json");
        config = LoadConfig("config\\config.json");
    }
    std::cout << "  配置摘要: worldPath=" << config.worldPath
              << " | status=" << config.status
              << " | activeLOD=" << (config.activeLOD ? "true" : "false")
              << " | useGreedyMesh=" << (config.useGreedyMesh ? "true" : "false")
              << std::endl;
    std::cout.flush();

    // 配置加载完成后，再初始化缓存
    { CrafterLog::StageTimer t("初始化全部缓存"); InitializeAllCaches(); }
    { CrafterLog::StageTimer t("加载流体方块表"); LoadFluidBlocks(config.fluidsFile); }
    { CrafterLog::StageTimer t("注册流体纹理"); RegisterFluidTextures(); }
    { CrafterLog::StageTimer t("初始化全局方块调色板"); InitializeGlobalBlockPalette(); }

    auto init_t1 = std::chrono::high_resolution_clock::now();
    auto init_ms = std::chrono::duration_cast<std::chrono::milliseconds>(init_t1 - init_t0).count();
    std::cout << "========== WorldImporter 初始化完成 (" << init_ms << "ms) ==========" << std::endl;
    std::cout.flush();
}
