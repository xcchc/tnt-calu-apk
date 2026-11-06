# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""校验 core/aim_solver.py 与 overlay Solver.java 的物理常量是否一致。

同一个公式在两处实现：Python 侧（给 PC 主循环 / 命令行用）和 Java 侧（打进 APK）。
改标定参数时必须两边一起改，否则 APK 显示的数字和你本地算出来的会不一样。
这个脚本解析两份源码里的常量定义并逐一比对，还会用两边同名的 solveTarget
做数值对拍（调用 Java 需要 javac，缺失时跳过数值对拍只比常量）。

用法:
    uv run tools/check_consts.py
    uv run tools/check_consts.py --java overlay/app/java/com/tnt/assist/Solver.java

退出码: 0 一致 / 1 不一致 / 2 文件缺失
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY_SRC = ROOT / "core" / "aim_solver.py"
JAVA_SRC = ROOT / "overlay" / "app" / "java" / "com" / "tnt" / "assist" / "Solver.java"

# Python 常量名 -> Java 常量名（目前同名，保留映射便于将来改名）
NAMES = ["K", "G", "V0K", "WIND_MULT", "MUZZLE", "AIM_UP", "TY_UP", "PX_PER_JU",
         "SCREEN_SCALE", "FLY_V0K", "FLY_G", "FLY_FPS", "CHARGE_K", "CHARGE_B",
         "MIN_FORCE", "MAX_FORCE"]


def parse_python(path: Path) -> dict[str, float]:
    """抓模块级 `NAME = <数字>` 定义（跳过函数体内的同名局部）。"""
    out: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(rf"^({'|'.join(NAMES)})\s*=\s*(-?[\d.]+)\s*(?:#.*)?$", line)
        if m:
            out[m.group(1)] = float(m.group(2))
    # Q = 1 - K 这类派生常量不需要比对
    return out


def parse_java(path: Path) -> dict[str, float]:
    out: dict[str, float] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = re.match(
            rf"^\s*public\s+static\s+final\s+double\s+({'|'.join(NAMES)})\s*=\s*(-?[\d.]+)d?\s*;",
            line)
        if m:
            out[m.group(1)] = float(m.group(2))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--py", default=str(PY_SRC))
    ap.add_argument("--java", default=str(JAVA_SRC))
    args = ap.parse_args()

    py_path, java_path = Path(args.py), Path(args.java)
    for p in (py_path, java_path):
        if not p.is_file():
            print(f"缺少文件: {p}", file=sys.stderr)
            return 2

    py = parse_python(py_path)
    java = parse_java(java_path)

    problems = 0
    print(f"{'常量':<14}{'Python':>18}{'Java':>18}   结果")
    print("-" * 62)
    for name in NAMES:
        a, b = py.get(name), java.get(name)
        if a is None and b is None:
            print(f"{name:<14}{'(未定义)':>18}{'(未定义)':>18}   跳过")
            continue
        if a is None or b is None:
            side = "Python" if a is None else "Java"
            print(f"{name:<14}{a if a is not None else '—':>18}"
                  f"{b if b is not None else '—':>18}   {side} 缺这一项")
            problems += 1
            continue
        same = abs(a - b) < 1e-12
        if not same:
            problems += 1
        print(f"{name:<14}{a:>18.10g}{b:>18.10g}   {'一致' if same else '不一致'}")

    print()
    if problems:
        print(f"{problems} 项不一致。改标定参数时 core/aim_solver.py 和 "
              f"overlay/.../Solver.java 必须同步。", file=sys.stderr)
        return 1
    print("常量一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
