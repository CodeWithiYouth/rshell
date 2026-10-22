// progress.h - 传输进度显示
#pragma once

#include <cstdint>
#include <string>

namespace rshell {

// 单行进度条：显示百分比、已传输量、速度。
// 输出到 stderr，避免污染 stdout 上的数据流；stderr 不是终端时不输出。
class ProgressBar {
public:
    // label 会显示在进度条左侧，例如 "put foo.txt -> /tmp/foo.txt"
    ProgressBar(std::string label, std::uint64_t total, bool enabled = true);
    ~ProgressBar();

    ProgressBar(const ProgressBar&) = delete;
    ProgressBar& operator=(const ProgressBar&) = delete;

    // 传输过程中调用（内部按 ~100ms 限流刷新）
    void update(std::uint64_t done);

    // 结束：成功时补一个换行，未成功时清掉当前行
    void finish(std::uint64_t done, bool success = true);

    double averageRate() const { return rate_; }
    double elapsed() const { return elapsed_; }

private:
    void render(std::uint64_t done, bool newline);
    std::string line(std::uint64_t done) const;

    std::string label_;
    std::uint64_t total_ = 0;
    double startTime_ = 0;
    double lastRender_ = 0;
    double rate_ = 0;
    double elapsed_ = 0;
    bool enabled_ = false;
    bool finished_ = false;
    bool drawn_ = false;
};

}  // namespace rshell
