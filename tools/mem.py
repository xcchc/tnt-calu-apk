"""Read the game's memory over adb using the on-device tnt_memeat probe.

usage (library):
    from tools.mem import Mem
    m = Mem("127.0.0.1:16384")
    pid, base = m.attach()
    panel = m.u64(console + 0xa0)

The probe binary must be deployed at /data/local/tmp/tnt_memeat and the
device must be rooted (`adb root`).
"""
from __future__ import annotations

import os
import shutil
import struct
import subprocess


def _find_adb() -> str:
    """按优先级定位 adb: $TNT_ADB -> PATH -> 默认名。"""
    env = os.environ.get("TNT_ADB")
    if env:
        return env
    return shutil.which("adb") or "adb"


ADB = _find_adb()
PROBE = "/data/local/tmp/tnt_memeat"
PKG = "com.grapefruit.game.tnt"


class Mem:
    def __init__(self, serial: str, probe: str = PROBE) -> None:
        self.serial = serial
        self.probe = probe
        self.pid: int | None = None
        self.base: int | None = None

    # ---------------- plumbing ----------------
    def sh(self, cmd: str, timeout: int = 60) -> str:
        p = subprocess.run([ADB, "-s", self.serial, "shell", cmd],
                           capture_output=True, text=True, errors="replace",
                           timeout=timeout)
        return (p.stdout or "") + (p.stderr or "")

    def raw(self, addr: int, size: int, timeout: int = 60) -> bytes:
        p = subprocess.run([ADB, "-s", self.serial, "exec-out",
                            f"{self.probe} dump {self.pid} {addr:x} {size}"],
                           capture_output=True, timeout=timeout)
        out = p.stdout or b""
        if len(out) != size:
            raise RuntimeError(f"short read {len(out)}/{size} at {addr:#x}")
        return out

    def attach(self) -> tuple[int, int]:
        out = self.sh(f"pidof {PKG}").strip().split()
        if not out:
            raise RuntimeError("game not running")
        self.pid = int(out[0])
        st = self.sh(f"{self.probe} state {self.pid}").strip()
        # state=idle base=400023600000 console=... panel=...
        base = None
        for tok in st.split():
            if tok.startswith("base="):
                base = int(tok.split("=", 1)[1], 16)
        self.base = base
        self._state = st
        return self.pid, base

    # ---------------- typed reads ----------------
    def read(self, addr: int, size: int) -> bytes:
        return self.raw(addr, size)

    def u64(self, addr: int) -> int:
        return struct.unpack("<Q", self.raw(addr, 8))[0]

    def i32(self, addr: int) -> int:
        return struct.unpack("<i", self.raw(addr, 4))[0]

    def u32(self, addr: int) -> int:
        return struct.unpack("<I", self.raw(addr, 4))[0]

    def f64(self, addr: int) -> float:
        return struct.unpack("<d", self.raw(addr, 8))[0]

    def f32(self, addr: int) -> float:
        return struct.unpack("<f", self.raw(addr, 4))[0]

    def qpointf(self, addr: int) -> tuple[float, float]:
        b = self.raw(addr, 16)
        return struct.unpack("<dd", b)

    def qpoint(self, addr: int) -> tuple[int, int]:
        b = self.raw(addr, 8)
        return struct.unpack("<ii", b)

    def qstring(self, addr: int) -> str:
        """QString (Qt5): d_ptr at +0, QStringData: ref, size, data[]. UTF-16."""
        d = self.u64(addr)
        if not d:
            return ""
        size = struct.unpack("<i", self.raw(d + 4, 4))[0]
        if size <= 0 or size > 200:
            return ""
        return self.raw(d + 16, size * 2).decode("utf-16-le", "replace")

    def ptr_chain(self, addr: int, *offsets: int) -> int:
        p = addr
        for o in offsets:
            p = self.u64(p + o)
        return p

    # ---------------- analysis helpers ----------------
    def find_qword(self, value: int, maxhits: int = 0, timeout: int = 300) -> list[int]:
        out = self.sh(f"{self.probe} findq {self.pid} {value:x} {maxhits}", timeout=timeout)
        hits = []
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("0x") or (line and all(c in "0123456789abcdef" for c in line)):
                try:
                    hits.append(int(line, 16))
                except ValueError:
                    pass
        return hits

    def dump_words(self, addr: int, nwords: int) -> list[int]:
        """Dump n qwords and return them as ints (via dumpq text output)."""
        out = self.sh(f"{self.probe} dumpq {self.pid} {addr:x} {nwords}")
        vals = []
        for line in out.splitlines():
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "q":
                vals.append(int(parts[2], 16))
        return vals


def describe(addr: int, data: bytes) -> str:
    qs = [struct.unpack_from("<Q", data, i)[0] for i in range(0, len(data), 8)]
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        hexs = " ".join(f"{b:02x}" for b in chunk)
        i32 = struct.unpack_from("<ii", chunk, 0) if len(chunk) == 16 else None
        f32 = struct.unpack_from("<ff", chunk, 0) if len(chunk) == 16 else None
        f64 = struct.unpack_from("<d", chunk, 0)[0] if len(chunk) == 16 else 0.0
        lines.append(f"{addr + i:012x}  {hexs:<47s} i32={i32} f32={f32} f64={f64:.4f}")
    return "\n".join(lines)
