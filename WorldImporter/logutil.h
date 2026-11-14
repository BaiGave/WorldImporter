#pragma once
// Thread-safe diagnostics shared by standalone WI and Blender's stdout reader.
// CRAFTER_LOG_DIR overrides the per-user directory; CRAFTER_DEBUG=0 disables DEBUG.
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <source_location>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace CrafterLog {
inline int pid() {
#ifdef _WIN32
    return _getpid();
#else
    return static_cast<int>(getpid());
#endif
}
inline long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool debug_enabled() {
    const char* value = std::getenv("CRAFTER_DEBUG");
    return !value || (std::string(value) != "0" && std::string(value) != "false" && std::string(value) != "off");
}
inline std::filesystem::path log_directory() {
    if (const char* dir = std::getenv("CRAFTER_LOG_DIR")) return dir;
#ifdef _WIN32
    if (const char* dir = std::getenv("LOCALAPPDATA")) return std::filesystem::path(dir) / "Crafter" / "logs";
#elif defined(__APPLE__)
    if (const char* dir = std::getenv("HOME")) return std::filesystem::path(dir) / "Library" / "Logs" / "Crafter";
#else
    if (const char* dir = std::getenv("XDG_STATE_HOME")) return std::filesystem::path(dir) / "crafter" / "logs";
    if (const char* dir = std::getenv("HOME")) return std::filesystem::path(dir) / ".local" / "state" / "crafter" / "logs";
#endif
    return std::filesystem::temp_directory_path() / "crafter-logs";
}

class Logger {
    std::atomic<unsigned long long> errors_{0}, warnings_{0};
    std::mutex mutex_;
    std::ofstream file_;
    std::filesystem::path path_;
    std::uintmax_t bytes_ = 0;
    std::streambuf* out_;
    std::streambuf* err_;
    std::chrono::steady_clock::time_point start_ = std::chrono::steady_clock::now();
    static constexpr std::uintmax_t max_bytes = 5 * 1024 * 1024;

    void rotate(std::size_t incoming) {
        if (!file_.is_open() || bytes_ + incoming <= max_bytes) return;
        file_.close();
        std::error_code ec;
        std::filesystem::remove(path_.string() + ".3", ec);
        for (int index = 2; index >= 0; --index) {
            auto from = index ? path_.string() + "." + std::to_string(index) : path_.string();
            auto to = path_.string() + "." + std::to_string(index + 1);
            if (std::filesystem::exists(from, ec)) {
                std::filesystem::rename(from, to, ec);
                if (ec) { raw_error("log rotation failed: " + ec.message()); break; }
            }
        }
        file_.open(path_, std::ios::out | std::ios::app);
        bytes_ = std::filesystem::file_size(path_, ec);
        if (ec) bytes_ = 0;
    }
    void raw_error(const std::string& text) noexcept {
        try {
            auto line = "[WorldImporter] " + text + "\n";
            err_->sputn(line.data(), static_cast<std::streamsize>(line.size()));
            err_->pubsync();
        } catch (...) {}
    }
public:
    Logger() : out_(std::cout.rdbuf()), err_(std::cerr.rdbuf()) {
        try {
            path_ = log_directory() / ("worldimporter-" + std::to_string(pid()) + ".log");
            std::filesystem::create_directories(path_.parent_path());
            file_.open(path_, std::ios::out | std::ios::app);
            if (!file_) throw std::runtime_error("cannot open " + path_.string());
            bytes_ = std::filesystem::file_size(path_);
        } catch (const std::exception& error) {
            raw_error(std::string("persistent logging unavailable: ") + error.what());
        }
    }
    std::string path() const { return path_.string(); }
    auto errors() const { return errors_.load(); }
    auto warnings() const { return warnings_.load(); }
    void write(const char* level, const std::string& message,
               const std::source_location& source = std::source_location::current()) noexcept {
        try {
            if (std::string(level) == "DEBUG" && !debug_enabled()) return;
            if (std::string(level) == "ERROR") errors_.fetch_add(1);
            if (std::string(level) == "WARN") warnings_.fetch_add(1);
            auto now = std::chrono::system_clock::now();
            auto stamp = std::chrono::system_clock::to_time_t(now);
            auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
            std::lock_guard<std::mutex> lock(mutex_);
            std::tm local{};
#ifdef _WIN32
            localtime_s(&local, &stamp);
#else
            localtime_r(&stamp, &local);
#endif
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_).count();
            std::istringstream input(message);
            std::string part;
            bool first = true;
            while (first || std::getline(input, part)) {
                if (first) { first = false; if (!std::getline(input, part)) part.clear(); }
                std::ostringstream line;
                line << '[' << std::put_time(&local, "%Y-%m-%dT%H:%M:%S") << '.'
                     << std::setw(3) << std::setfill('0') << millis << "] [" << level << "]"
                     << " pid=" << pid() << " thread=" << std::this_thread::get_id()
                     << " elapsed_ms=" << elapsed << " source=" << source.file_name() << ':' << source.line()
                     << " function=" << source.function_name() << " | " << part << '\n';
                auto text = line.str();
                auto* console = std::string(level) == "ERROR" || std::string(level) == "WARN" ? err_ : out_;
                console->sputn(text.data(), static_cast<std::streamsize>(text.size()));
                console->pubsync();
                rotate(text.size());
                if (file_.is_open()) {
                    file_ << text;
                    file_.flush();
                    bytes_ += text.size();
                    if (!file_) { raw_error("writing log failed; console logging continues"); file_.close(); }
                }
            }
        } catch (...) { raw_error("diagnostic write failed; original operation continues"); }
    }
};
inline Logger& logger() { static Logger instance; return instance; }
inline void info(const std::string& message, const std::source_location& source = std::source_location::current()) { logger().write("INFO", message, source); }
inline void debug(const std::string& message, const std::source_location& source = std::source_location::current()) { logger().write("DEBUG", message, source); }
inline void warn(const std::string& message, const std::source_location& source = std::source_location::current()) { logger().write("WARN", message, source); }
inline void error(const std::string& message, const std::source_location& source = std::source_location::current()) { logger().write("ERROR", message, source); }

// Scoped per-thread context. Never put logging inside the block/vertex hot loop.
inline thread_local std::string operation_context;
struct Context {
    std::string previous;
    explicit Context(std::string value) : previous(operation_context) {
        operation_context = previous.empty() ? std::move(value) : previous + " " + value;
    }
    ~Context() { operation_context = std::move(previous); }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};
inline void contextual_error(const std::string& text,
                             const std::source_location& location = std::source_location::current()) {
    error(operation_context + " " + text, location);
}
inline void contextual_debug(const std::string& text,
                             const std::source_location& location = std::source_location::current()) {
    debug(operation_context + " " + text, location);
}

inline void output_result(const std::string& path, long long started, bool success,
                          const std::source_location& location = std::source_location::current()) {
    std::error_code ec;
    auto bytes = std::filesystem::file_size(path, ec);
    logger().write(success ? "INFO" : "ERROR", "operation=write_output path=" + path +
        " status=" + (success ? "success" : "error") + " duration_ms=" + std::to_string(now_ms() - started) +
        " bytes=" + (ec ? "unknown" : std::to_string(bytes)), location);
}

struct StageTimer {
    std::string name;
    std::chrono::steady_clock::time_point t0;
    bool silent;
    int exceptions;
    unsigned long long errors_before;
    std::source_location source;
    explicit StageTimer(const char* n, bool silent_ = false,
                        std::source_location location = std::source_location::current())
        : name(n), t0(std::chrono::steady_clock::now()), silent(silent_),
          exceptions(std::uncaught_exceptions()), errors_before(logger().errors()), source(location) {
        if (!silent) info("▶ operation=" + name + " status=running context=" + operation_context, source);
    }
    ~StageTimer() noexcept {
        if (silent) return;
        try {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            auto failed = std::uncaught_exceptions() > exceptions;
            const bool handled_error = logger().errors() > errors_before;
            logger().write(failed ? "ERROR" : (handled_error ? "WARN" : "INFO"), "■ operation=" + name +
                           (failed ? " status=error" : (handled_error ? " status=completed_with_errors" : " status=success")) + " duration_ms=" + std::to_string(ms) + " context=" + operation_context, source);
        } catch (...) {}
    }
    StageTimer(const StageTimer&) = delete;
    StageTimer& operator=(const StageTimer&) = delete;
};

// Capture legacy cout/cerr too; per-thread line buffers avoid interleaved lines.
// Logger writes to original streambufs, not these adapters (no recursion).
class LineBuffer : public std::streambuf {
    std::mutex mutex_;
    std::map<std::thread::id, std::string> pending_;
    const char* level_;
    void emit(const std::string& text) {
        std::string low = text;
        for (auto& character : low) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        const char* severity = level_;
        if (low.find("error") != std::string::npos || low.find("failed") != std::string::npos ||
            low.find("exception") != std::string::npos || text.find("错误") != std::string::npos || text.find("失败") != std::string::npos)
            severity = "ERROR";
        else if (low.find("warn") != std::string::npos || text.find("警告") != std::string::npos) severity = "WARN";
        logger().write(severity, text);
    }
    void append(const char* text, std::streamsize count) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& line = pending_[std::this_thread::get_id()];
        for (std::streamsize i = 0; i < count; ++i) {
            if (text[i] == '\n') {
                if (!line.empty()) emit(line);
                line.clear();
            } else if (text[i] != '\r') {
                line += text[i];
                if (line.size() >= 16384) { emit(line); line.clear(); }
            }
        }
    }
protected:
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        char value = traits_type::to_char_type(ch);
        append(&value, 1);
        return ch;
    }
    std::streamsize xsputn(const char* text, std::streamsize count) override { append(text, count); return count; }
    // cerr is unit-buffered: sync() follows each <<, not each complete line.
    int sync() override { return 0; }

public:
    explicit LineBuffer(const char* level) : level_(level) {}
    void drain() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& [thread, line] : pending_) if (!line.empty()) { emit(line); line.clear(); }
        } catch (...) {}
    }
};
class ConsoleCapture {
    LineBuffer out_{"INFO"}, err_{"WARN"};
    std::streambuf* old_out_;
    std::streambuf* old_err_;
public:
    ConsoleCapture() {
        logger(); // Snapshot original buffers before installing adapters.
        old_out_ = std::cout.rdbuf(&out_);
        old_err_ = std::cerr.rdbuf(&err_);
    }
    ~ConsoleCapture() {
        std::cout.rdbuf(old_out_);
        std::cerr.rdbuf(old_err_);
        out_.drain(); err_.drain();
    }
    ConsoleCapture(const ConsoleCapture&) = delete;
    ConsoleCapture& operator=(const ConsoleCapture&) = delete;
};
inline void environment(const std::string& executable) {
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec).string();
#ifdef _WIN32
    const char* system = "Windows";
#elif defined(__APPLE__)
    const char* system = "macOS";
#else
    const char* system = "Linux/Unix";
#endif
    info("operation=startup system=" + std::string(system) + " executable=" + executable +
         " cwd=" + cwd + " threads=" + std::to_string(std::thread::hardware_concurrency()) +
         " pointer_bits=" + std::to_string(sizeof(void*) * 8) + " compiler="
#ifdef _MSC_VER
         + std::to_string(_MSC_VER)
#else
         + std::string(__VERSION__)
#endif
         + " build=" + __DATE__ + " " + __TIME__ + " log=" + logger().path());
}
} // namespace CrafterLog
#define CRAFTER_STAGE_TIMER(varname, name) ::CrafterLog::StageTimer varname(name)
