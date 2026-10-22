// replay.cpp - 离屏回放：把抓到的原始输出喂给 Terminal 并打印文本网格
// 用法：rshell_term_replay <dump 文件> [cols] [rows]
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "terminal.h"

namespace {

std::string utf8FromCell(const rshell::Cell& c) {
    std::string out;
    char32_t ch = c.ch == 0 ? U' ' : c.ch;
    if (ch < 0x80) {
        out.push_back(static_cast<char>(ch));
    } else if (ch < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else if (ch < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法: rshell_term_replay <dump 文件> [cols] [rows]\n";
        return 2;
    }
    const int cols = argc > 2 ? std::atoi(argv[2]) : 80;
    const int rows = argc > 3 ? std::atoi(argv[3]) : 24;

    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::cerr << "无法打开 " << argv[1] << "\n";
        return 2;
    }
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    rshell::Terminal term(cols, rows);
    term.feed(data);
    for (std::size_t i = 0; i < term.lineCount(); ++i) {
        std::string line;
        for (const rshell::Cell& c : term.lineAt(i)) {
            if (c.flags & rshell::kFlagWideTail) continue;
            line += utf8FromCell(c);
        }
        std::printf("%3zu|%s|\n", i, line.c_str());
    }
    std::printf("cursor=%d,%d\n", term.cursorRow(), term.cursorCol());
    // 属性汇总：空白处被画上下划线/加粗这类问题只靠文本回放看不出来
    std::size_t underline = 0, bold = 0, wide = 0;
    for (std::size_t i = 0; i < term.lineCount(); ++i) {
        for (const rshell::Cell& c : term.lineAt(i)) {
            if (c.flags & rshell::kFlagUnderline) ++underline;
            if (c.flags & rshell::kFlagBold) ++bold;
            if (c.flags & rshell::kFlagWide) ++wide;
        }
    }
    std::printf("flags: underline=%zu bold=%zu wide=%zu\n", underline, bold, wide);
    return 0;
}
