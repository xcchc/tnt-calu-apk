# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""战斗现场动态采集器: 定位 console/panel 实例并逐帧转储关键字段。

用法:
    uv run tools/calib_battle.py [--seconds 120] [--tag run1]

流程:
1. 每 400 ms 扫 BattleConsole vptr(base + _ZTV13BattleConsole + 0x10), 命中即进战斗;
2. 命中后 500 ms 周期采集: console 字段 + angle/force/self/role 下钻 + Config 风 + PanelBattle 角色表;
3. 每 3 s 通过 adb screencap 存对照截图到 $TNT_OUT/calib_shots/;
4. console 消失即出战; 全部数据写 $TNT_OUT/calib_<tag>.json。

只读不写, 走 tnt_memeat(process_vm_readv), 不触碰 /proc/pid/mem(反dump自杀)。
"""
from __future__ import annotations

import json
import struct
import sys
import time

from calib_common import BASE, OUT, dump, find_pid, rw_ranges, scan_qword, sh, shot
from core_offsets import SINST_CONFIG, VT_CONSOLE, VT_PANEL


def u64(b: bytes, off: int) -> int:
    return struct.unpack_from("<Q", b, off)[0]


def i32(b: bytes, off: int) -> int:
    return struct.unpack_from("<i", b, off)[0]


def f64(b: bytes, off: int) -> float:
    return struct.unpack_from("<d", b, off)[0]


def sample(pid: str, console: int, panel: int | None) -> dict:
    s: dict = {"t": round(time.time(), 2)}
    raw = dump(pid, console, 0x240)
    if len(raw) < 0x240:
        s["error"] = "console unreadable"
        return s
    s["console_vptr_ok"] = u64(raw, 0) == BASE + VT_CONSOLE + 16
    s["c28_round"] = i32(raw, 0x28)
    s["c21_canfire"] = raw[0x21]
    angle_obj = u64(raw, 0x60)
    force_obj = u64(raw, 0x100)
    self_obj = u64(raw, 0xA0)
    ctx_obj = u64(raw, 0xC8)
    s["objs"] = {k: hex(v) for k, v in
                 dict(angle=angle_obj, force=force_obj, self=self_obj, ctx=ctx_obj).items()}
    if angle_obj:
        b = dump(pid, angle_obj, 0x120)
        if len(b) >= 0xB0:
            s["angle_cand"] = {hex(o): i32(b, o) for o in (0x98, 0xA0, 0xA8)}
    if force_obj:
        b = dump(pid, force_obj, 0x40)
        if len(b) >= 0x28:
            s["force_cand"] = {hex(o): round(f64(b, o), 5) for o in (0x18, 0x20, 0x28)}
    if self_obj:
        b = dump(pid, self_obj, 0x220)
        if len(b) >= 0x1E0:
            role = u64(b, 0xC8)
            s["self_role"] = hex(role)
            s["self_ptrs"] = {hex(o): hex(u64(b, o)) for o in (0x80, 0x198, 0x1A0, 0x1B0, 0x1D8)}
            if 0x7000_0000_0000 <= role <= 0x7FFF_FFFF_FFFF:
                r = dump(pid, role, 0xC0)
                if len(r) >= 0x90:
                    s["role_cand"] = {hex(o): i32(r, o) for o in (0x60, 0x64, 0x68, 0x6C, 0x88, 0x8C)}
    cfg = dump(pid, BASE + SINST_CONFIG, 8)
    if len(cfg) == 8:
        cfg_obj = u64(cfg, 0)
        s["config"] = hex(cfg_obj)
        if cfg_obj:
            b = dump(pid, cfg_obj + 0x450, 0x30)
            if len(b) == 0x30:
                s["wind_cand"] = {hex(0x450 + o): i32(b, o) for o in range(0, 0x30, 8)}
    if panel:
        pb = dump(pid, panel, 0xB0)
        if len(pb) >= 0x98:
            s["panel_vptr_ok"] = u64(pb, 0) == BASE + VT_PANEL + 16
            hash_d = u64(pb, 0x90)
            s["panel_hash_d"] = hex(hash_d)
            if hash_d:
                hd = dump(pid, hash_d, 0x28)
                if len(hd) == 0x28:
                    s["hash_hsize"] = i32(hd, 0x14)
                    s["hash_nbuckets"] = i32(hd, 0x20)
    return s


def main() -> None:
    args = sys.argv[1:]
    dur = int(args[args.index("--seconds") + 1]) if "--seconds" in args else 120
    tag = args[args.index("--tag") + 1] if "--tag" in args else "run1"
    shots = OUT / "calib_shots"
    shots.mkdir(parents=True, exist_ok=True)

    pid = find_pid()
    if not pid:
        raise SystemExit("game not running")
    print(f"pid={pid} dur={dur}s tag={tag}")
    ranges = rw_ranges(pid)
    needle_c = BASE + VT_CONSOLE + 16
    needle_p = BASE + VT_PANEL + 16

    samples: list[dict] = []
    console = panel = None
    t0 = time.time()
    last_shot = 0.0
    in_battle = False
    while time.time() - t0 < dur:
        if console is None or (samples and not samples[-1].get("console_vptr_ok", True)):
            console = None
            for a, b in ranges:
                console = scan_qword(pid, a, b, needle_c)
                if console:
                    break
            if console:
                for a, b in ranges:
                    panel = scan_qword(pid, a, b, needle_p)
                    if panel:
                        break
                print(f"[{time.time()-t0:6.1f}s] IN BATTLE console=0x{console:x} "
                      f"panel={hex(panel) if panel else None}")
                in_battle = True
        if console:
            samples.append(sample(pid, console, panel))
            if time.time() - last_shot > 3:
                last_shot = time.time()
                shot(f"{tag}_{int(last_shot)}", shots)
            time.sleep(0.5)
        else:
            if in_battle:
                print(f"[{time.time()-t0:6.1f}s] LEFT BATTLE")
                in_battle = False
            time.sleep(0.4)

    out = OUT / f"calib_{tag}.json"
    out.write_text(json.dumps(samples, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"{len(samples)} samples -> {out}")


if __name__ == "__main__":
    main()
