# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""对比新旧内存镜像中同名函数的访存偏移, 验证结构体字段偏移是否随版本漂移。

背景: TNT v260906.1 -> v261016.1, 符号地址分区域漂移, 但结构体字段偏移
(BattleConsole+0x28/0x60/0xA0/0xC8/0x100, Config+0x470, role+0x88/0x8C ...) 是否变化未知。
方法: 对两版同名函数做 arm64 反汇编, 提取 [Xn, #imm] 访存偏移序列并对比。

用法:
    uv run tools/field_drift.py <old_img> <new_img> [--fns name1,name2,...]
默认对比一组关键函数。镜像 = 从 module_base 起的连续 VA dump(文件偏移 == 模块偏移)。
"""
from __future__ import annotations

import json
import re
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

MEM_RE = re.compile(r"\[(x\d+|sp)(?:,\s*#(-?(?:0x[0-9a-f]+|\d+)))?\]")

DEFAULT_FNS = [
    "_ZN13BattleConsole11updateAngleEb",
    "_ZN13BattleConsole17createActionForceEb",
    "_ZN22BattleSpritePlayerSelf12syncPositionEv",
    "_ZN11PanelBattle10actionFireERK17BattleCommandFire",
    "_ZN11PanelBattle12setCameraPosERK6QPointb",
    "_ZN11PanelBattle18onZoomScaleChangedEd",
    "_ZN10BattleWind8hashWindEi",
]


def load_syms(old_path: str, new_path: str) -> tuple[dict, dict]:
    with open(old_path, encoding="utf-8") as f:
        old = json.load(f)
    with open(new_path, encoding="utf-8") as f:
        new = json.load(f)
    # 旧表是 anon-relative, 加 0xADE000 换成模块偏移(由两版对照实测得出)
    for s in old.values():
        s["value"] += 0xADE000
    return old, new


def offs_of(md: Cs, code: bytes, addr: int) -> list[tuple[int, str, str]]:
    out = []
    for ins in md.disasm(code, addr):
        m = MEM_RE.search(ins.op_str)
        if m and m.group(2) is not None:
            imm = int(m.group(2), 0)
            out.append((imm, ins.mnemonic, ins.op_str))
    return out


def main() -> None:
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    old_img = Path(args[0]).read_bytes()
    new_img = Path(args[1]).read_bytes()
    fns = DEFAULT_FNS
    if "--fns" in args:
        fns = args[args.index("--fns") + 1].split(",")
    old_syms = args[args.index("--old-syms") + 1] if "--old-syms" in args else "dumps/syms_old.json"
    new_syms = args[args.index("--new-syms") + 1] if "--new-syms" in args else "dumps/syms.json"
    old, new = load_syms(old_syms, new_syms)
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True

    for fn in fns:
        o, n = old.get(fn), new.get(fn)
        if not o or not n:
            print(f"== {fn}: old={'y' if o else 'N'} new={'y' if n else 'N'} (skip)")
            continue
        size = min(o["size"], n["size"]) or 256
        o_offs = offs_of(md, old_img[o["value"]:o["value"] + o["size"]], o["value"])
        n_offs = offs_of(md, new_img[n["value"]:n["value"] + n["size"]], n["value"])
        o_seq = [x[0] for x in o_offs]
        n_seq = [x[0] for x in n_offs]
        same = o_seq == n_seq
        print(f"== {fn}  old@0x{o['value']:x} new@0x{n['value']:x}  偏移序列{'一致' if same else '不一致'}")
        if not same:
            from itertools import zip_longest
            for i, (a, b) in enumerate(zip_longest(o_seq, n_seq)):
                mark = " " if a == b else "*"
                if a != b:
                    print(f"   {mark} #{i}: old={a} new={b}")
            print(f"   old seq: {o_seq[:40]}")
            print(f"   new seq: {n_seq[:40]}")


if __name__ == "__main__":
    main()
