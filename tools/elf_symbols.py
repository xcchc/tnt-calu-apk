# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""ELF 动态符号表提取与新旧对比(纯标准库, 无依赖)。

TNT 游戏 libgame_arm64-v8a.so 的节表被爱加密壳伪装(全叫 ..dynamic),
但 PT_DYNAMIC 动态段真实: DT_SYMTAB/DT_STRTAB/DT_HASH 指向完整符号表
(旧版 symtab@0x4bc88 strtab@0x151dc8, ~28k 符号, 线性表用 DT_HASH nchain 计数)。
本解析器走 PT_DYNAMIC, 不信任节表。

用法:
    uv run tools/elf_symbols.py dump <libgame.so> [out.json]
    uv run tools/elf_symbols.py diff <old.so> <new.so> [--filter 关键词]
    uv run tools/elf_symbols.py find <libgame.so> <关键词> [<关键词>...]

背景: v260906.1 -> v261016.1 重定位。旧版已验证符号:
BattleConsole vtable=0x150C080, Singleton<Config>::s_inst=0x1535960, syncPosition=0x12fa4d4。
runtime_addr = module_base + st_value。
"""
from __future__ import annotations

import json
import struct
import sys
from pathlib import Path

PT_LOAD, PT_DYNAMIC = 1, 2
DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB = 0, 4, 5, 6
DT_STRSZ, DT_SYMENT, DT_GNU_HASH = 10, 11, 0x6FFFFEF5


def _vaddr_to_offset(vaddr: int, phdrs: list[dict]) -> int | None:
    for ph in phdrs:
        if ph["type"] == PT_LOAD and ph["vaddr"] <= vaddr < ph["vaddr"] + ph["filesz"]:
            return ph["offset"] + (vaddr - ph["vaddr"])
    return None


def parse_elf_symbols(path: str | Path) -> dict[str, dict]:
    data = Path(path).read_bytes()
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError(f"not an ELF64: {path}")

    e_phoff, = struct.unpack_from("<Q", data, 0x20)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x36)

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = struct.unpack_from("<IIQQQQQQ", data, off)
        phdrs.append(dict(type=p_type, offset=p_offset, vaddr=p_vaddr, filesz=p_filesz))

    dyn = next((ph for ph in phdrs if ph["type"] == PT_DYNAMIC), None)
    if dyn is None:
        raise ValueError("no PT_DYNAMIC")

    tags = {}
    off = dyn["offset"]
    while off + 16 <= dyn["offset"] + dyn["filesz"]:
        d_tag, d_un = struct.unpack_from("<QQ", data, off)
        off += 16
        if d_tag == DT_NULL:
            break
        tags.setdefault(d_tag, d_un)

    symtab_va = tags.get(DT_SYMTAB)
    strtab_va = tags.get(DT_STRTAB)
    if symtab_va is None or strtab_va is None:
        raise ValueError("no DT_SYMTAB/DT_STRTAB")
    symtab = _vaddr_to_offset(symtab_va, phdrs) or symtab_va
    strtab = _vaddr_to_offset(strtab_va, phdrs) or strtab_va
    syment = tags.get(DT_SYMENT, 24)

    # 符号计数: 优先 DT_HASH 的 nchain(线性表长度); 次选 DT_GNU_HASH 推算
    count = None
    if DT_HASH in tags:
        hash_off = _vaddr_to_offset(tags[DT_HASH], phdrs) or tags[DT_HASH]
        nbucket, nchain = struct.unpack_from("<II", data, hash_off)
        count = nchain
    if count is None and DT_GNU_HASH in tags:
        gh = _vaddr_to_offset(tags[DT_GNU_HASH], phdrs) or tags[DT_GNU_HASH]
        nbuckets, symoffset, bloom_size, bloom_shift = struct.unpack_from("<IIII", data, gh)
        # 走 bucket 链找最大索引
        buckets_off = gh + 16 + bloom_size * 8
        max_idx = symoffset
        for b in range(nbuckets):
            idx, = struct.unpack_from("<I", data, buckets_off + b * 4)
            max_idx = max(max_idx, idx)
        if max_idx >= symoffset:
            chains_off = buckets_off + nbuckets * 4
            idx = max_idx
            while True:
                val, = struct.unpack_from("<I", data, chains_off + (idx - symoffset) * 4)
                idx += 1
                if val & 1:
                    break
        count = idx
    if count is None:
        raise ValueError("cannot determine symbol count")

    symbols: dict[str, dict] = {}
    for i in range(count):
        off = symtab + i * syment
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from("<IBBHQQ", data, off)
        if st_name == 0:
            continue
        end = data.index(b"\x00", strtab + st_name)
        name = data[strtab + st_name:end].decode("utf-8", "replace")
        if name and name not in symbols:
            symbols[name] = {
                "value": st_value,
                "size": st_size,
                "type": st_info & 0xF,
                "bind": st_info >> 4,
                "shndx": st_shndx,
            }
    return symbols


def cmd_dump(argv: list[str]) -> None:
    path = argv[0]
    out = argv[1] if len(argv) > 1 else None
    syms = parse_elf_symbols(path)
    payload = json.dumps(syms, ensure_ascii=False)
    if out:
        Path(out).write_text(payload, encoding="utf-8")
        print(f"{len(syms)} symbols -> {out}")
    else:
        print(payload)


def cmd_find(argv: list[str]) -> None:
    path, keys = argv[0], [k.lower() for k in argv[1:]]
    syms = parse_elf_symbols(path)
    hits = [(n, s) for n, s in syms.items() if any(k in n.lower() for k in keys)]
    hits.sort(key=lambda t: t[1]["value"])
    print(f"{len(hits)} hits (of {len(syms)} symbols)")
    for n, s in hits:
        print(f"  0x{s['value']:08x}  size={s['size']:<6} {n}")


def cmd_diff(argv: list[str]) -> None:
    old_path, new_path = argv[0], argv[1]
    flt = None
    if "--filter" in argv:
        flt = argv[argv.index("--filter") + 1].lower()
    old = parse_elf_symbols(old_path)
    new = parse_elf_symbols(new_path)
    shared = [n for n in old if n in new]
    only_old = [n for n in old if n not in new]
    only_new = [n for n in new if n not in old]
    drifted = []
    for n in shared:
        d = new[n]["value"] - old[n]["value"]
        if d:
            drifted.append((n, old[n]["value"], new[n]["value"], d))
    drifted.sort(key=lambda t: abs(t[3]), reverse=True)
    print(f"old symbols: {len(old)}  new symbols: {len(new)}")
    print(f"shared: {len(shared)}  only-old: {len(only_old)}  only-new: {len(only_new)}")
    print(f"same value: {len(shared) - len(drifted)}  drifted: {len(drifted)}")
    if drifted:
        show = [(n, o, nw, d) for n, o, nw, d in drifted if (flt is None or flt in n.lower())]
        print(f"\ntop drifts{f' (filter={flt})' if flt else ''}:")
        for n, o, nw, d in show[:40]:
            print(f"  {n}\n     0x{o:08x} -> 0x{nw:08x}  delta={d:+d}")
    from collections import Counter
    hist = Counter(d for _, _, _, d in drifted)
    print("\ndelta histogram (top 8):")
    for d, c in hist.most_common(8):
        print(f"  delta={d:+d}: {c} symbols")
    print(f"\nonly-in-old (first 15): {only_old[:15]}")
    print(f"only-in-new (first 15): {only_new[:15]}")


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, argv = sys.argv[1], sys.argv[2:]
    {"dump": cmd_dump, "find": cmd_find, "diff": cmd_diff}[cmd](argv)


if __name__ == "__main__":
    main()
