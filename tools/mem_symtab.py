# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""从游戏内存镜像提取真实动态符号表(名字未被爱加密壳随机化)。

TNT v260906.1/v261016.1: 磁盘 .so 的 dynsym/dynstr 是假的(随机名);
真实符号名在运行时解密区的真实 dynstr 里(~31k 串), 镜像中另有两份真实 dynsym 数组:
  表A: st_value = 模块偏移 (runtime_addr = module_base + st_value)  ← 采用
  表B: st_value = 解密区相对偏移 (表A值 - 解密代码区起点)
两表同名同 size, 差一个常数。dynstr 基址 B 由"已知名字记录 + 邻居严格校验"反推。

用法:
    uv run tools/mem_symtab.py parse <dump.bin> [--probe 名字前缀] [--out syms.json]
    uv run tools/mem_symtab.py find  <dump.bin> [--probe 名字前缀] <关键词>...
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

ST = struct.Struct("<IBBHQQ")


def find_str_cluster(img: bytes, probe: bytes) -> tuple[int, int]:
    pos = img.find(probe)
    if pos < 0:
        raise ValueError(f"probe not found: {probe!r}")

    def ok(p: int) -> bool:
        return img[p] == 0 or 0x20 <= img[p] <= 0x7E

    start = pos
    while start > 0 and ok(start - 1):
        start -= 1
    end = pos + len(probe)
    while end < len(img) and ok(end):
        end += 1
    return start, end


def cluster_names(img: bytes, cs: int, ce: int) -> dict[str, int]:
    out: dict[str, int] = {}
    i = cs
    while i < ce:
        j = img.find(b"\x00", i, ce)
        if j < 0:
            break
        if j > i:
            name = img[i:j].decode("utf-8", "replace")
            out.setdefault(name, i)
        i = j + 1
    return out


def valid_name(img: bytes, B: int, st_name: int) -> bool:
    s = B + st_name
    return 0 < s < len(img) - 2 and img[s - 1] == 0 and 0x20 <= img[s] <= 0x7E


def find_tables(img: bytes, A: int) -> list[tuple[int, int]]:
    """反推 dynstr 基址 B 并定位 dynsym 数组锚点。返回 [(anchor, B), ...]。"""
    found: dict[tuple[int, int], int] = {}
    for p in range(0, len(img) - 24, 8):
        v = struct.unpack_from("<I", img, p)[0]
        if v == 0 or v > 0x400000:
            continue
        B = A - v
        if B < 0 or B & 7:
            continue
        score = 0
        for k in (-4, -3, -2, -1, 1, 2, 3, 4):
            q = p + k * 24
            if q < 0 or q + 24 > len(img):
                break
            st_name, st_info, st_other, st_shndx, st_value, st_size = ST.unpack_from(img, q)
            if st_info & 0xF > 10 or (st_info >> 4) > 3 or st_value >= len(img) or st_size >= 0x1000000:
                break
            if not valid_name(img, B, st_name):
                break
            score += 1
        if score >= 5:
            found[(p, B)] = score
    anchors = sorted(found, key=lambda t: -found[t])
    return anchors


def enum_table(img: bytes, anchor: int, B: int) -> dict[str, dict]:
    """从锚点双向容忍枚举(允许少量空洞, 连续 8 条非法即停)。"""
    out: dict[str, dict] = {}
    misses = 0
    off = anchor
    while off >= 0 and misses < 8 and off + 24 <= len(img):
        st_name, st_info, st_other, st_shndx, st_value, st_size = ST.unpack_from(img, off)
        if (st_name == 0 or valid_name(img, B, st_name)) and st_info & 0xF <= 10 and (st_info >> 4) <= 3 \
                and st_value < len(img) and st_size < 0x1000000:
            if st_name:
                end = img.find(b"\x00", B + st_name)
                name = img[B + st_name:end].decode("utf-8", "replace")
                out.setdefault(name, dict(value=st_value, size=st_size, type=st_info & 0xF,
                                          bind=st_info >> 4, shndx=st_shndx))
            misses = 0
        else:
            misses += 1
        off -= 24
    misses = 0
    off = anchor + 24
    while misses < 8 and off + 24 <= len(img):
        st_name, st_info, st_other, st_shndx, st_value, st_size = ST.unpack_from(img, off)
        if (st_name == 0 or valid_name(img, B, st_name)) and st_info & 0xF <= 10 and (st_info >> 4) <= 3 \
                and st_value < len(img) and st_size < 0x1000000:
            if st_name:
                end = img.find(b"\x00", B + st_name)
                name = img[B + st_name:end].decode("utf-8", "replace")
                out.setdefault(name, dict(value=st_value, size=st_size, type=st_info & 0xF,
                                          bind=st_info >> 4, shndx=st_shndx))
            misses = 0
        else:
            misses += 1
        off += 24
    return out


def main() -> None:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(1)
    cmd, rest = args[0], args[1:]
    path = rest[0]
    probe = b"_ZN13BattleConsole"
    out_path = None
    if "--probe" in rest:
        probe = rest[rest.index("--probe") + 1].encode()
    if "--out" in rest:
        out_path = rest[rest.index("--out") + 1]
    keys = [k.lower() for k in rest[1:] if not k.startswith("--") and k != out_path]

    img = Path(path).read_bytes()
    cs, ce = find_str_cluster(img, probe)
    names = cluster_names(img, cs, ce)
    print(f"dynstr cluster: 0x{cs:x}..0x{ce:x} ({len(names)} strings)")
    A = names.get(probe.decode(errors="replace")) or next(
        a for n, a in names.items() if n.startswith(probe.decode(errors="replace")))
    anchors = find_tables(img, A)
    if not anchors:
        raise SystemExit("no symtab anchor found")
    print(f"anchors: {[(hex(p), hex(b)) for p, b in anchors[:4]]}")

    tables = [(anchor, B, enum_table(img, anchor, B)) for anchor, B in anchors[:4]]
    # 选"模块偏移语义"表: 函数值应落在解密代码区(>0xB09000); 另一表差一个解密区基址常数
    def func_median(tbl: dict[str, dict]) -> float:
        vals = sorted(s["value"] for s in tbl.values() if s["type"] == 2 and s["size"] > 8)
        return vals[len(vals) // 2] if vals else 0

    tables.sort(key=lambda t: -len(t[2]))
    base_tbl = tables[0]
    for anchor, B, tbl in tables[1:]:
        if len(tbl) > len(base_tbl[2]):
            base_tbl = (anchor, B, tbl)
    print(f"table at 0x{base_tbl[0]:x} (B=0x{base_tbl[1]:x}) entries={len(base_tbl[2])} "
          f"func_median=0x{func_median(base_tbl[2]):x}")
    # 合并其余表(处理值语义差一个常数)
    merged = dict(base_tbl[2])
    med0 = func_median(base_tbl[2])
    for anchor, B, tbl in tables:
        if anchor == base_tbl[0]:
            continue
        med = func_median(tbl)
        delta = 0
        if med and med0 and abs(med - med0) > 0x1000:
            delta = med0 - med
        for n, s in tbl.items():
            if n not in merged:
                merged[n] = dict(s)
                merged[n]["value"] += delta
    print(f"merged {len(merged)} symbols")

    if cmd == "parse":
        if out_path:
            Path(out_path).write_text(json.dumps(merged, ensure_ascii=False), encoding="utf-8")
            print(f"-> {out_path}")
    else:
        keys = keys or ["battleconsole"]
        hits = [(n, s) for n, s in merged.items() if any(k in n.lower() for k in keys)]
        hits.sort(key=lambda t: t[1]["value"])
        print(f"{len(hits)} hits:")
        for n, s in hits[:60]:
            print(f"  0x{s['value']:08x}  size={s['size']:<6} {n}")


if __name__ == "__main__":
    main()
