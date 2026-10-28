// md_render.h - Markdown 的 GDI 排版（块级布局 + 行内混排 + 表格）
//
// 与解析层分开：这里只依赖 HDC/字体/颜色，方便面板复用测量与绘制（同一个函数，
// draw=false 时只算高度，保证两边结果一致）。
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "md.h"

namespace rshell::md {

struct Style {
    HFONT normal = nullptr;
    HFONT bold = nullptr;
    HFONT italic = nullptr;
    HFONT mono = nullptr;
    HFONT heading[3]{};  // 1~3 级标题字号，空则退回 bold
    COLORREF text = RGB(240, 240, 240);
    COLORREF dim = RGB(150, 150, 160);
    COLORREF accent = RGB(140, 180, 255);
    COLORREF codeFg = RGB(190, 215, 195);
    COLORREF codeBg = RGB(18, 24, 20);
    COLORREF border = RGB(60, 72, 88);
    int bodyHeight = 16;  // normal 字体的行高
};

// 在 (x, y) 起排版；返回占用高度。draw=false 时不写像素，只测高度。
int layout(HDC dc, int x, int y, int width, const std::wstring& source, const Style& style,
           bool draw);

}  // namespace rshell::md
