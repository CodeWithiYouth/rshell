// md.cpp
#include "md.h"

#include <cwctype>

namespace rshell::md {

namespace {

bool startsWith(std::wstring_view text, std::wstring_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::wstring_view trimView(std::wstring_view text) {
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t' || text.back() == L'\r'))
        text.remove_suffix(1);
    return text;
}

bool isRule(std::wstring_view line) {
    if (line.size() < 3) return false;
    const wchar_t c = line.front();
    if (c != L'-' && c != L'*' && c != L'_') return false;
    for (const wchar_t ch : line) {
        if (ch != c) return false;
    }
    return true;
}

std::vector<std::wstring> splitCells(std::wstring_view line) {
    std::wstring_view body = trimView(line);
    if (!body.empty() && body.front() == L'|') body.remove_prefix(1);
    if (!body.empty() && body.back() == L'|') body.remove_suffix(1);

    std::vector<std::wstring> cells;
    std::wstring current;
    for (const wchar_t ch : body) {
        if (ch == L'|') {
            cells.push_back(std::wstring(trimView(current)));
            current.clear();
        } else {
            current += ch;
        }
    }
    cells.push_back(std::wstring(trimView(current)));
    return cells;
}

bool isTableRow(std::wstring_view line) {
    const std::wstring_view trimmed = trimView(line);
    return !trimmed.empty() && trimmed.front() == L'|';
}

// | --- | :---: | ---: | —— 允许 3 个以上的横线
bool isTableSeparator(std::wstring_view line) {
    if (!isTableRow(line)) return false;
    const std::vector<std::wstring> cells = splitCells(line);
    if (cells.empty()) return false;
    for (const std::wstring& cell : cells) {
        std::wstring_view value = trimView(cell);
        if (!value.empty() && value.front() == L':') value.remove_prefix(1);
        if (!value.empty() && value.back() == L':') value.remove_suffix(1);
        if (value.size() < 3) return false;
        for (const wchar_t ch : value) {
            if (ch != L'-') return false;
        }
    }
    return true;
}

Align alignOf(std::wstring_view cell) {
    const std::wstring_view value = trimView(cell);
    const bool left = !value.empty() && value.front() == L':';
    const bool right = !value.empty() && value.back() == L':';
    if (left && right) return Align::Center;
    if (right) return Align::Right;
    return Align::Left;
}

}  // namespace

std::vector<Run> parseInline(std::wstring_view text) {
    std::vector<Run> runs;
    Run current;
    auto flush = [&]() {
        if (!current.text.empty()) {
            runs.push_back(current);
            current.text.clear();
        }
    };

    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == L'*') {
            if (i + 1 < text.size() && text[i + 1] == L'*') {
                flush();
                current.bold = !current.bold;
                i += 2;
            } else {
                flush();
                current.italic = !current.italic;
                ++i;
            }
        } else if (text[i] == L'`') {
            flush();
            current.code = !current.code;
            ++i;
        } else {
            current.text += text[i++];
        }
    }
    flush();
    if (runs.empty()) runs.push_back(Run{});
    return runs;
}

std::vector<Block> parse(std::wstring_view source) {
    std::vector<std::wstring> lines;
    {
        std::size_t start = 0;
        while (start <= source.size()) {
            const std::size_t end = source.find(L'\n', start);
            if (end == std::wstring_view::npos) {
                lines.emplace_back(source.substr(start));
                break;
            }
            lines.emplace_back(source.substr(start, end - start));
            start = end + 1;
        }
    }

    std::vector<Block> blocks;
    std::size_t i = 0;
    while (i < lines.size()) {
        std::wstring_view line = lines[i];
        if (!line.empty() && line.back() == L'\r') line.remove_suffix(1);
        const std::wstring_view trimmed = trimView(line);

        if (trimmed.empty()) {
            ++i;
            continue;
        }

        // 代码块
        if (startsWith(trimmed, L"```")) {
            Block block;
            block.kind = BlockKind::Code;
            block.language = std::wstring(trimView(trimmed.substr(3)));
            ++i;
            std::wstring body;
            while (i < lines.size()) {
                std::wstring_view inner = lines[i];
                if (!inner.empty() && inner.back() == L'\r') inner.remove_suffix(1);
                if (startsWith(trimView(inner), L"```")) {
                    ++i;
                    break;
                }
                body += inner;
                body += L'\n';
                ++i;
            }
            if (!body.empty() && body.back() == L'\n') body.pop_back();
            block.text = std::move(body);
            blocks.push_back(std::move(block));
            continue;
        }

        // 表格：当前行是 | 开头，且下一行是分隔行
        if (isTableRow(trimmed) && i + 1 < lines.size()) {
            std::wstring_view next = lines[i + 1];
            if (!next.empty() && next.back() == L'\r') next.remove_suffix(1);
            if (isTableSeparator(next)) {
                Block block;
                block.kind = BlockKind::Table;
                block.table.hasHeader = true;

                const std::vector<std::wstring> headers = splitCells(trimmed);
                for (const std::wstring& cell : splitCells(next))
                    block.table.aligns.push_back(alignOf(cell));
                while (block.table.aligns.size() < headers.size())
                    block.table.aligns.push_back(Align::Left);

                std::vector<TableCell> headerRow;
                for (const std::wstring& cell : headers) {
                    TableCell entry;
                    entry.runs = parseInline(cell);
                    headerRow.push_back(std::move(entry));
                }
                block.table.rows.push_back(std::move(headerRow));

                i += 2;
                while (i < lines.size()) {
                    std::wstring_view row = lines[i];
                    if (!row.empty() && row.back() == L'\r') row.remove_suffix(1);
                    if (!isTableRow(trimView(row))) break;
                    const std::vector<std::wstring> cells = splitCells(trimView(row));
                    std::vector<TableCell> cellsOut;
                    for (std::size_t c = 0; c < headers.size(); ++c) {
                        TableCell entry;
                        entry.runs = parseInline(c < cells.size() ? cells[c] : std::wstring());
                        cellsOut.push_back(std::move(entry));
                    }
                    block.table.rows.push_back(std::move(cellsOut));
                    ++i;
                }
                blocks.push_back(std::move(block));
                continue;
            }
        }

        // 标题
        if (trimmed.front() == L'#') {
            std::size_t level = 0;
            while (level < trimmed.size() && trimmed[level] == L'#' && level < 6) ++level;
            if (level < trimmed.size() && trimmed[level] == L' ') {
                Block block;
                block.kind = BlockKind::Heading;
                block.level = static_cast<int>(level > 3 ? 3 : level);
                block.runs = parseInline(trimView(trimmed.substr(level + 1)));
                blocks.push_back(std::move(block));
                ++i;
                continue;
            }
        }

        // 分隔线（必须在列表之前判断，避免把 --- 当成列表）
        if (isRule(trimmed)) {
            Block block;
            block.kind = BlockKind::Rule;
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        // 引用
        if (startsWith(trimmed, L"> ")) {
            Block block;
            block.kind = BlockKind::Quote;
            block.runs = parseInline(trimView(trimmed.substr(2)));
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        // 无序列表
        if (startsWith(trimmed, L"- ") || startsWith(trimmed, L"* ") ||
            startsWith(trimmed, L"+ ")) {
            Block block;
            block.kind = BlockKind::Bullet;
            block.runs = parseInline(trimView(trimmed.substr(2)));
            blocks.push_back(std::move(block));
            ++i;
            continue;
        }

        // 有序列表：1. / 1)
        {
            std::size_t digits = 0;
            while (digits < trimmed.size() && iswdigit(trimmed[digits])) ++digits;
            if (digits > 0 && digits + 1 < trimmed.size() &&
                (trimmed[digits] == L'.' || trimmed[digits] == L')') &&
                trimmed[digits + 1] == L' ') {
                Block block;
                block.kind = BlockKind::Ordered;
                block.number = _wtoi(std::wstring(trimmed.substr(0, digits)).c_str());
                block.runs = parseInline(trimView(trimmed.substr(digits + 2)));
                blocks.push_back(std::move(block));
                ++i;
                continue;
            }
        }

        // 普通段落：单行一个块
        Block block;
        block.kind = BlockKind::Paragraph;
        block.runs = parseInline(trimmed);
        blocks.push_back(std::move(block));
        ++i;
    }

    return blocks;
}

}  // namespace rshell::md
