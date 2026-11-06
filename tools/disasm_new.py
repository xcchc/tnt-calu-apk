# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""反汇编镜像中指定符号的函数体(镜像代码明文, 可静态分析)。

用法:
    uv run tools/disasm_new.py <symbol_name> [max_insns]
    uv run tools/disasm_new.py _ZN13BattleConsole11updateAngleEb 120

需要 TNT_IMG(镜像) 与 TNT_SYMS(符号表), 或用 --img/--syms 指定。
"""
from __future__ import annotations

import sys

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

from imglib import read_image, read_symbols


def main() -> None:
    args = sys.argv[1:]
    img_path = syms_path = None
    if "--img" in args:
        i = args.index("--img")
        img_path = args[i + 1]
        del args[i:i + 2]
    if "--syms" in args:
        i = args.index("--syms")
        syms_path = args[i + 1]
        del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__)
    name = args[0]
    limit = int(args[1]) if len(args) > 1 else 120
    syms = read_symbols(syms_path)
    img = read_image(img_path)
    if name not in syms:
        hits = [n for n in syms if name in n][:20]
        raise SystemExit(f"符号不存在: {name}\n相近的: {hits}")
    s = syms[name]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True
    start = s["value"] - 0x80
    end = s["value"] + s["size"] + 0x40
    code = img[start:end]
    print(f"{name} sym@0x{s['value']:x} size={s['size']}  disasm 0x{start:x}..0x{end:x}")
    for i, ins in enumerate(md.disasm(code, start)):
        if i >= limit:
            print("  ...")
            break
        print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}")


if __name__ == "__main__":
    main()
