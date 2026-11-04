// md_render.h - Markdown 的 GDI 排版（块级缓存 + 显示列表 + 绘制）
//
// 只接收字体（外部创建）与 HDC/宽度/颜色，不持有界面状态，方便复用和单测。
// 同一段文本：
//   - build() 解析 + 折行，把结果存成绘制命令（只在内容/宽度/字体变化时重做）
//   - draw() 直接重放命令，不做任何测量
// 流式追加时只有变化的块会重新折行，长回答也不会每帧重排全文。
#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "md.h"

namespace rshell::md {

struct Style {
    HFONT normal = nullptr;
    HFONT bold = nullptr;
    HFONT italic = nullptr;
    HFONT mono = nullptr;
    HFONT heading[3]{};  // 1~3 级标题字号（缺省回退 bold）
    COLORREF text = RGB(240, 240, 240);
    COLORREF dim = RGB(150, 150, 160);
    COLORREF accent = RGB(140, 180, 255);
    COLORREF codeFg = RGB(190, 215, 195);
    COLORREF codeBg = RGB(18, 24, 20);
    COLORREF border = RGB(60, 72, 88);
    int bodyHeight = 16;  // normal 的单行高
};

// 排好版的一段 Markdown。内容不变时 build() 直接返回上次的高度，
// draw() 只重放命令；内容变化时只重排发生变化的块。
class LayoutCache {
public:
    // 增量构建，返回总高度（像素）
    int build(HDC dc, int width, const std::wstring& source, const Style& style);
    // 重放：不做任何 GDI 测量，只画。clip 是可视区（客户区坐标），
    // 长回答滚出屏幕的部分直接跳过，不再交给 GDI。
    void draw(HDC dc, int x, int y, const RECT* clip = nullptr) const;
    int height() const { return height_; }
    // 上一次 build 实际重新折行了几块（测试/性能观测用）
    std::size_t lastRebuiltBlocks() const { return rebuilt_; }
    void clear() { blocks_.clear(); height_ = 0; width_ = -1; source_.clear(); visualLines_.clear(); codeBlocks_.clear(); }

    // 一行“看得见的文本”（相对排版左上角），用于鼠标选区与复制。
    struct VisualLine {
        int x = 0;
        int y = 0;
        std::wstring text;
        HFONT font = nullptr;   // 该行主要使用的字体（代码块是 mono）
        bool hardBreak = true;  // true = 这里确实是换行（复制时补 \n）
    };
    const std::vector<VisualLine>& visualLines() const { return visualLines_; }

    // 代码块（用于右上角的“复制”按钮）
    struct CodeBlock {
        int y = 0;
        int height = 0;
        std::wstring code;
    };
    const std::vector<CodeBlock>& codeBlocks() const { return codeBlocks_; }

    // 绘制命令（块内相对坐标）；draw() 只重放它们，不再测量
    struct Command {
        enum class Kind { Text, Fill, Frame, Line } kind = Kind::Text;
        int x = 0;
        int y = 0;
        int x2 = 0;
        int y2 = 0;
        std::wstring text;
        HFONT font = nullptr;
        COLORREF color = 0;
        COLORREF border = 0;
    };

private:
    struct BlockCache {
        Block block;                 // 用于下一次比较（是否同一块）
        int height = 0;
        std::vector<Command> commands;
    };

    std::vector<BlockCache> blocks_;
    int height_ = 0;
    int width_ = -1;
    std::wstring source_;
    std::size_t rebuilt_ = 0;
    std::vector<VisualLine> visualLines_;
    std::vector<CodeBlock> codeBlocks_;
    void rebuildDerived();
};

// 兼容旧调用：一次排版（必要时绘制），返回高度。
int layout(HDC dc, int x, int y, int width, const std::wstring& source, const Style& style,
           bool draw);

}  // namespace rshell::md
