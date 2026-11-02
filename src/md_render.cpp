// md_render.cpp
#include "md_render.h"

#include <algorithm>

namespace rshell::md {

namespace {

int textWidth(HDC dc, const std::wstring& text, HFONT font) {
    if (text.empty() || !font) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    return size.cx;
}

int fontHeight(HDC dc, HFONT font) {
    if (!font) return 16;
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    return metrics.tmHeight > 0 ? metrics.tmHeight : 16;
}

HFONT fontFor(const Style& style, const Run& run, HFONT overrideFont) {
    if (run.code) return style.mono ? style.mono : style.normal;
    if (overrideFont) return overrideFont;
    if (run.bold) return style.bold ? style.bold : style.normal;
    if (run.italic) return style.italic ? style.italic : style.normal;
    return style.normal;
}

// 在 maxWidth 内最多能放几个字符。
// 用 GetTextExtentExPointW 一次问出来（原来的二分要调 10 次左右，
// 中文长段落里这一项就是排版的大头）。
int fitChars(HDC dc, const std::wstring& text, std::size_t start, int maxWidth, HFONT font) {
    if (start >= text.size() || maxWidth <= 0) return 0;
    const int len = static_cast<int>(text.size() - start);
    HGDIOBJ old = SelectObject(dc, font);
    int fit = 0;
    SIZE size{};
    const BOOL ok =
        GetTextExtentExPointW(dc, text.data() + start, len, maxWidth, &fit, nullptr, &size);
    SelectObject(dc, old);
    if (!ok) {
        // 极端情况（字体没选上）退回逐字判断，至少不崩
        fit = std::min(len, 1);
    }
    if (fit <= 0) fit = 1;  // 至少放一个字符，避免死循环
    if (start + static_cast<std::size_t>(fit) < text.size()) {
        const wchar_t next = text[start + static_cast<std::size_t>(fit)];
        if (next >= 0xDC00 && next <= 0xDFFF && fit > 1) --fit;  // 别切开代理对
    }
    return fit;
}

using Command = LayoutCache::Command;

void emitText(std::vector<Command>& out, int x, int y, std::wstring text, HFONT font,
              COLORREF color) {
    if (text.empty() || !font) return;
    Command cmd;
    cmd.kind = Command::Kind::Text;
    cmd.x = x;
    cmd.y = y;
    cmd.text = std::move(text);
    cmd.font = font;
    cmd.color = color;
    out.push_back(std::move(cmd));
}

void emitFill(std::vector<Command>& out, int x, int y, int x2, int y2, COLORREF color) {
    Command cmd;
    cmd.kind = Command::Kind::Fill;
    cmd.x = x;
    cmd.y = y;
    cmd.x2 = x2;
    cmd.y2 = y2;
    cmd.color = color;
    out.push_back(std::move(cmd));
}

void emitFrame(std::vector<Command>& out, int x, int y, int x2, int y2, COLORREF border) {
    Command cmd;
    cmd.kind = Command::Kind::Frame;
    cmd.x = x;
    cmd.y = y;
    cmd.x2 = x2;
    cmd.y2 = y2;
    cmd.border = border;
    out.push_back(std::move(cmd));
}

void emitLine(std::vector<Command>& out, int x, int y, int x2, int y2, COLORREF border) {
    Command cmd;
    cmd.kind = Command::Kind::Line;
    cmd.x = x;
    cmd.y = y;
    cmd.x2 = x2;
    cmd.y2 = y2;
    cmd.border = border;
    out.push_back(std::move(cmd));
}

// 行内混排 + 折行；把每段文字拆成 Text 命令，返回占用高度
int emitInline(HDC dc, int x, int y, int width, const std::vector<Run>& runs, const Style& style,
               COLORREF color, HFONT overrideFont, int lineHeight, std::vector<Command>& out,
               bool emit = true) {
    if (width <= 0) width = 1;
    int cx = x;
    int cy = y;
    bool any = false;

    for (const Run& run : runs) {
        if (run.text.empty()) continue;
        const HFONT font = fontFor(style, run, overrideFont);
        const COLORREF runColor = run.code ? style.codeFg : color;

        std::size_t pos = 0;
        while (pos < run.text.size()) {
            const int avail = x + width - cx;
            if (avail <= 0) {
                cy += lineHeight;
                cx = x;
                continue;
            }
            const int count = fitChars(dc, run.text, pos, avail, font);
            if (count <= 0) break;
            if (emit)
                emitText(out, cx, cy, run.text.substr(pos, static_cast<std::size_t>(count)), font,
                         runColor);
            SIZE size{};
            {
                HGDIOBJ old = SelectObject(dc, font);
                GetTextExtentPoint32W(dc, run.text.data() + pos, count, &size);
                SelectObject(dc, old);
            }
            cx += size.cx;
            pos += static_cast<std::size_t>(count);
            any = true;
            if (pos < run.text.size()) {
                cy += lineHeight;
                cx = x;
            }
        }
    }

    if (!any) return lineHeight;
    return cy + lineHeight - y;
}

// 单个 run 合计宽度（表格算自然列宽用）
int runsWidth(HDC dc, const std::vector<Run>& runs, const Style& style, HFONT overrideFont) {
    int total = 0;
    for (const Run& run : runs) {
        if (run.text.empty()) continue;
        total += textWidth(dc, run.text, fontFor(style, run, overrideFont));
    }
    return total;
}

void emitTable(HDC dc, int x, int y, int width, const Table& table, const Style& style,
               std::vector<Command>& out, int& height) {
    const int padX = 8;
    const int padY = 4;
    const int minColumn = 40;
    const std::size_t columns = table.rows.empty() ? 0 : table.rows.front().size();
    if (columns == 0) {
        height = 0;
        return;
    }

    // 自然列宽
    std::vector<int> widths(columns, minColumn);
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        const HFONT overrideFont = (r == 0 && table.hasHeader) ? style.bold : nullptr;
        for (std::size_t c = 0; c < columns; ++c) {
            const std::vector<Run>& runs = table.rows[r][c].runs;
            const int natural = runsWidth(dc, runs, style, overrideFont) + padX * 2;
            widths[c] = std::max(widths[c], std::min(natural, width));
        }
    }

    int total = 0;
    for (const int w : widths) total += w;
    if (total > width) {
        // 按比例压缩，下限 40；压缩后仍超就把超出量从最宽的列扣掉
        int scaled = 0;
        for (int& w : widths) {
            w = std::max(minColumn, w * width / total);
            scaled += w;
        }
        int excess = scaled - width;
        while (excess > 0) {
            int* widest = nullptr;
            for (int& w : widths)
                if (!widest || w > *widest) widest = &w;
            if (!widest || *widest <= minColumn) break;
            const int cut = std::min(excess, *widest - minColumn);
            *widest -= cut;
            excess -= cut;
        }
    }

    total = 0;
    for (const int w : widths) total += w;

    int cy = y;
    for (std::size_t r = 0; r < table.rows.size(); ++r) {
        const bool header = (r == 0 && table.hasHeader);
        const HFONT overrideFont = header ? style.bold : nullptr;

        // 行高 = 单元格换行后的最大高度
        int rowHeight = 0;
        for (std::size_t c = 0; c < columns; ++c) {
            const int cellHeight = emitInline(dc, 0, cy, std::max(1, widths[c] - padX * 2),
                                              table.rows[r][c].runs, style, style.text, overrideFont,
                                              style.bodyHeight, out, false);
            rowHeight = std::max(rowHeight, cellHeight);
        }
        rowHeight += padY * 2;

        if (header) emitFill(out, x, cy, x + total, cy + rowHeight, style.codeBg);

        int cx = x;
        for (std::size_t c = 0; c < columns; ++c) {
            const int inner = std::max(1, widths[c] - padX * 2);
            int offset = 0;
            const int contentWidth = runsWidth(dc, table.rows[r][c].runs, style, overrideFont);
            if (contentWidth < inner) {
                switch (c < table.aligns.size() ? table.aligns[c] : Align::Left) {
                    case Align::Center: offset = (inner - contentWidth) / 2; break;
                    case Align::Right: offset = inner - contentWidth; break;
                    default: break;
                }
            }
            emitInline(dc, cx + padX + offset, cy + padY, inner, table.rows[r][c].runs, style,
                       style.text, overrideFont, style.bodyHeight, out);
            cx += widths[c];
        }
        // 表格线
        emitLine(out, x, cy + rowHeight, x + total, cy + rowHeight, style.border);
        int lineX = x;
        for (std::size_t c = 0; c + 1 < columns; ++c) {
            lineX += widths[c];
            emitLine(out, lineX, cy, lineX, cy + rowHeight, style.border);
        }
        cy += rowHeight;
    }
    height = cy - y;
}

// 一块的排版：把绘制内容写进 out（坐标相对块起点），返回块高
int emitBlock(HDC dc, int width, const Block& block, const Style& style,
              std::vector<Command>& out) {
    int cy = 0;
    switch (block.kind) {
        case BlockKind::Paragraph:
            cy += emitInline(dc, 0, cy, width, block.runs, style, style.text, nullptr,
                             style.bodyHeight, out);
            cy += 6;
            break;

        case BlockKind::Heading: {
            const int index = std::clamp(block.level, 1, 3) - 1;
            HFONT font = style.heading[index] ? style.heading[index] : style.bold;
            const int lineHeight = fontHeight(dc, font);
            cy += 4;
            cy += emitInline(dc, 0, cy, width, block.runs, style, style.text, font, lineHeight,
                             out);
            cy += 10;
            break;
        }

        case BlockKind::Bullet:
            emitText(out, 2, cy, std::wstring(1, L'\x2022'), style.normal, style.dim);
            cy += emitInline(dc, 16, cy, width - 16, block.runs, style, style.text, nullptr,
                             style.bodyHeight, out);
            cy += 4;
            break;

        case BlockKind::Ordered: {
            const std::wstring marker = std::to_wstring(block.number) + L".";
            emitText(out, 2, cy, marker, style.normal, style.dim);
            cy += emitInline(dc, 26, cy, width - 26, block.runs, style, style.text, nullptr,
                             style.bodyHeight, out);
            cy += 4;
            break;
        }

        case BlockKind::Quote:
            cy += emitInline(dc, 12, cy, width - 12, block.runs, style, style.dim, nullptr,
                             style.bodyHeight, out);
            emitFill(out, 0, 0, 3, cy, style.border);
            cy += 6;
            break;

        case BlockKind::Rule:
            emitLine(out, 0, cy + 4, width, cy + 4, style.border);
            cy += 10;
            break;

        case BlockKind::Code: {
            const int monoHeight = fontHeight(dc, style.mono ? style.mono : style.normal);
            std::vector<std::wstring> lines;
            {
                std::size_t start = 0;
                while (start <= block.text.size()) {
                    const std::size_t end = block.text.find(L'\n', start);
                    if (end == std::wstring::npos) {
                        lines.emplace_back(block.text.substr(start));
                        break;
                    }
                    lines.emplace_back(block.text.substr(start, end - start));
                    start = end + 1;
                }
            }
            if (lines.empty()) lines.emplace_back();
            const int padding = 8;
            const int bodyHeight = static_cast<int>(lines.size()) * monoHeight;
            emitFill(out, 0, cy, width, cy + bodyHeight + padding * 2, style.codeBg);
            emitFrame(out, 0, cy, width, cy + bodyHeight + padding * 2, style.border);
            int lineY = cy + padding;
            for (const std::wstring& line : lines) {
                emitText(out, padding, lineY, line, style.mono ? style.mono : style.normal,
                         style.codeFg);
                lineY += monoHeight;
            }
            cy += bodyHeight + padding * 2 + 8;
            break;
        }

        case BlockKind::Table: {
            int tableHeight = 0;
            emitTable(dc, 0, cy, width, block.table, style, out, tableHeight);
            cy += tableHeight + 8;
            break;
        }
    }
    return cy;
}

bool sameRuns(const std::vector<Run>& a, const std::vector<Run>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].text != b[i].text || a[i].bold != b[i].bold || a[i].italic != b[i].italic ||
            a[i].code != b[i].code)
            return false;
    }
    return true;
}

bool sameTable(const Table& a, const Table& b) {
    if (a.hasHeader != b.hasHeader || a.aligns != b.aligns) return false;
    if (a.rows.size() != b.rows.size()) return false;
    for (std::size_t r = 0; r < a.rows.size(); ++r) {
        if (a.rows[r].size() != b.rows[r].size()) return false;
        for (std::size_t c = 0; c < a.rows[r].size(); ++c)
            if (!sameRuns(a.rows[r][c].runs, b.rows[r][c].runs)) return false;
    }
    return true;
}

// 只有内容和类型都一样才能复用上一块的排版结果
bool sameBlock(const Block& a, const Block& b) {
    return a.kind == b.kind && a.level == b.level && a.number == b.number &&
           a.language == b.language && a.text == b.text && sameRuns(a.runs, b.runs) &&
           sameTable(a.table, b.table);
}

}  // namespace

int LayoutCache::build(HDC dc, int width, const std::wstring& source, const Style& style) {
    if (width <= 0) width = 1;
    // 内容、宽度都没变：直接复用（这是「一帧只排一次」的关键）
    if (width == width_ && source == source_) {
        rebuilt_ = 0;
        return height_;
    }

    std::vector<Block> blocks = parse(source);
    std::vector<BlockCache> next;
    next.reserve(blocks.size());

    int total = 0;
    std::size_t rebuilt = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        BlockCache cache;
        const bool reuse = i < blocks_.size() && width == width_ &&
                           sameBlock(blocks[i], blocks_[i].block);
        if (reuse) {
            cache = std::move(blocks_[i]);  // 复用命令行
        } else {
            cache.block = blocks[i];
            cache.height = emitBlock(dc, width, blocks[i], style, cache.commands);
            ++rebuilt;
        }
        total += cache.height;
        next.push_back(std::move(cache));
    }

    blocks_ = std::move(next);
    width_ = width;
    source_ = source;
    height_ = total;
    rebuilt_ = rebuilt;
    return height_;
}

void LayoutCache::draw(HDC dc, int x, int y, const RECT* clip) const {
    int blockTop = y;
    for (const BlockCache& block : blocks_) {
        // 整块在可视区之外：直接跳过（长回答只有最后一屏需要画）
        if (clip && (blockTop + block.height <= clip->top || blockTop >= clip->bottom)) {
            blockTop += block.height;
            continue;
        }
        for (const Command& cmd : block.commands) {
            switch (cmd.kind) {
                case Command::Kind::Text: {
                    if (cmd.text.empty()) break;
                    HGDIOBJ old = SelectObject(dc, cmd.font);
                    SetTextColor(dc, cmd.color);
                    TextOutW(dc, x + cmd.x, blockTop + cmd.y, cmd.text.c_str(),
                             static_cast<int>(cmd.text.size()));
                    SelectObject(dc, old);
                    break;
                }
                case Command::Kind::Fill: {
                    RECT rc{x + cmd.x, blockTop + cmd.y, x + cmd.x2, blockTop + cmd.y2};
                    HBRUSH brush = CreateSolidBrush(cmd.color);
                    FillRect(dc, &rc, brush);
                    DeleteObject(brush);
                    break;
                }
                case Command::Kind::Frame: {
                    RECT rc{x + cmd.x, blockTop + cmd.y, x + cmd.x2, blockTop + cmd.y2};
                    HPEN pen = CreatePen(PS_SOLID, 1, cmd.border);
                    HGDIOBJ oldPen = SelectObject(dc, pen);
                    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dc, oldBrush);
                    SelectObject(dc, oldPen);
                    DeleteObject(pen);
                    break;
                }
                case Command::Kind::Line: {
                    HPEN pen = CreatePen(PS_SOLID, 1, cmd.border);
                    HGDIOBJ oldPen = SelectObject(dc, pen);
                    MoveToEx(dc, x + cmd.x, blockTop + cmd.y, nullptr);
                    LineTo(dc, x + cmd.x2, blockTop + cmd.y2);
                    SelectObject(dc, oldPen);
                    DeleteObject(pen);
                    break;
                }
            }
        }
        blockTop += block.height;
    }
}

int layout(HDC dc, int x, int y, int width, const std::wstring& source, const Style& style,
           bool draw) {
    LayoutCache cache;
    const int height = cache.build(dc, width, source, style);
    if (draw) cache.draw(dc, x, y);
    return height;
}

}  // namespace rshell::md
