// TaskMonitor.h
#ifndef TASK_MONITOR_H
#define TASK_MONITOR_H

#include <string>
#include <mutex>
#include <functional>
#include <atomic>
#include <unordered_map>

// 任务状态枚举
enum class TaskStatus {
    IDLE,                     // 空闲状态
    INITIALIZING,             // 初始化
    CALCULATING_LOD,          // 计算LOD等级
    GENERATING_CHUNK_BATCHES, // 生成区块批次
    LOADING_CHUNKS,           // 加载区块
    GENERATING_MODELS,        // 生成模型
    PROCESSING_BATCH,         // 处理批次
    DEDUPLICATING_VERTICES,   // 去重顶点
    DEDUPLICATING_UV,         // 去重UV
    DEDUPLICATING_FACES,      // 去重面
    GREEDY_MESHING,           // 贪心网格合并
    EXPORTING_MODELS,         // 导出模型
    COMPLETED,                // 完成
    FAILED                    // 错误状态
};

// 将TaskStatus转换为可读字符串
std::string TaskStatusToString(TaskStatus status);

// 进度信息结构体
struct ProgressInfo {
    int current = 0;          // 当前进度
    int total = 0;            // 总数
    std::string description;  // 额外描述信息
    
    // 计算百分比进度
    float GetPercentage() const {
        if (total <= 0) return 0.0f;
        return static_cast<float>(current) * 100.0f / static_cast<float>(total);
    }
};

// 任务监控类
class TaskMonitor {
public:
    // 获取单例实例
    static TaskMonitor& GetInstance();

    // 更新当前任务状态
    void SetStatus(TaskStatus status, const std::string& description = "");
    
    // 获取当前任务状态
    TaskStatus GetStatus() const;
    
    // 获取当前任务状态描述
    std::string GetStatusDescription() const;
    
    // 更新进度信息
    void UpdateProgress(const std::string& category, int current, int total, const std::string& description = "");
    
    // 获取进度信息
    ProgressInfo GetProgress(const std::string& category) const;
    
    // 重置所有状态
    void Reset();
    
    // 设置状态变化回调函数（可选）
    using StatusCallback = std::function<void(TaskStatus, const std::string&)>;
    void SetStatusCallback(StatusCallback callback);
    
    // 设置进度变化回调函数（可选）
    using ProgressCallback = std::function<void(const std::string&, const ProgressInfo&)>;
    void SetProgressCallback(ProgressCallback callback);

private:
    // 私有构造函数和析构函数（单例模式）
    TaskMonitor();
    ~TaskMonitor() = default;
    
    // 禁止拷贝和赋值
    TaskMonitor(const TaskMonitor&) = delete;
    TaskMonitor& operator=(const TaskMonitor&) = delete;
    
    // 当前任务状态
    std::atomic<TaskStatus> m_currentStatus;
    std::string m_statusDescription;
    
    // 不同类别的进度信息
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ProgressInfo> m_progressMap;
    
    // 回调函数
    StatusCallback m_statusCallback;
    ProgressCallback m_progressCallback;
};

// 全局访问函数
inline TaskMonitor& GetTaskMonitor() {
    return TaskMonitor::GetInstance();
}

#endif // TASK_MONITOR_H 