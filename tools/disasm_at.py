# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""反汇编镜像中任意地址区域。

用法:
    TNT_IMG=dumps/tnt_img.bin uv run tools/disasm_at.py <hex_addr> [insns]
    uv run tools/disasm_at.py --img dumps/tnt_img.bin 0x13495b0 60

镜像 = 从 module_base 起的连续 VA dump(文件偏移 == 模块偏移)。
"""
from __future__ import annotations

import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

from imglib import read_image


def main() -> None:
    args = sys.argv[1:]
    img_path = None
    if "--img" in args:
        i = args.index("--img")
        img_path = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    addr = int(args[0], 16)
    n = int(args[1]) if len(args) > 1 else 80
    img = read_image(img_path)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True
    for ins in md.disasm(img[addr:addr + n * 4], addr):
        print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}")


if __name__ == "__main__":
    main()
