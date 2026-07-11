#pragma once

// ============================================================
// WorldImporter 日志辅助工具（header-only）
// 提供阶段计时器与通用日志输出，供各模块统一使用。
// 所有输出走 std::cout/std::cerr，会被 Blender 端的管道读取
// 并显示在插件 UI 的日志区域。
// ============================================================

#include <iostream>
#include <chrono>
#include <string>

namespace CrafterLog {

// 计时单位：毫秒
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
}

// 阶段计时器：构造时输出“开始”，析构时输出“完成 + 耗时”
// 用法：{ StageTimer t("阶段名"); do_something(); }
struct StageTimer {
    const char* name;
    std::chrono::high_resolution_clock::time_point t0;
    bool silent;

    explicit StageTimer(const char* n, bool silent_ = false)
        : name(n), t0(std::chrono::high_resolution_clock::now()), silent(silent_) {
        if (!silent) {
            std::cout << "▶ " << name << std::endl;
            std::cout.flush();
        }
    }

    ~StageTimer() {
        if (!silent) {
            auto t1 = std::chrono::high_resolution_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "■ " << name << " 完成 (" << ms << "ms)" << std::endl;
            std::cout.flush();
        }
    }

    // 禁止拷贝/移动，避免误用
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
};

// 便捷输出函数
inline void info(const std::string& msg) {
    std::cout << msg << std::endl;
    std::cout.flush();
}

inline void warn(const std::string& msg) {
    std::cerr << "警告: " << msg << std::endl;
    std::cerr.flush();
}

inline void error(const std::string& msg) {
    std::cerr << "错误: " << msg << std::endl;
    std::cerr.flush();
}

} // namespace CrafterLog

// 便捷宏：在当前块内创建一个匿名阶段计时器
// 用法：CRAFTER_STAGE("阶段名") { do_something(); }
// 或直接 CRAFTER_STAGEscoped("阶段名"); 配合 { } 块使用
#define CRAFTER_STAGE_TIMER(varname, name) ::CrafterLog::StageTimer varname(name)
