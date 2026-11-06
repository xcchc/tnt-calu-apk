# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""解析 vtable 槽位 -> 虚函数符号名 映射, 并反汇编真实函数入口。

symtab 的 st_value 错位(指向函数中段, 爱加密壳特征), 但 vtable 里的函数指针
是运行时真实入口。对每个 vtable 槽位, 取与 symtab 值最近的符号名作为槽位名
(ICF/错位是固定偏移, 近邻匹配可靠), 新旧两版比对槽位序列验证布局一致性。

用法:
    uv run tools/vtable_map.py <old_img> <new_img> \
        [--old-syms dumps/syms_old.json] [--new-syms dumps/syms.json] \
        [--base 0x400023600000] [--vt _ZTV13BattleConsole ...]
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM


def load(old_syms: str, new_syms: str) -> tuple[dict, dict]:
    with open(old_syms, encoding="utf-8") as f:
        old = json.load(f)
    with open(new_syms, encoding="utf-8") as f:
        new = json.load(f)
    for s in old.values():
        s["value"] += 0xADE000
    return old, new


def name_at(syms: dict, addr: int, lo: int = -0x600, hi: int = 0x600) -> str | None:
    best, bestd = None, None
    for n, s in syms.items():
        if s["type"] != 2 or s["size"] < 4:
            continue
        d = s["value"] - addr
        if lo <= d <= hi and (bestd is None or abs(d) < abs(bestd)):
            best, bestd = n, d
    return f"{best} (sym{bestd:+d})" if best else None


def vt_entries(img: bytes, vt_addr: int, size: int, base: int) -> list[tuple[int, int, bool]]:
    """返回 [(slot_idx, module_offset_or_abs, in_module)]。"""
    out = []
    for i in range(size // 8):
        q = struct.unpack_from("<Q", img, vt_addr + i * 8)[0]
        in_mod = base <= q < base + 0x1812000
        out.append((i, q - base if in_mod else q, in_mod))
    return out


def main() -> None:
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    new_img = Path(args[1]).read_bytes()
    vts = ["_ZTV13BattleConsole", "_ZTV22BattleSpritePlayerSelf", "_ZTV11PanelBattle",
           "_ZTV18BattleSpritePlayer", "_ZTV12BattlePlayer"]
    if "--vt" in args:
        vts = args[args.index("--vt") + 1:]
    old_syms = args[args.index("--old-syms") + 1] if "--old-syms" in args else "dumps/syms_old.json"
    new_syms = args[args.index("--new-syms") + 1] if "--new-syms" in args else "dumps/syms.json"
    base = int(args[args.index("--base") + 1], 16) if "--base" in args else 0x400023600000
    old, new = load(old_syms, new_syms)

    for vt in vts:
        n = new.get(vt)
        if not n:
            print(f"== {vt}: missing")
            continue
        print(f"\n===== {vt}  new=0x{n['value']:x} size={n['size']}")
        for i, val, in_mod in vt_entries(new_img, n["value"], n["size"], base):
            if i < 2:
                continue
            if not in_mod:
                print(f"  [{i:2d}] ext  0x{val:x}")
                continue
            nm = name_at(new, val)
            print(f"  [{i:2d}] 0x{val:08x}  {nm or '?'}")


if __name__ == "__main__":
    main()
