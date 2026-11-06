# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""探测内存镜像中真实 dynsym 数组的位置与 dynstr 基址。

方法: 取一个唯一符号名, 知道它在镜像中的绝对偏移 A。
真实 dynsym 条目形如 {u32 st_name = A - B, u8 st_info, u8 st_other, u16 st_shndx, u64 st_value, u64 st_size}。
B(dynstr_base)未知。对每个 8 字节对齐位置 p 的 u32 值 v, 令 B = A - v,
校验 p±k*24 处条目的 st_name 也指向"B 之后紧跟 NUL 的可打印串", 打分选最优。
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path


def main() -> None:
    img = Path(sys.argv[1]).read_bytes()
    probe = sys.argv[2].encode() if len(sys.argv) > 2 else b"_ZN13BattleConsole20onRoleInMechaChangedEv"
    A = img.find(probe)
    if A < 0:
        raise SystemExit("probe not found")
    print(f"probe abs offset: 0x{A:x}")

    def valid_name(B: int, st_name: int) -> bool:
        s = B + st_name
        return 0 < s < len(img) - 2 and img[s - 1] == 0 and 0x20 <= img[s] <= 0x7E

    st = struct.Struct("<IBBHQQ")
    best = []
    # B 候选: 由对齐位置的 u32 反推; 只看 v < 0x400000 (st_name 一般远小于 dynstr 尺寸)
    for p in range(0, len(img) - 24, 8):
        v = struct.unpack_from("<I", img, p)[0]
        if v == 0 or v > 0x400000:
            continue
        B = A - v
        if B < 0 or B & 7:
            continue
        score = 0
        ok = True
        for k in (-4, -3, -2, -1, 1, 2, 3, 4):
            q = p + k * 24
            if q < 0 or q + 24 > len(img):
                break
            st_name, st_info, st_other, st_shndx, st_value, st_size = st.unpack_from(img, q)
            if st_info & 0xF > 10 or (st_info >> 4) > 3:
                break
            if st_value >= len(img) or st_size >= 0x1000000:
                break
            if not valid_name(B, st_name):
                break
            score += 1
        if score >= 4:
            best.append((score, p, B))
            if score == 7:
                break

    best.sort(reverse=True)
    print(f"{len(best)} candidate entries, top:")
    for score, p, B in best[:10]:
        st_name, st_info, st_other, st_shndx, st_value, st_size = st.unpack_from(img, p)
        nm = img[B + st_name:img.find(b"\x00", B + st_name)].decode("utf-8", "replace")
        print(f"  score={score} p=0x{p:x} B=0x{B:x} val=0x{st_value:x} sz=0x{st_size:x} name={nm[:60]}")


if __name__ == "__main__":
    main()
