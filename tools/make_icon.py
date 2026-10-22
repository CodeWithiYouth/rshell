# make_icon.py - 生成 rshell 的应用图标（src/res/rshell.ico）
#
# 设计：一枚深蓝渐变的圆角方块，中间是终端的提示符 ">_"。
# 全部用几何图形画出来（不依赖任何字体或美术资源），所以：
#   * 任意尺寸都能重新渲染，小尺寸只保留最粗的笔画，不会糊成一团；
#   * 想改配色只动下面几个常量再跑一次即可。
#
# 用法：python tools/make_icon.py [输出路径]

import os
import sys

from PIL import Image, ImageChops, ImageDraw

# 配色：与界面同源（GitHub Dark 的 accent 蓝）
TOP = (0x3B, 0x8E, 0xF7)     # 渐变左上
BOTTOM = (0x11, 0x3F, 0x9C)  # 渐变右下
GLYPH = (0xFF, 0xFF, 0xFF)   # 提示符

SUPERSAMPLE = 8  # 先放大 8 倍画，再降采样，边缘才平滑
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def diagonal_gradient(size, top, bottom):
    """从左上到右下的线性渐变（小图放大，避免逐像素算 1024x1024）。"""
    small = 96
    img = Image.new("RGB", (small, small))
    px = img.load()
    for y in range(small):
        for x in range(small):
            t = (x + y) / (2.0 * (small - 1))
            px[x, y] = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
    return img.resize((size, size), Image.BICUBIC)


def render(size):
    s = size * SUPERSAMPLE
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # 圆角方块（四角半径约 22%，比正圆的方形更像现代应用图标）
    inset = round(s * 0.03)
    radius = round(s * 0.225)
    shape = Image.new("L", (s, s), 0)
    ImageDraw.Draw(shape).rounded_rectangle(
        [inset, inset, s - inset - 1, s - inset - 1], radius=radius, fill=255
    )

    body = diagonal_gradient(s, TOP, BOTTOM).convert("RGBA")
    img.paste(body, (0, 0), shape)

    draw = ImageDraw.Draw(img, "RGBA")

    # 顶部一层很淡的高光（自上而下渐隐），让方块有体积感。
    # 小尺寸不做高光：16px 下这点亮度差只会让笔画发灰。
    if size > 24:
        gloss_h = round(s * 0.58)
        fade = Image.new("L", (1, gloss_h))
        for y in range(gloss_h):
            t = y / max(1, gloss_h - 1)
            fade.putpixel((0, y), int(54 * (1.0 - t) ** 1.7))
        gloss = Image.new("L", (s, s), 0)
        gloss.paste(fade.resize((s, gloss_h)), (0, inset))
        gloss = ImageChops.multiply(gloss, shape)
        img.paste(Image.new("RGBA", (s, s), (255, 255, 255, 255)), (0, 0), gloss)

    # 提示符 ">"：两段折线，圆头圆角。
    # 16/20/24 这几个档位单独加粗加大：分辨率不够时，笔画一细就糊成一团。
    bold = size <= 24
    stroke = max(2, round(s * (0.125 if bold else 0.085)))
    left = s * (0.245 if bold else 0.30)
    mid_y = s * (0.480 if bold else 0.50)
    right = s * (0.470 if bold else 0.47)
    top_y = s * (0.260 if bold else 0.315)
    bottom_y = s * (0.700 if bold else 0.685)
    draw.line([(left, top_y), (right, mid_y)], fill=GLYPH, width=stroke, joint="curve")
    draw.line([(right, mid_y), (left, bottom_y)], fill=GLYPH, width=stroke, joint="curve")
    r = stroke / 2.0
    for cx, cy in ((left, top_y), (right, mid_y), (left, bottom_y)):
        draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=GLYPH)

    # 下划线 "_"
    bar = max(2, round(s * (0.105 if bold else 0.075)))
    draw.line(
        [
            (s * (0.590 if bold else 0.555), s * (0.710 if bold else 0.665)),
            (s * (0.770 if bold else 0.715), s * (0.710 if bold else 0.665)),
        ],
        fill=GLYPH,
        width=bar,
    )

    # 裁回圆角，避免折线/圆点溢出方块
    out = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    out.paste(img, (0, 0), shape)
    return out.resize((size, size), Image.LANCZOS)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    target = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "src", "res", "rshell.ico")
    os.makedirs(os.path.dirname(target), exist_ok=True)

    frames = [render(size) for size in SIZES]
    # PIL 的多尺寸 ICO：把最大的那张存进去，sizes 指定要写哪些档位
    frames[-1].save(target, format="ICO", sizes=[(s, s) for s in SIZES],
                    append_images=frames[:-1])
    print(f"wrote {target} ({', '.join(str(s) for s in SIZES)})")


if __name__ == "__main__":
    main()
