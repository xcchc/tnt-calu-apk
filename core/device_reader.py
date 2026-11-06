# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""TNT 辅助设备侧采集器: 自动连接 ADB / root / 部署 memeat / 进战检测 + 高速状态流。

数据通道(只读):
  - 进程定位: adb pidof (无害 procfs)
  - 内存读取: tnt_memeat (process_vm_readv) —— 绝不读 /proc/PID/mem(反dump自杀)
  - 快通道:   tnt_memeat watch <pid> <hz_idle> <hz_battle>
              设备端常驻进程一次性完成: 找模块基址 + 全堆扫实例 + 逐帧读字段,
              单次 adb 调用流式输出 (实测 17Hz, 58ms/帧)。

用法:
  uv run core/device_reader.py --once          # 单次快照(json)
  uv run core/device_reader.py --loop          # 持续采集, JSONL 到 stdout
  uv run core/device_reader.py --loop --out work/state.jsonl
"""
from __future__ import annotations

import argparse
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Iterator

ROOT = Path(__file__).resolve().parent.parent


def _find_adb() -> str:
    """按优先级定位 adb: $TNT_ADB -> PATH -> 常见安装位置。"""
    env = os.environ.get("TNT_ADB")
    if env:
        return env
    found = shutil.which("adb")
    if found:
        return found
    for cand in (
        Path(os.environ.get("LOCALAPPDATA", "")) / "Android/Sdk/platform-tools/adb.exe",
        Path(os.environ.get("ANDROID_HOME", "")) / "platform-tools/adb.exe",
    ):
        if cand.is_file():
            return str(cand)
    return "adb"


ADB = _find_adb()
DEV = os.environ.get("TNT_ADB_SERIAL", "127.0.0.1:16384")
MEMEAT = "/data/local/tmp/tnt_memeat"
GAME_PKG = "com.grapefruit.game.tnt"
GAME_LIB = "libgame_arm64-v8a.so"


class DeviceError(RuntimeError):
    """ADB / root / 游戏进程不可用, 需要显示到悬浮层。"""


_serial: str | None = None
_memeat_size: int | None = None


def _run(args: list[str], timeout: float = 20.0) -> subprocess.CompletedProcess:
    try:
        return subprocess.run([ADB, *args], capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise DeviceError(f"ADB 执行失败: {exc}") from exc


def _decode(p: subprocess.CompletedProcess) -> str:
    return (p.stdout or b"").decode(errors="replace")


def _devices() -> list[str]:
    p = _run(["devices"], timeout=10)
    out = []
    for line in _decode(p).splitlines():
        if "\tdevice" in line:
            out.append(line.split("\t", 1)[0].strip())
    return out


def ensure_device() -> str:
    """确保有一个在线的 ADB 设备, 并已 root。失败抛 DeviceError。"""
    global _serial, _memeat_size
    if _serial:
        p = _run(["-s", _serial, "get-state"], timeout=5)
        if p.returncode == 0 and "device" in _decode(p):
            return _serial
        _serial = None
        _memeat_size = None

    devs = _devices()
    if not devs:
        if ":" in DEV:
            _run(["connect", DEV], timeout=8)
            time.sleep(1.2)
            devs = _devices()
        if not devs:
            raise DeviceError("ADB 未连接：请启动 MuMu 并确认 ADB 调试已开启")

    _serial = DEV if DEV in devs else devs[0]
    _memeat_size = None

    p = _run(["-s", _serial, "shell", "id"], timeout=8)
    if "uid=0" not in _decode(p):
        # MuMu 自带 adbd, `adb root` 后 adbd 会重启; 等它回来再确认。
        _run(["-s", _serial, "root"], timeout=15)
        time.sleep(1.5)
        _run(["-s", _serial, "wait-for-device"], timeout=15)
        p = _run(["-s", _serial, "shell", "id"], timeout=8)
        if "uid=0" not in _decode(p):
            raise DeviceError("模拟器无 root：无法读取游戏内存 (adb root 失败)")
    return _serial


def sh(cmd: str, timeout: float = 20.0) -> str:
    serial = ensure_device()
    return _decode(_run(["-s", serial, "shell", cmd], timeout=timeout))


def ensure_memeat() -> None:
    """本地二进制变化时自动 push 到设备, 避免“工具是旧版”的静默错误。"""
    global _memeat_size
    local = ROOT / "tools" / "tnt_memeat"
    if not local.exists():
        raise DeviceError(
            "缺少 tools/tnt_memeat。请先编译(见 tools/build_memeat.sh)或从 release 下载。")
    size = local.stat().st_size
    if _memeat_size == size:
        return
    serial = ensure_device()
    p = _run(["-s", serial, "shell", f"stat -c %s {MEMEAT} 2>/dev/null"], timeout=8)
    remote = _decode(p).strip()
    if remote != str(size):
        p = _run(["-s", serial, "push", str(local), MEMEAT], timeout=30)
        if p.returncode != 0:
            raise DeviceError("推送 tnt_memeat 失败: " + (p.stderr or b"").decode(errors="replace"))
        _run(["-s", serial, "shell", f"chmod 755 {MEMEAT}"], timeout=8)
    _memeat_size = size


def ensure_reverse(local_port: int = 8765, host_port: int = 28080) -> None:
    """悬浮层连接 127.0.0.1:8765 -> 主机 :28080。"""
    serial = ensure_device()
    p = _run(["-s", serial, "reverse", f"tcp:{local_port}", f"tcp:{host_port}"], timeout=10)
    if p.returncode != 0:
        raise DeviceError("adb reverse 失败: " + (p.stderr or b"").decode(errors="replace"))


def find_pid() -> str:
    out = sh(f"pidof {GAME_PKG}").split()
    if not out:
        return ""
    # 多进程包: 选真正加载了 libgame 的主进程(子进程读不到模块基址)
    for pid in out:
        n = sh(f"grep -c {GAME_LIB} /proc/{pid}/maps 2>/dev/null").strip()
        if n.isdigit() and int(n) > 0:
            return pid
    return out[0]


def _error(detail: str, message: str) -> dict:
    return {"ts": round(time.time(), 3), "state": "error",
            "detail": detail, "message": message}


def parse_line(line: str) -> dict | None:
    """解析 tnt_memeat 的一行输出。"""
    line = line.strip()
    if not line:
        return None
    kv: dict[str, str] = {}
    for tok in line.split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            kv[k] = v
    state = kv.get("state", "battle" if "vptr_ok" in kv else "")
    now = round(time.time(), 3)
    if state == "idle":
        return {"ts": now, "state": "idle", "detail": "not_in_battle",
                "base": kv.get("base")}
    if state in ("no_base", "no_maps"):
        return _error(state, "游戏尚未进入战斗或内存基址不可读")
    if state != "battle":
        return None
    if kv.get("vptr_ok") != "1":
        return _error("bad_vptr", "战斗对象校验失败(版本/基址不匹配)")

    weapon = int(kv.get("weapon", kv.get("angle", -999)))
    actual = int(kv.get("actual", weapon))
    st: dict = {
        "ts": now,
        "state": "battle",
        "round": int(kv.get("round", -1)),
        "can_fire": int(kv.get("canfire", 0)) == 1,
        "angle": actual,                 # 兼容旧字段: 盘面实际角度
        "actual_angle": actual,
        "weapon_angle": weapon,          # 武器角度 45..78
        "terrain_angle": float(kv.get("terrain", 0.0)),
        "facing": kv.get("facing"),
        "ang_max": int(kv.get("amax", 0)),   # 武器角度上限
        "ang_min": int(kv.get("amin", 0)),   # 武器角度下限
        "force": float(kv.get("force", -1)),
        "wind10": int(kv.get("wind", 0x7FFFFFFF)),
        "wind_text": kv.get("wind_txt", "-"),
        "console": kv.get("console"),
        "panel": kv.get("panel"),
    }
    if st["wind10"] in (0x7FFFFFFF, -0x80000000):
        st["wind10"] = None
    try:
        sx, sy = kv.get("self", "0,0").split(",")
        st["self"] = {"x": int(sx), "y": int(sy)}
    except ValueError:
        st["self"] = {"x": 0, "y": 0}
    st["self_uid"] = int(kv.get("self_uid", 0))
    raw_team = kv.get("self_team")
    if raw_team is None:
        st["self_team"] = None
    else:
        try:
            # C 端按 %llx 输出; 0 也是有效队伍(队伍 0/2 实测都存在)
            st["self_team"] = int(raw_team, 16)
        except ValueError:
            st["self_team"] = None
    st["roles"] = []
    for item in filter(None, kv.get("roles", "").split(",")):
        try:
            parts = item.split(":")
            # roles= 格式 (见 tools/tnt_memeat.c):
            #   uid:sprite_x:sprite_y:team:name:dead:hide:titan:role_x:role_y
            # sprite 取自 QGraphicsItemPrivate::pos, 由服务器轨迹动画逐帧驱动;
            # role 是 BattleRole+0x88 的服务器同步点, 对端隐身/飞行期间会冻结
            # (真机实测最大偏差 1142px)。所以 x,y 用 sprite, role 另存备查。
            uid, x, y = int(parts[0]), int(parts[1]), int(parts[2])
            team = int(parts[3], 16) if len(parts) >= 4 else 0
            name = ""
            if len(parts) >= 5 and parts[4] != "-":
                try:
                    name = bytes.fromhex(parts[4]).decode("utf-8", "replace")
                except ValueError:
                    name = ""
            dead = len(parts) >= 6 and int(float(parts[5])) != 0
            hide = len(parts) >= 7 and int(float(parts[6])) != 0
            titan = len(parts) >= 8 and int(float(parts[7])) != 0
            role_x = int(parts[8]) if len(parts) >= 10 else x
            role_y = int(parts[9]) if len(parts) >= 10 else y
        except (ValueError, IndexError):
            continue
        # 过滤重建过程中的残留节点: uid 非法 / 坐标明显越界
        if uid <= 0 or uid >= 0xFFFFFF00:
            continue
        if not (-20000 <= x <= 20000 and -20000 <= y <= 20000):
            continue
        st["roles"].append({"uid": uid, "x": x, "y": y, "team": team,
                            "name": name, "dead": dead, "hide": hide,
                            "titan": titan, "role_x": role_x,
                            "role_y": role_y})
    return st


def stream(hz_idle: float = 2.0, hz_battle: float = 30.0,
           stall_s: float = 5.0) -> Iterator[dict]:
    """无限流: 自动处理 ADB 掉线 / 游戏重启 / watch 卡死(看门狗)。"""
    last_err = ""
    while True:
        try:
            ensure_device()
            ensure_memeat()
        except DeviceError as exc:
            msg = str(exc)
            if msg != last_err:
                print(f"[device] {msg}", file=sys.stderr, flush=True)
                last_err = msg
            yield _error("adb", msg)
            time.sleep(2.0)
            continue

        pid = find_pid()
        if not pid:
            msg = "游戏未运行：请启动 TNT"
            if msg != last_err:
                print(f"[device] {msg}", file=sys.stderr, flush=True)
                last_err = msg
            yield _error("no_pid", msg)
            time.sleep(1.0)
            continue

        cmd = [ADB, "-s", _serial or DEV, "shell", f"{MEMEAT} watch {pid} {hz_idle} {hz_battle}"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, bufsize=0)
        q: queue.Queue[bytes | None] = queue.Queue()

        def _pump(p=proc, qq=q) -> None:
            try:
                if p.stdout is None:
                    return
                for raw in iter(p.stdout.readline, b""):
                    qq.put(raw)
            except (OSError, ValueError):
                pass
            finally:
                qq.put(None)

        threading.Thread(target=_pump, daemon=True).start()
        last_pid_check = time.monotonic()
        try:
            while True:
                try:
                    raw = q.get(timeout=stall_s)
                except queue.Empty:
                    # 卡死: 杀掉设备端 watch 重新拉起(避免悬浮层画面冻结在旧数据)
                    yield {"ts": round(time.time(), 3), "state": "stall",
                           "detail": "watch_stalled", "message": "内存读取卡住，正在重连..."}
                    break
                if raw is None:
                    break
                st = parse_line(raw.decode(errors="replace"))
                if st is not None:
                    st["pid"] = pid
                    yield st
                # 游戏重启后 PID 会变; 旧 watch 仍绑定旧进程, 会一直吐 idle/旧数据。
                now = time.monotonic()
                if now - last_pid_check >= 1.0:
                    last_pid_check = now
                    cur = find_pid()
                    if cur and cur != pid:
                        print(f"[device] 游戏 PID 变化 {pid} -> {cur}, 重启读取", file=sys.stderr, flush=True)
                        break
        finally:
            try:
                proc.kill()
            except OSError:
                pass
            time.sleep(0.3)


def read_once() -> dict:
    try:
        ensure_device()
        ensure_memeat()
    except DeviceError as exc:
        return _error("adb", str(exc))
    pid = find_pid()
    if not pid:
        return _error("no_pid", "游戏未运行：请启动 TNT")
    st = parse_line(sh(f"{MEMEAT} state {pid}"))
    if st is None:
        return _error("parse_failed", "内存读取输出无法解析")
    st["pid"] = pid
    return st


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--loop", action="store_true")
    ap.add_argument("--out", default="-")
    ap.add_argument("--hz-idle", type=float, default=2.0)
    ap.add_argument("--hz-battle", type=float, default=20.0)
    args = ap.parse_args()

    if args.once:
        print(json.dumps(read_once(), ensure_ascii=False, indent=1))
        return
    if args.loop:
        fh = sys.stdout if args.out == "-" else open(args.out, "a", encoding="utf-8")
        try:
            for st in stream(args.hz_idle, args.hz_battle):
                fh.write(json.dumps(st, ensure_ascii=False) + "\n")
                fh.flush()
        except KeyboardInterrupt:
            pass
        finally:
            if fh is not sys.stdout:
                fh.close()
        return
    ap.print_help()


if __name__ == "__main__":
    main()
