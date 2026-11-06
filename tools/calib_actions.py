# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""校准 v3: BattleCommandAction order_data 通道(风) + roles 验证 + 截图。

旧版 memscan 的 actions 通道:
  command 对象 vptr = _ZTV19BattleCommandAction + 0x10,
  +0x0c first_order, +0x10 round, +0x14 speed, +0x18 state_hash, +0x20 order_data ptr。
order_data = BattleNotify_Action 的 protobuf 字节, field 4 = wind。

用法: uv run tools/calib_actions.py --seconds 300 --tag run5
输出: $TNT_OUT/calib_<tag>_actions.log + calib_shots/*.png
"""
from __future__ import annotations

import struct
import sys
import time

from calib_common import BASE, OUT, dump, find_pid, rw_ranges, scan_qword, sh, shot
from core_offsets import MEMEAT, VT_CONSOLE, VT_PANEL

VT_ACTION = 0x1545878  # _ZTV19BattleCommandAction


def main() -> None:
    args = sys.argv[1:]
    dur = int(args[args.index("--seconds") + 1]) if "--seconds" in args else 300
    tag = args[args.index("--tag") + 1] if "--tag" in args else "run5"
    shots = OUT / "calib_shots"
    shots.mkdir(parents=True, exist_ok=True)
    pid = find_pid()
    if not pid:
        raise SystemExit("game not running")
    print(f"pid={pid} dur={dur}s", flush=True)
    ranges = rw_ranges(pid, min_size=65536)
    needle_a = BASE + VT_ACTION + 16
    seen_actions: set[int] = set()
    t0 = time.time()
    last_shot = 0.0
    log = open(OUT / f"calib_{tag}_actions.log", "w", encoding="utf-8")
    dumped = 0
    while time.time() - t0 < dur:
        # 1) battle 快通道 (含 roles)
        cn = cp = None
        for a, b in ranges:
            cn = scan_qword(pid, a, b, BASE + VT_CONSOLE + 16, max_hits=16)
            if cn:
                break
        if cn:
            for a, b in ranges:
                cp = scan_qword(pid, a, b, BASE + VT_PANEL + 16, max_hits=16)
                if cp:
                    break
            line = sh(f"{MEMEAT} battle {pid} {BASE:x} {cn:x} {cp or 0:x}").decode().strip()
            log.write(f"BATTLE {time.time():.2f} {line}\n")
            # 2) actions 扫描
            for a, b in ranges:
                for hit in scan_qword_all(pid, a, b, needle_a):
                    if hit in seen_actions:
                        continue
                    seen_actions.add(hit)
                    raw = dump(pid, hit, 0x28)
                    if len(raw) < 0x28:
                        continue
                    first, rnd, speed = struct.unpack_from("<Iii", raw, 0x0C)
                    order = struct.unpack_from("<Q", raw, 0x20)[0]
                    od = dump(pid, order, 128) if order else b""
                    log.write(f"ACTION {time.time():.2f} obj=0x{hit:x} first={first} "
                              f"round={rnd} speed={speed} order=0x{order:x} "
                              f"bytes={od[:64].hex()}\n")
                    dumped += 1
            log.flush()
            if time.time() - last_shot > 3:
                last_shot = time.time()
                shot(f"{tag}_{int(last_shot)}", shots)
            time.sleep(0.8)
        else:
            time.sleep(0.4)
    log.close()
    print(f"done actions={dumped}", flush=True)


def scan_qword_all(pid: str, a: int, b: int, needle: int) -> list[int]:
    out = sh(f"{MEMEAT} scan {pid} {a:x} {b - a} {needle:x} 16").decode().split()
    return [int(x, 16) for x in out]


if __name__ == "__main__":
    main()
