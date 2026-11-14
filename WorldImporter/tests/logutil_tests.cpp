// Standalone: c++ -std=c++20 -pthread -IWorldImporter WorldImporter/tests/logutil_tests.cpp -o /tmp/wi-log-test
#include "logutil.h"
#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    {
        CrafterLog::ConsoleCapture capture;
        CrafterLog::environment("logutil_tests");
        CrafterLog::info("explicit-source");
        CrafterLog::debug("debug-marker");
        { CrafterLog::StageTimer timer("normal-stage"); }
        try {
            CrafterLog::StageTimer timer("failing-stage");
            throw std::runtime_error("test-exception");
        } catch (const std::exception& error) { CrafterLog::error(error.what()); }
        {
            CrafterLog::Context batch("batch=7");
            {
                CrafterLog::Context worker("worker=2");
                CrafterLog::contextual_error("task=9 injected-error");
            }
            assert(CrafterLog::operation_context == "batch=7");
        }
        assert(CrafterLog::operation_context.empty());
        {
            CrafterLog::StageTimer timer("handled-stage");
            CrafterLog::error("handled-error-marker");
        }
        CrafterLog::output_result("missing-output-file", CrafterLog::now_ms(), false);
        std::cout << "legacy-stdout " << 42 << std::endl;
        std::cerr << "legacy-stderr " << 17 << std::endl;
        std::vector<std::thread> workers;
        for (int thread = 0; thread < 4; ++thread) {
            workers.emplace_back([thread] {
                for (int index = 0; index < 50; ++index)
                    std::cout << "worker=" << thread << " index=" << index << '\n';
            });
        }
        for (auto& worker : workers) worker.join();
        // Partial final line is drained when capture is destroyed.
        std::cout << "partial-final";
    }
    std::ifstream input(CrafterLog::logger().path());
    std::string content((std::istreambuf_iterator<char>(input)), {});
    assert(content.find("pid=") != std::string::npos);
    assert(content.find("thread=") != std::string::npos);
    assert(content.find("source=") != std::string::npos);
    assert(content.find("logutil_tests.cpp:") != std::string::npos);
    assert(content.find("operation=normal-stage status=success duration_ms=") != std::string::npos);
    assert(content.find("operation=failing-stage status=error duration_ms=") != std::string::npos);
    assert(content.find("legacy-stdout 42") != std::string::npos);
    assert(content.find("legacy-stderr 17") != std::string::npos);
    assert(content.find("partial-final") != std::string::npos);
    if (!CrafterLog::debug_enabled()) assert(content.find("debug-marker") == std::string::npos);
    for (int thread = 0; thread < 4; ++thread)
        for (int index = 0; index < 50; ++index)
            assert(content.find("worker=" + std::to_string(thread) + " index=" + std::to_string(index) + "\n") != std::string::npos);
    assert(content.find("batch=7 worker=2 task=9 injected-error") != std::string::npos);
    assert(content.find("operation=handled-stage status=completed_with_errors") != std::string::npos);
    assert(content.find("operation=write_output path=missing-output-file status=error") != std::string::npos);
    assert(CrafterLog::logger().errors() >= 4);
    // Exercise real rotation, including archive count.
    std::string payload(16384, 'x');
    for (int index = 0; index < 1100; ++index) CrafterLog::debug(payload);
    if (CrafterLog::debug_enabled()) {
        assert(std::filesystem::exists(CrafterLog::logger().path() + ".3"));
        assert(!std::filesystem::exists(CrafterLog::logger().path() + ".4"));
        assert(std::filesystem::file_size(CrafterLog::logger().path()) <= 5 * 1024 * 1024);
    }
    return 0;
}
