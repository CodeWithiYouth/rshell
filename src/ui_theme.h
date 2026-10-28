// ui_theme.h - GUI 共用的配色与颜色工具
//
// 从 gui_main.cpp 抽出，供 AI 面板等独立编译单元复用同一套颜色，
// 避免两处各写一份色值后逐渐漂移。
#pragma once

#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#endif

// 32 位 0xRRGGBB -> COLORREF
inline COLORREF toColorRef(std::uint32_t c) {
    return RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
}

// 界面配色：深石板灰底色，亮蓝用于文字/焦点，深蓝实心按钮保证白字对比度。
// 全部是编译期常量，换主题只动这里。
namespace rshell::ui {
using std::uint32_t;

constexpr uint32_t bgApp = 0x0C121B;       // 窗口底色
constexpr uint32_t bgPanel = 0x141E2B;     // 侧栏 / 标题栏
constexpr uint32_t bgCard = 0x1E2C3E;      // 卡片 / 次级按钮
constexpr uint32_t bgHover = 0x2B4059;     // 悬停
constexpr uint32_t bgActive = 0x1E416B;    // 选中
constexpr uint32_t border = 0x485E7B;      // 常规描边
constexpr uint32_t borderSoft = 0x2C3D53;  // 弱描边
constexpr uint32_t text = 0xF1F5FC;        // 主文字
constexpr uint32_t textDim = 0xBDCADC;     // 次要文字
constexpr uint32_t textFaint = 0x99ABC3;   // 提示文字
constexpr uint32_t accent = 0x8AB8FF;      // 深色底上的强调文字 / 焦点
constexpr uint32_t action = 0x2563EB;      // 实心主操作
constexpr uint32_t actionHover = 0x2D6BEA; // 主操作悬停
constexpr uint32_t actionPressed = 0x1D4ED8;
constexpr uint32_t success = 0x61D5AC;
constexpr uint32_t warn = 0xE5B76A;
constexpr uint32_t danger = 0xF07888;

// t=0 取 a，t=1 取 b
inline uint32_t mix(uint32_t a, uint32_t b, double t) {
    auto channel = [&](int shift) {
        const double v = ((a >> shift) & 0xFF) * (1.0 - t) + ((b >> shift) & 0xFF) * t;
        return static_cast<uint32_t>(v + 0.5) & 0xFF;
    };
    return (channel(16) << 16) | (channel(8) << 8) | channel(0);
}
}  // namespace rshell::ui
