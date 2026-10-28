// md.h - 极简 Markdown 解析：标题、粗体/斜体/行内代码、列表、引用、代码块、分隔线、表格
//
// 只覆盖 AI 回答里常见的子集，不做图片、链接跳转、嵌套列表和 HTML。
// 解析层不依赖 GDI，便于单测。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace rshell::md {

struct Run {
    std::wstring text;
    bool bold = false;
    bool italic = false;
    bool code = false;
};

enum class Align { Left, Center, Right };

struct TableCell {
    std::vector<Run> runs;
};

struct Table {
    std::vector<Align> aligns;
    std::vector<std::vector<TableCell>> rows;  // rows[0] 是表头（有分隔行时）
    bool hasHeader = false;
};

enum class BlockKind {
    Paragraph,
    Heading,
    Bullet,
    Ordered,
    Code,
    Quote,
    Rule,
    Table,
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int level = 1;       // 标题层级 1~3
    int number = 1;      // 有序列表序号
    std::wstring language;  // 代码块语言标记
    std::wstring text;      // 代码块原文
    std::vector<Run> runs;
    Table table;
};

std::vector<Block> parse(std::wstring_view source);

// 行内解析（测试用）
std::vector<Run> parseInline(std::wstring_view text);

}  // namespace rshell::md
