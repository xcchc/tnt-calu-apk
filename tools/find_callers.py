# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""在镜像中查找 bl 目标为指定函数的调用点, 反汇编调用者上下文。

用法:
    uv run tools/find_callers.py <hex_target> [radius_insns]
    TNT_IMG=dumps/tnt_img.bin uv run tools/find_callers.py 0x13495b0 30
"""
from __future__ import annotations

import struct
import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

from imglib import read_image

# 解密代码区(模块内相对偏移), 见 docs/layout-v261016.json -> module.code_region_rel
CODE_LO, CODE_HI = 0xB09000, 0x1588000


def main() -> None:
    args = sys.argv[1:]
    img_path = None
    if "--img" in args:
        i = args.index("--img")
        img_path = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    target = int(args[0], 16)
    radius = int(args[1]) if len(args) > 1 else 30
    img = read_image(img_path)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True

    hits = []
    for off in range(CODE_LO & ~3, min(CODE_HI, len(img)), 4):
        w = struct.unpack_from("<I", img, off)[0]
        if (w & 0xFC000000) == 0x94000000:  # bl
            imm = w & 0x03FFFFFF
            if imm & 0x2000000:
                imm -= 0x4000000
            dest = off + imm * 4
            if dest == target:
                hits.append(off)
    print(f"{len(hits)} call sites to 0x{target:x}")
    for h in hits[:12]:
        start = h - radius * 4
        print(f"\n--- caller context around 0x{h:x} ---")
        for ins in md.disasm(img[start:h + 16], start):
            mark = " <==" if ins.address == h else ""
            print(f"  0x{ins.address:x}: {ins.mnemonic} {ins.op_str}{mark}")


if __name__ == "__main__":
    main()
