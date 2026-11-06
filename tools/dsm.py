# /// script
# requires-python = ">=3.10"
# dependencies = ["capstone"]
# ///
"""按 ELF 程序头从真实 .so 里反汇编: 给定虚拟地址, 输出指令。

用法:
    uv run tools/dsm.py <libgame.so> <hex_va> [n_insns]
    uv run tools/dsm.py dumps/libgame_arm64-v8a.so 0x13495b0 60

与 tools/disasm_at.py 的区别: 这个走 ELF PT_LOAD 做 VA->文件偏移换算,
适用于磁盘上的原始 .so; disasm_at.py 适用于"运行时整块 dump"(偏移即 VA)。
"""
from __future__ import annotations

import struct
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM


def segments(path: Path) -> list[tuple[int, int, int, int]]:
    data = path.read_bytes()
    assert data[:4] == b"\x7fELF", "不是 ELF"
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    phentsize = struct.unpack_from("<H", data, 0x36)[0]
    phnum = struct.unpack_from("<H", data, 0x38)[0]
    out = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type = struct.unpack_from("<I", data, off)[0]
        if p_type != 1:  # PT_LOAD
            continue
        p_offset = struct.unpack_from("<Q", data, off + 0x08)[0]
        p_vaddr = struct.unpack_from("<Q", data, off + 0x10)[0]
        p_filesz = struct.unpack_from("<Q", data, off + 0x20)[0]
        p_memsz = struct.unpack_from("<Q", data, off + 0x28)[0]
        out.append((p_vaddr, p_vaddr + p_filesz, p_offset, p_memsz))
    return out


def main() -> None:
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    so = Path(sys.argv[1])
    va = int(sys.argv[2], 16)
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    segs = segments(so)
    off = None
    for vstart, vend, foff, _memsz in segs:
        if vstart <= va < vend:
            off = foff + (va - vstart)
            break
    if off is None:
        raise SystemExit("该 VA 不在任何 PT_LOAD 文件段内")
    data = so.read_bytes()[off:off + n * 4]
    md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    md.skipdata = True
    for ins in md.disasm(data, va):
        print(f"  0x{ins.address:x}: {ins.mnemonic:8s} {ins.op_str}")


if __name__ == "__main__":
    main()
