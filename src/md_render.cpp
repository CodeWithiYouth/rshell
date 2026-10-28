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

// 在 maxWidth 内最多能放几个字符（二分；避免把代理对劈开）
int fitChars(HDC dc, const std::wstring& text, std::size_t start, int maxWidth, HFONT font) {
    if (start >= text.size() || maxWidth <= 0) return 0;
    HGDIOBJ old = SelectObject(dc, font);
    int lo = 0;
    int hi = static_cast<int>(text.size() - start);
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        SIZE size{};
        GetTextExtentPoint32W(dc, text.data() + start, mid, &size);
        if (size.cx <= maxWidth)
            lo = mid;
        else
            hi = mid - 1;
    }
    SelectObject(dc, old);
    if (lo == 0) lo = 1;  // 至少放一个字符，避免死循环
    if (start + static_cast<std::size_t>(lo) < text.size()) {
        const wchar_t next = text[start + static_cast<std::size_t>(lo)];
        if (next >= 0xDC00 && next <= 0xDFFF && lo > 1) --lo;  // 别切开代理对
    }
    return lo;
}

void drawChunk(HDC dc, const std::wstring& text, std::size_t start, int count, int x, int y,
               HFONT font, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    TextOutW(dc, x, y, text.data() + start, count);
    SelectObject(dc, old);
}

// 行内混排 + 折行；返回占用高度
int inlineLayout(HDC dc, int x, int y, int width, const std::vector<Run>& runs, const Style& style,
                 COLORREF color, HFONT overrideFont, int lineHeight, bool draw) {
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
            if (draw) drawChunk(dc, run.text, pos, count, cx, cy, font, runColor);
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

void drawTable(HDC dc, int x, int y, int width, const Table& table, const Style& style, bool draw,
               int& height) {
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
            const int cellHeight = inlineLayout(dc, 0, 0, std::max(1, widths[c] - padX * 2),
                                                table.rows[r][c].runs, style,
                                                header ? style.text : style.text, overrideFont,
                                                style.bodyHeight, false);
            rowHeight = std::max(rowHeight, cellHeight);
        }
        rowHeight += padY * 2;

        if (draw) {
            if (header) {
                HBRUSH brush = CreateSolidBrush(style.codeBg);
                RECT rc{x, cy, x + total, cy + rowHeight};
                FillRect(dc, &rc, brush);
                DeleteObject(brush);
            }
            int cx = x;
            for (std::size_t c = 0; c < columns; ++c) {
                const int inner = std::max(1, widths[c] - padX * 2);
                int offset = 0;
                const int contentWidth =
                    runsWidth(dc, table.rows[r][c].runs, style, overrideFont);
                if (contentWidth < inner) {
                    switch (c < table.aligns.size() ? table.aligns[c] : Align::Left) {
                        case Align::Center: offset = (inner - contentWidth) / 2; break;
                        case Align::Right: offset = inner - contentWidth; break;
                        default: break;
                    }
                }
                inlineLayout(dc, cx + padX + offset, cy + padY, inner, table.rows[r][c].runs,
                             style, header ? style.text : style.text, overrideFont,
                             style.bodyHeight, true);
                cx += widths[c];
            }
            // 表格线
            HPEN pen = CreatePen(PS_SOLID, 1, style.border);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, x, cy + rowHeight, nullptr);
            LineTo(dc, x + total, cy + rowHeight);
            int lineX = x;
            for (std::size_t c = 0; c + 1 < columns; ++c) {
                lineX += widths[c];
                MoveToEx(dc, lineX, cy, nullptr);
                LineTo(dc, lineX, cy + rowHeight);
            }
            SelectObject(dc, oldPen);
            DeleteObject(pen);
        }
        cy += rowHeight;
    }
    height = cy - y;
}

}  // namespace

int layout(HDC dc, int x, int y, int width, const std::wstring& source, const Style& style,
           bool draw) {
    const std::vector<Block> blocks = parse(source);
    int cy = y;

    for (const Block& block : blocks) {
        switch (block.kind) {
            case BlockKind::Paragraph:
                cy += inlineLayout(dc, x, cy, width, block.runs, style, style.text, nullptr,
                                   style.bodyHeight, draw);
                cy += 6;
                break;

            case BlockKind::Heading: {
                const int index = std::clamp(block.level, 1, 3) - 1;
                HFONT font = style.heading[index] ? style.heading[index] : style.bold;
                const int lineHeight = fontHeight(dc, font);
                cy += 4;
                cy += inlineLayout(dc, x, cy, width, block.runs, style, style.text, font,
                                   lineHeight, draw);
                cy += 10;
                break;
            }

            case BlockKind::Bullet:
                if (draw) {
                    HGDIOBJ old = SelectObject(dc, style.normal);
                    SetTextColor(dc, style.dim);
                    TextOutW(dc, x + 2, cy, L"\x2022", 1);
                    SelectObject(dc, old);
                }
                cy += inlineLayout(dc, x + 16, cy, width - 16, block.runs, style, style.text,
                                   nullptr, style.bodyHeight, draw);
                cy += 4;
                break;

            case BlockKind::Ordered: {
                const std::wstring marker = std::to_wstring(block.number) + L".";
                if (draw) {
                    HGDIOBJ old = SelectObject(dc, style.normal);
                    SetTextColor(dc, style.dim);
                    TextOutW(dc, x + 2, cy, marker.c_str(), static_cast<int>(marker.size()));
                    SelectObject(dc, old);
                }
                cy += inlineLayout(dc, x + 26, cy, width - 26, block.runs, style, style.text,
                                   nullptr, style.bodyHeight, draw);
                cy += 4;
                break;
            }

            case BlockKind::Quote:
                if (draw) {
                    HBRUSH brush = CreateSolidBrush(style.border);
                    RECT bar{x, cy, x + 3, cy + style.bodyHeight};
                    FillRect(dc, &bar, brush);
                    DeleteObject(brush);
                }
                cy += inlineLayout(dc, x + 12, cy, width - 12, block.runs, style, style.dim,
                                   nullptr, style.bodyHeight, draw);
                cy += 6;
                break;

            case BlockKind::Rule:
                if (draw) {
                    HPEN pen = CreatePen(PS_SOLID, 1, style.border);
                    HGDIOBJ oldPen = SelectObject(dc, pen);
                    MoveToEx(dc, x, cy + 4, nullptr);
                    LineTo(dc, x + width, cy + 4);
                    SelectObject(dc, oldPen);
                    DeleteObject(pen);
                }
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
                if (draw) {
                    RECT rc{x, cy, x + width, cy + bodyHeight + padding * 2};
                    HBRUSH brush = CreateSolidBrush(style.codeBg);
                    FillRect(dc, &rc, brush);
                    DeleteObject(brush);
                    HPEN pen = CreatePen(PS_SOLID, 1, style.border);
                    HGDIOBJ oldPen = SelectObject(dc, pen);
                    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                    Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
                    SelectObject(dc, oldBrush);
                    SelectObject(dc, oldPen);
                    DeleteObject(pen);

                    HGDIOBJ oldFont = SelectObject(dc, style.mono ? style.mono : style.normal);
                    SetTextColor(dc, style.codeFg);
                    int lineY = cy + padding;
                    for (const std::wstring& line : lines) {
                        TextOutW(dc, x + padding, lineY, line.c_str(),
                                 static_cast<int>(line.size()));
                        lineY += monoHeight;
                    }
                    SelectObject(dc, oldFont);
                }
                cy += bodyHeight + padding * 2 + 8;
                break;
            }

            case BlockKind::Table: {
                int tableHeight = 0;
                drawTable(dc, x, cy, width, block.table, style, draw, tableHeight);
                cy += tableHeight + 8;
                break;
            }
        }
    }

    return cy - y;
}

}  // namespace rshell::md
