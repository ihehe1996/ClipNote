#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 resources/clipnote.png 生成 resources/clipnote.ico（多尺寸）。

**这是离线生成器，不是构建步骤。** 生成的 .ico 提交进仓库，所以构建链里
不需要 Python 和 Pillow 这两个依赖 —— 别把它接到 CMake 上。

为什么 png 和 ico 两份都要（不是重复）：
  - .png 走 resources/resources.qrc，由 QIcon 在运行期读，管的是托盘图标和
    自绘标题栏左上角那个（见 ui/AppIcon.cpp）
  - .ico 走 resources/clipnote.rc，由 windres 编进 PE 的资源段，管的是**资源管理器
    在文件上画的图标** —— 桌面/开始菜单快捷方式、任务栏、资源管理器里的文件本身、
    以及「应用和功能」里的卸载项
两边互相替代不了：QIcon 读不到 PE 资源段，而资源管理器根本不看 qrc。
只留 png 的后果就是这个 bug：装完之后快捷方式显示成 Windows 的通用图标。

换 logo 的流程：替换 resources/clipnote.png → 跑一次这个脚本 → 重新构建。

为什么 ICO 要手搓（而不是一句 Image.save(..., format="ICO", sizes=[...])）：
  1. 本机的 Pillow 7.2 会把 256×256 那条**静默丢掉**（不报错，生成出来的图标就是
     少了最大那一档）；
  2. 它给 16/24/32/48 写的也是 PNG 条目。而 Windows 官方只保证 256 那条能用 PNG，
     小尺寸的 PNG 条目属于「现在能跑」的地界 —— 万一哪条 shell 路径（详情视图的
     16px、覆盖图标、或者某个 GDI 工具）没处理，表现就是这个图标又不显示了，
     正好是这次要修的那个症状。
所以：小尺寸按老规矩用未压缩 BMP 条目（兼容性最好，反正也就几 KB），
64 以上用 PNG 条目（256 那条用 BMP 要占 260KB）。容器和 BMP 编码都是这里自己写的。

用法：
    python tools/make-icon.py
"""

import os
import struct
import sys
import zlib

try:
    from PIL import Image
except ImportError:
    sys.exit("需要 Pillow：pip install Pillow")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "resources", "clipnote.png")
DST = os.path.join(ROOT, "resources", "clipnote.ico")

# 小尺寸走 BMP 条目、大尺寸走 PNG 条目的分界（见上面第 2 条理由）。
BMP_MAX = 48

# 大尺寸给资源管理器的大图标视图用，小尺寸给列表视图、开始菜单和标题栏用。
# Windows 会自己挑最接近的那个再缩，所以尺寸覆盖率比单张大图重要。
SIZES = [16, 24, 32, 48, 64, 128, 256]


def load_square():
    """读源图并补成正方形，返回 (图, 是否够大)。"""
    image = Image.open(SRC).convert("RGBA")

    # 先补成正方形：ICO 条目必须是方的，源图不是方的就得补透明边（而不是拉伸，
    # 那会把 logo 扯变形）。
    if image.width != image.height:
        side = max(image.width, image.height)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(image, ((side - image.width) // 2, (side - image.height) // 2))
        image = square

    enough = image.width >= max(SIZES)
    # 比最大尺寸还大的话先落下来，后面再逐个缩，比从 2000px 一步缩到 16px 清楚
    if image.width > max(SIZES):
        image = image.resize((max(SIZES), max(SIZES)), Image.LANCZOS)
    return image, enough


def bmp_entry(image, size):
    """一个 32bpp 的 BMP 条目（BITMAPINFOHEADER + 像素 + AND 掩码）。

    三个容易写错的地方：
      - biHeight 要写成**两倍**行数：图标格式在这个头下面叠了两张图，
        先是带 alpha 的 XOR 位图，再是一张 1bpp 的 AND 掩码；
      - 像素是**自下而上**存的，而且要 BGRA 不是 RGBA；
      - AND 掩码全留 0（= 全不透明）。透明度由 alpha 通道负责，掩码是给
        「不认识 alpha 的老代码」兜底的，现代 shell 不看它，但少了这张图
        有些解析器会认为条目被截断。
    """
    pixel = image.resize((size, size), Image.LANCZOS).tobytes()  # RGBA, 自上而下
    rows = []
    for y in range(size - 1, -1, -1):                            # 倒过来：自下而上
        row = pixel[y * size * 4:(y + 1) * size * 4]
        # RGBA -> BGRA，顺手每行拼起来
        rows.append(b"".join(bytes((row[i + 2], row[i + 1], row[i], row[i + 3]))
                             for i in range(0, len(row), 4)))
    xor = b"".join(rows)
    mask_row = ((size + 31) // 32) * 4                           # 每行按 4 字节对齐
    mask = b"\x00" * (mask_row * size)

    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0,
                         len(xor) + len(mask), 0, 0, 0, 0)
    return header + xor + mask


def png_entry(image, size):
    """64px 以上用 PNG 条目，免得 256 那条光着头就占 260KB。"""
    tmp = image.resize((size, size), Image.LANCZOS)

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data
                + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    raw = b""
    pixels = tmp.tobytes()
    for y in range(size):
        raw += b"\x00" + pixels[y * size * 4:(y + 1) * size * 4]  # 每行前面一个过滤器字节
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9))
            + chunk(b"IEND", b""))


def build_ico(entries):
    """把 {尺寸: 数据块} 拼成一个 ICO 文件。"""
    sizes = sorted(entries)
    # ICONDIR：保留位 0、类型 1(图标)、条目数
    out = bytearray(struct.pack("<HHH", 0, 1, len(sizes)))
    offset = 6 + 16 * len(sizes)
    directory = bytearray()
    blobs = bytearray()
    for size in sizes:
        blob = entries[size]
        # ICONDIRENTRY：宽高各一个字节（256 只能写成 0）、调色板 0、保留 0、
        # 平面数 1、位深 32、数据块长度、数据块偏移
        directory += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32,
                                 len(blob), offset)
        blobs += blob
        offset += len(blob)
    return bytes(out + directory + blobs)


def main():
    if not os.path.exists(SRC):
        sys.exit("找不到源图：%s" % SRC)

    image, enough = load_square()

    entries = {}
    for size in SIZES:
        entries[size] = (bmp_entry(image, size) if size <= BMP_MAX
                         else png_entry(image, size))

    with open(DST, "wb") as handle:
        handle.write(build_ico(entries))

    verify(image)
    print("写出 %s" % DST)
    print("  尺寸 %d 种：%s" % (len(SIZES), ", ".join(str(s) for s in SIZES)))
    print("  体积 %.1f KB" % (os.path.getsize(DST) / 1024.0))

    if not enough:
        # 不算错：拿 200px 的源图放大出来的 256 条目，仍然比「只给到 128、
        # 让 Windows 自己去放大」清楚。只是下次换 logo 时值得给张更大的。
        print("  提示：源图只有 %dpx，256 那条是放大出来的。"
              "有 256px 以上的原图会更干净。" % image.width)


def verify(image):
    """回读一遍：条目数、尺寸、以及解出来的像素对不对。

    这一步不能省 —— 自己拼二进制最容易错的就是「头写对了、像素是花的」，
    光看尺寸列表看不出来。这里让 Pillow 用自己的解码器读回来当裁判。
    """
    with Image.open(DST) as check:
        got = sorted(size[0] for size in check.info.get("sizes", []))
    if got != SIZES:
        sys.exit("回读出来的尺寸是 %s，和期望的 %s 对不上" % (got, SIZES))

    # 取样点：中心，加上三个不对称的位置。只查中心是不够的 —— logo 上下翻个个儿、
    # 或者左右翻，中心那个点很可能照样对得上，而偏一点的位置立刻就不一样了。
    probes = [(0.5, 0.5), (0.25, 0.25), (0.75, 0.75), (0.25, 0.75)]

    for size in SIZES:
        # Pillow 选条目的官方写法：给它一个 size，再 load
        with Image.open(DST) as check:
            check.size = (size, size)
            check.load()
            if check.size != (size, size):
                sys.exit("%d 那条解出来的尺寸是 %s" % (size, check.size))
            mine = check.convert("RGBA")

        want = image.resize((size, size), Image.LANCZOS)
        for fx, fy in probes:
            point = (int(size * fx), int(size * fy))
            a, b = mine.getpixel(point), want.getpixel(point)
            if any(abs(x - y) > 4 for x, y in zip(a, b)):
                sys.exit("%d 那条在 %s 解出来是 %s，期望 %s —— 编坏了"
                         % (size, point, a, b))


if __name__ == "__main__":
    main()
