# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""服务器侧弹道模块 —— 与 core/aim_solver.py / overlay Solver.java 必须完全一致。

三处常量一致性由 tools/check_consts.py 自动校验(读源码正则比对), 改动任一处必须同步。

物理模型(v261016.1, 2026-11-02 用 39 个服务器权威弹道点重拟合):
  A(n) = (1-Q^n)/K, B(n) = n - A(n), Q = 1-K
  x(n) = xm + vx0*A + w*B,  y(n) = ym + vy0*A + (G/K)*B
  vx0 = V0K*f*cos(th), vy0 = -V0K*f*sin(th), w = WIND_MULT*wind_action
  muzzle = pos + MUZZLE*(cos th, -sin th);  y 轴向下
  f = force/100;  瞄准点 = 对手站立点(脚下), AIM_UP = 0
残差: 出厂常量 6.45 px rms -> 本组 2.07 px rms(单发最优 1.01 px)。
"""
from __future__ import annotations

import math

# ---- 与 core/aim_solver.py 完全相同的常量 ----
K = 0.0186951412
G = 4.28938354
V0K = 144.787175
WIND_MULT = 1.46643562
MUZZLE = 28.0
AIM_UP = 0.0
TY_UP = 20.0
PX_PER_JU = 82.5
SCREEN_SCALE = 1.98
FPS = 30.0
Q = 1.0 - K

FLY_V0K = 105.64
FLY_G = 2.28
FLY_FPS = 30.0

CHARGE_K = 0.1990
CHARGE_B = -0.0306

MIN_FORCE = 5.0
MAX_FORCE = 100.0


def point(x0: float, y0: float, theta_deg: float, force_pct: float, wind_action: float,
          n: float) -> tuple[float, float]:
    """第 n 帧弹道点(y 向下)。x0,y0 为出膛点。"""
    th = math.radians(theta_deg)
    f = max(0.0, min(1.0, force_pct / 100.0))
    vx0 = V0K * f * math.cos(th)
    vy0 = -V0K * f * math.sin(th)
    w = WIND_MULT * wind_action
    A = (1.0 - Q ** n) / K
    B = n - A
    return x0 + vx0 * A + w * B, y0 + vy0 * A + (G / K) * B


def muzzle_point(x: float, y: float, theta_deg: float) -> tuple[float, float]:
    th = math.radians(theta_deg)
    return x + MUZZLE * math.cos(th), y - MUZZLE * math.sin(th)


def solve_target(theta_deg: float, dist_ju: float, height_ju: float = 0.0,
                 wind: float = 0.0) -> float | None:
    """给定角度求命中站立点所需力度(%); 无解返回 None。"""
    th = math.radians(theta_deg)
    x0 = MUZZLE * math.cos(th)
    y0 = -MUZZLE * math.sin(th)
    tx = dist_ju * PX_PER_JU
    ty = -(height_ju * PX_PER_JU + AIM_UP)
    w = WIND_MULT * wind
    t = math.tan(th)

    def phi(n: float) -> float:
        A = (1.0 - Q ** n) / K
        B = n - A
        return y0 - t * (tx - x0 - w * B) + (G / K) * B - ty

    lo, hi = 0.05, 3000.0
    flo, fhi = phi(lo), phi(hi)
    if flo * fhi > 0:
        return None
    for _ in range(200):
        mid = (lo + hi) / 2
        fm = phi(mid)
        if flo * fm <= 0:
            hi, fhi = mid, fm
        else:
            lo, flo = mid, fm
    n = (lo + hi) / 2.0
    A = (1.0 - Q ** n) / K
    denom = V0K * math.cos(th) * A
    if abs(denom) < 1e-9:
        return None
    return (tx - x0 - w * (n - A)) / denom * 100.0


def fly_force_for_range(angle_deg: float, dist_px: float) -> float | None:
    th = math.radians(angle_deg)
    s = math.sin(2 * th)
    if abs(s) < 1e-9 or dist_px <= 0:
        return None
    v2 = dist_px * FLY_G / s
    if v2 <= 0:
        return None
    return math.sqrt(v2) / FLY_V0K


def hold_for_force(force01: float) -> float:
    f = max(0.0, min(1.0, force01))
    return max(0.0, (f - CHARGE_B) / CHARGE_K)


def pick(self_screen, tap_screen, angle_deg: float, wind_world: float = 0.0,
         scale: float = SCREEN_SCALE) -> dict:
    """点选打点: 屏幕"我脚下"+"目标点" -> 力度/按住秒数/飞行力度。"""
    dxw = (tap_screen[0] - self_screen[0]) / scale
    dyw = (tap_screen[1] - self_screen[1]) / scale
    right = dxw > 0
    w = wind_world if right else -wind_world
    dist_ju = abs(dxw) / PX_PER_JU
    height_ju = -dyw / PX_PER_JU
    f = solve_target(angle_deg, dist_ju, height_ju, w)
    straight = math.hypot(dxw, dyw)
    fly = fly_force_for_range(angle_deg, straight)
    return {
        "angle": angle_deg,
        "force": None if f is None else round(f, 1),
        "hold_s": None if f is None else round(hold_for_force(f / 100.0), 2),
        "fly_force": None if fly is None else round(fly * 100.0, 1),
        "fly_hold_s": None if fly is None else round(hold_for_force(fly), 2),
        "facing": "right" if right else "left",
        "dist_ju": round(dist_ju, 2),
        "height_ju": round(height_ju, 2),
        "dist_px": round(abs(dxw), 1),
        "wind_action": round(w, 2),
    }


def selfcheck() -> None:
    """与 core/aim_solver.py 对比(需能 import core)。"""
    import sys
    sys.path.insert(0, ".")
    from core import aim_solver as A  # noqa: PLC0415

    for name in ("K", "G", "V0K", "WIND_MULT", "MUZZLE", "AIM_UP", "TY_UP", "PX_PER_JU",
                 "SCREEN_SCALE", "FLY_V0K", "FLY_G", "CHARGE_K", "CHARGE_B"):
        a, b = getattr(A, name), globals()[name]
        assert abs(a - b) < 1e-12, f"{name}: {a} != {b}"
    for th in (20, 35, 45, 55, 70):
        for d in (1.0, 5.0, 12.0, 20.0):
            for h in (-3.0, 0.0, 3.0):
                x = A.solve_target(th, d, h, 4.0)
                y = solve_target(th, d, h, 4.0)
                assert (x is None) == (y is None)
                if x is not None:
                    assert abs(x - y) < 1e-9, (th, d, h, x, y)
    print("ballistics.py == aim_solver.py: OK")


if __name__ == "__main__":
    selfcheck()
