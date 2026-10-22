// progress.cpp
#include "progress.h"

#include <cstdio>
#include <iostream>

#include "util.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace rshell {

namespace {

bool stderrIsTty() {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

}  // namespace

ProgressBar::ProgressBar(std::string label, std::uint64_t total, bool enabled)
    : label_(std::move(label)), total_(total) {
    enabled_ = enabled && stderrIsTty();
    startTime_ = monotonicSeconds();
    lastRender_ = startTime_;
}

ProgressBar::~ProgressBar() {
    if (enabled_ && drawn_ && !finished_) {
        std::cerr << "\r" << std::string(96, ' ') << "\r" << std::flush;
    }
}

std::string ProgressBar::line(std::uint64_t done) const {
    return label_ + "  " + formatProgress(done, total_, rate_);
}

void ProgressBar::render(std::uint64_t done, bool newline) {
    std::string text = line(done);
    std::cerr << "\r" << text;
    if (text.size() < 96) std::cerr << std::string(96 - text.size(), ' ');
    std::cerr << (newline ? "\n" : "") << std::flush;
    drawn_ = true;
}

void ProgressBar::update(std::uint64_t done) {
    if (finished_) return;
    double now = monotonicSeconds();
    elapsed_ = now - startTime_;
    if (elapsed_ > 0.0) rate_ = static_cast<double>(done) / elapsed_;
    if (!enabled_) return;
    if (now - lastRender_ < 0.1 && done < total_) return;
    lastRender_ = now;
    render(done, false);
}

void ProgressBar::finish(std::uint64_t done, bool success) {
    if (finished_) return;
    finished_ = true;
    double now = monotonicSeconds();
    elapsed_ = now - startTime_;
    if (elapsed_ > 0.0) rate_ = static_cast<double>(done) / elapsed_;
    if (!enabled_) return;
    if (success) {
        render(done, true);
    } else {
        std::cerr << "\r" << std::string(96, ' ') << "\r" << std::flush;
        drawn_ = false;
    }
}

}  // namespace rshell
