// TaskMonitor.cpp
#include "TaskMonitor.h"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <thread>

// 存储最后一次输出的每个类别的进度信息
static std::unordered_map<std::string, bool> g_categoryPrinted;

// 全局互斥锁，用于控制控制台输出
static std::mutex g_consoleMutex;

// 将TaskStatus转换为可读字符串
std::string TaskStatusToString(TaskStatus status) {
    switch (status) {
        case TaskStatus::IDLE: return "空闲";
        case TaskStatus::INITIALIZING: return "init";
        case TaskStatus::CALCULATING_LOD: return "计算LOD等级";
        case TaskStatus::GENERATING_CHUNK_BATCHES: return "生成区块批次";
        case TaskStatus::LOADING_CHUNKS: return "加载区块";
        case TaskStatus::GENERATING_MODELS: return "生成模型";
        case TaskStatus::PROCESSING_BATCH: return "处理批次";
        case TaskStatus::DEDUPLICATING_VERTICES: return "去重顶点";
        case TaskStatus::DEDUPLICATING_UV: return "去重UV坐标";
        case TaskStatus::DEDUPLICATING_FACES: return "DEDUPLICATING_FACES";
        case TaskStatus::GREEDY_MESHING: return "贪心网格合并";
        case TaskStatus::EXPORTING_MODELS: return "导出模型";
        case TaskStatus::COMPLETED: return "完成";
        case TaskStatus::FAILED: return "错误";
        default: return "UNKNOW";
    }
}

// 获取当前时间戳的格式化字符串
std::string GetTimeStamp() {
    auto now = std::chrono::system_clock::now();
    auto nowTime = std::chrono::system_clock::to_time_t(now);
    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&nowTime), "%H:%M:%S");
    ss << '.' << std::setfill('0') << std::setw(3) << nowMs;
    return ss.str();
}

// 单例实例
TaskMonitor& TaskMonitor::GetInstance() {
    static TaskMonitor instance;
    return instance;
}

// 构造函数
TaskMonitor::TaskMonitor()
    : m_currentStatus(TaskStatus::IDLE),
      m_statusDescription(""),
      m_statusCallback(nullptr),
      m_progressCallback(nullptr) {
}

// 更新当前任务状态
void TaskMonitor::SetStatus(TaskStatus status, const std::string& description) {
    // 更新状态
    m_currentStatus.store(status);
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusDescription = description;
    }
    
    // 获取控制台锁，确保输出不会被其他线程打断
    std::lock_guard<std::mutex> consoleLock(g_consoleMutex);
    
    // 输出状态变化日志
    std::cout << "[" << GetTimeStamp() << "] 状态: " 
              << TaskStatusToString(status);
    
    if (!description.empty()) {
        std::cout << " - " << description;
    }
    std::cout << std::endl;
    
    // 调用回调函数(如果已设置)
    if (m_statusCallback) {
        m_statusCallback(status, description);
    }
}

// 获取当前任务状态
TaskStatus TaskMonitor::GetStatus() const {
    return m_currentStatus.load();
}

// 获取当前任务状态描述
std::string TaskMonitor::GetStatusDescription() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_statusDescription;
}

// 限制进度更新频率的函数
bool ShouldUpdateProgress(const std::string& category, int current, int total) {
    static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastUpdateTime;
    static std::unordered_map<std::string, int> lastPercentage;
    
    auto now = std::chrono::steady_clock::now();
    
    // 首次更新或最后一次更新，始终显示
    if (lastUpdateTime.find(category) == lastUpdateTime.end() || current >= total) {
        lastUpdateTime[category] = now;
        lastPercentage[category] = static_cast<int>((static_cast<float>(current) / total) * 100);
        return true;
    }
    
    // 计算当前百分比
    int currentPercentage = static_cast<int>((static_cast<float>(current) / total) * 100);
    
    // 时间间隔检查：至少100ms一次更新
    auto timeSinceLastUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - lastUpdateTime[category]).count();
    
    // 进度变化检查：百分比至少变化1%才更新
    bool percentageChanged = (currentPercentage != lastPercentage[category]);
    
    // 如果时间间隔足够长且进度有变化，或者时间间隔超过500ms，则更新
    if ((timeSinceLastUpdate >= 100 && percentageChanged) || timeSinceLastUpdate >= 500) {
        lastUpdateTime[category] = now;
        lastPercentage[category] = currentPercentage;
        return true;
    }
    
    return false;
}

// 更新进度信息
void TaskMonitor::UpdateProgress(const std::string& category, int current, int total, const std::string& description) {
    ProgressInfo progressInfo;
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        progressInfo.current = current;
        progressInfo.total = total;
        progressInfo.description = description;
        
        m_progressMap[category] = progressInfo;
    }
    
    // 检查是否应该更新进度显示（限制频率）
    if (!ShouldUpdateProgress(category, current, total)) {
        // 调用回调函数(如果已设置)，即使不更新显示
        if (m_progressCallback) {
            m_progressCallback(category, progressInfo);
        }
        return;
    }
    
    // 构建进度信息字符串
    std::stringstream progressStream;
    progressStream << "[" << GetTimeStamp() << "] 进度: " << category 
                 << " " << current << "/" << total;
    
    if (total > 0) {
        float percentage = static_cast<float>(current) * 100.0f / static_cast<float>(total);
        progressStream << " (" << std::fixed << std::setprecision(2) << percentage << "%)";
    }
    
    if (!description.empty()) {
        progressStream << " - " << description;
    }
    
    // 获取控制台锁，确保输出不会被其他线程打断
    std::lock_guard<std::mutex> consoleLock(g_consoleMutex);
    
    // 使用\r回到行首，覆盖之前的输出
    // 如果是第一次输出这个类别的进度，或者进度已完成，则输出一个完整行
    bool isFirstLine = (g_categoryPrinted.find(category) == g_categoryPrinted.end());
    bool isCompleted = (current >= total);
    
    if (isFirstLine) {
        g_categoryPrinted[category] = true;
    }
    
    if (isFirstLine || isCompleted) {
        std::cout << progressStream.str() << std::endl;
        if (isCompleted) {
            // 完成后移除跟踪
            g_categoryPrinted.erase(category);
        }
    } else {
        // 使用\r回到行首，覆盖之前的输出
        // 先清除当前行，避免残留字符
        std::cout << "\r" << std::string(120, ' ') << "\r" << progressStream.str() << std::flush;
    }
    
    // 调用回调函数(如果已设置)
    if (m_progressCallback) {
        m_progressCallback(category, progressInfo);
    }
}

// 获取进度信息
ProgressInfo TaskMonitor::GetProgress(const std::string& category) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    auto it = m_progressMap.find(category);
    if (it != m_progressMap.end()) {
        return it->second;
    }
    
    // 如果找不到该类别，返回空的进度信息
    return ProgressInfo{};
}

// 重置所有状态
void TaskMonitor::Reset() {
    m_currentStatus.store(TaskStatus::IDLE);
    
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusDescription.clear();
        m_progressMap.clear();
    }
    
    // 获取控制台锁
    std::lock_guard<std::mutex> consoleLock(g_consoleMutex);
    
    // 清空类别打印跟踪
    g_categoryPrinted.clear();
    
    std::cout << "[" << GetTimeStamp() << "] State Reset" << std::endl;
}

// 设置状态变化回调函数
void TaskMonitor::SetStatusCallback(StatusCallback callback) {
    m_statusCallback = callback;
}

// 设置进度变化回调函数
void TaskMonitor::SetProgressCallback(ProgressCallback callback) {
    m_progressCallback = callback;
} 