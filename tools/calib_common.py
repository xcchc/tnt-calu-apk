# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""校准采集的公共部分: adb 定位 + 设备连接 + 采样输出目录。

环境变量:
    TNT_ADB           adb 可执行文件路径 (默认: PATH 里的 adb)
    TNT_ADB_SERIAL    设备 serial (默认 127.0.0.1:16384)
    TNT_OUT           采样输出目录 (默认 ./out)

采集流程本身见 calib_battle.py / calib_battle2.py / calib_actions.py。
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from core_offsets import MEMEAT, PKG

# 重建目标版本的模块基址 (每次启动会变, 这里只是首扫的起点提示)
BASE = 0x400023600000


def find_adb() -> str:
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


ADB = find_adb()
DEV = os.environ.get("TNT_ADB_SERIAL", "127.0.0.1:16384")
OUT = Path(os.environ.get("TNT_OUT", "out"))


def sh(cmd: str) -> bytes:
    return subprocess.run([ADB, "-s", DEV, "shell", cmd], capture_output=True).stdout


def find_pid() -> str:
    out = sh(f"pidof {PKG}").decode().split()
    return out[0] if out else ""


def rw_ranges(pid: str, min_size: int = 4096,
              max_size: int = 100 * 1024 * 1024) -> list[tuple[int, int]]:
    """可读写的匿名 region (排除 .so 等文件映射)。"""
    out = []
    for line in sh(f"cat /proc/{pid}/maps").decode(errors="replace").splitlines():
        parts = line.split()
        if len(parts) < 2 or "rw-p" not in line or ".so" in line:
            continue
        a, b = parts[0].split("-")
        lo, hi = int(a, 16), int(b, 16)
        if min_size < hi - lo < max_size:
            out.append((lo, hi))
    return out


def scan_qword(pid: str, a: int, b: int, needle: int, max_hits: int = 4) -> int | None:
    out = sh(f"{MEMEAT} scan {pid} {a:x} {b - a} {needle:x} {max_hits}").decode().split()
    return int(out[0], 16) if out else None


def dump(pid: str, addr: int, size: int) -> bytes:
    return sh(f"{MEMEAT} dump {pid} {addr:x} {size}")


def shot(tag: str, out_dir: Path | None = None) -> Path | None:
    """截屏并 pull 到本地, 返回本地路径。"""
    d = out_dir or OUT
    d.mkdir(parents=True, exist_ok=True)
    remote = "/data/local/tmp/calib.png"
    sh(f"screencap -p {remote}")
    local = d / f"{tag}.png"
    p = subprocess.run([ADB, "-s", DEV, "pull", remote, str(local)],
                       capture_output=True)
    return local if p.returncode == 0 else None
