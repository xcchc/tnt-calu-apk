# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""战斗校准 v2: 逐回合全对象 raw dump + tnt_memeat battle 快通道 + 截图。

用途: 定位"风力"字段(回合变化值)并验证 battle 子命令。
用法: uv run tools/calib_battle2.py --seconds 240 --tag run2
输出: $TNT_OUT/calib_<tag>_battle.log, calib_raw/<tag>_r<round>.bin,
      calib_shots/*.png, calib_<tag>_rawmeta.json
"""
from __future__ import annotations

import json
import struct
import sys
import time

from calib_common import BASE, OUT, dump, find_pid, rw_ranges, scan_qword, sh, shot
from core_offsets import MEMEAT, SINST_CONFIG, VT_CONSOLE, VT_PANEL


def u64(b: bytes, off: int) -> int:
    return struct.unpack_from("<Q", b, off)[0]


def main() -> None:
    args = sys.argv[1:]
    dur = int(args[args.index("--seconds") + 1]) if "--seconds" in args else 240
    tag = args[args.index("--tag") + 1] if "--tag" in args else "run2"
    shots = OUT / "calib_shots"
    shots.mkdir(parents=True, exist_ok=True)
    rawdir = OUT / "calib_raw"
    rawdir.mkdir(parents=True, exist_ok=True)

    pid = find_pid()
    if not pid:
        raise SystemExit("game not running")
    print(f"pid={pid} dur={dur}s tag={tag}", flush=True)
    ranges = rw_ranges(pid)
    needle_c = BASE + VT_CONSOLE + 16
    needle_p = BASE + VT_PANEL + 16

    console = panel = None
    t0 = time.time()
    last_shot = 0.0
    log = open(OUT / f"calib_{tag}_battle.log", "w", encoding="utf-8")
    raw_meta = []
    in_battle = False
    dumped_rounds = set()
    while time.time() - t0 < dur:
        if console is None:
            for a, b in ranges:
                console = scan_qword(pid, a, b, needle_c)
                if console:
                    break
            if console:
                for a, b in ranges:
                    panel = scan_qword(pid, a, b, needle_p)
                    if panel:
                        break
                print(f"[{time.time()-t0:6.1f}s] IN BATTLE console=0x{console:x}", flush=True)
                in_battle = True
        if console:
            line = sh(f"{MEMEAT} battle {pid} {BASE:x} {console:x} {panel or 0:x}").decode().strip()
            if not line or "vptr_ok=0" in line.split(" ")[0] or "console_unreadable" in line:
                console = None
                continue
            log.write(f"{time.time():.2f} {line}\n")
            log.flush()
            kv = dict(p.split("=", 1) for p in line.split() if "=" in p)
            rnd = kv.get("round")
            if rnd and rnd not in dumped_rounds:
                dumped_rounds.add(rnd)
                parts = {
                    "console": dump(pid, console, 0x240),
                    "panel": dump(pid, panel, 0xB0) if panel else b"",
                    "config_ptr": dump(pid, BASE + SINST_CONFIG, 8),
                }
                cfg = u64(parts["config_ptr"], 0) if len(parts["config_ptr"]) == 8 else 0
                if cfg:
                    parts["config"] = dump(pid, cfg, 0x600)
                for name, off in (("angle_obj", 0x60), ("force_obj", 0x100),
                                  ("self_obj", 0xA0), ("ctx_obj", 0xC8)):
                    obj = u64(parts["console"], off)
                    if obj:
                        parts[name] = dump(pid, obj, 0x220)
                blob = b"".join(parts[k] for k in ("console", "panel", "config_ptr", "config",
                                                   "angle_obj", "force_obj", "self_obj", "ctx_obj")
                                if k in parts)
                fn = rawdir / f"{tag}_r{rnd}.bin"
                fn.write_bytes(blob)
                raw_meta.append({"round": rnd, "file": str(fn), "console": hex(console),
                                 "panel": hex(panel) if panel else None, "cfg": hex(cfg),
                                 "t": round(time.time(), 2)})
                print(f"[{time.time()-t0:6.1f}s] round={rnd} raw->{fn.name} ({len(blob)}B)",
                      flush=True)
            if time.time() - last_shot > 3:
                last_shot = time.time()
                shot(f"{tag}_{int(last_shot)}", shots)
            time.sleep(0.4)
        else:
            if in_battle:
                print(f"[{time.time()-t0:6.1f}s] LEFT BATTLE", flush=True)
                in_battle = False
            time.sleep(0.4)
    log.close()
    (OUT / f"calib_{tag}_rawmeta.json").write_text(
        json.dumps(raw_meta, indent=1, ensure_ascii=False), encoding="utf-8")
    print("done", flush=True)


if __name__ == "__main__":
    main()
