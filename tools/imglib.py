# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""反汇编镜像(运行时 dump)的公共辅助: 定位镜像文件 + 读符号表。

镜像约定: 从 module_base 起的连续 VA dump, 因此"文件偏移 == 模块偏移"。
请用环境变量或命令行指定, 不再写死本机路径:

    TNT_IMG   镜像文件路径      (默认 ./dumps/tnt_img.bin)
    TNT_SYMS  符号表 JSON 路径  (默认 ./dumps/syms.json)

符号表可用 tools/mem_symtab.py 从同一份镜像里提取。
"""
from __future__ import annotations

import json
import os
from pathlib import Path

DEFAULT_IMG = "dumps/tnt_img.bin"
DEFAULT_SYMS = "dumps/syms.json"


def image_path(explicit: str | None = None) -> Path:
    p = explicit or os.environ.get("TNT_IMG") or DEFAULT_IMG
    path = Path(p)
    if not path.is_file():
        raise SystemExit(
            f"找不到镜像文件: {path}\n"
            f"请用 TNT_IMG=<path> 或 --img <path> 指定;\n"
            f"镜像 = 从 module_base 起的连续 VA dump (文件偏移 == 模块偏移)。")
    return path


def read_image(explicit: str | None = None) -> bytes:
    return image_path(explicit).read_bytes()


def symbols_path(explicit: str | None = None) -> Path:
    p = explicit or os.environ.get("TNT_SYMS") or DEFAULT_SYMS
    path = Path(p)
    if not path.is_file():
        raise SystemExit(
            f"找不到符号表: {path}\n"
            f"请用 TNT_SYMS=<path> 或 --syms <path> 指定;\n"
            f"可用 tools/mem_symtab.py parse <镜像> --out {DEFAULT_SYMS} 生成。")
    return path


def read_symbols(explicit: str | None = None) -> dict:
    return json.loads(symbols_path(explicit).read_text(encoding="utf-8"))
