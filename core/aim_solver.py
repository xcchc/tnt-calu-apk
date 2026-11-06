# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""TNT 瞄准解算器(Phase 3): 角度候选表 -> 命中力度。

物理模型(v260906.1 实战标定, Phase 5 重校准):
  - 空气阻力: 每帧速度 x Q (Q = 1-K)
  - 弹道(离散帧 30fps): x(n) = x0 + vx0*A(n) + w*B(n);  y(n) = y0 + vy0*A(n) + (G/K)*B(n)
    A(n) = (1-Q^n)/K, B(n) = n - A(n)
  - v0 = V0K * force(0..1); 风加速度 = WIND_MULT * wind_action(游戏风力, 顺射向为正)
  - 出膛点 = 角色 + 28px 沿炮管; 碰撞中心 = 脚底上 20px(y-up)

输入约定(与 device_reader 对接):
  - 自/敌世界坐标 (x, y): role+0x88/0x8C, y 轴方向与单位换算见 SCALE 常量(Phase 5 标定)
  - wind_action: 当前回合风值(正=顺当前射向)

用法:
  uv run core/aim_solver.py --dist 12 --height 0 --wind 4.3
  uv run core/aim_solver.py --self 148,175 --enemy 320,140 --wind -7.4 --facing right
"""
from __future__ import annotations

import argparse
import json
import math

# ---- 物理常量(v261016.1 服务器权威弹道重拟合 2026-11-02) ----
# 39 个服务器弹道点重拟合: 出厂常量 rms 6.45 px -> 本组常量 rms 2.07 px(单发最优 1.01 px)
# 拟合样本: battle4 (角度24, 力度35.94/56.26, 风+7.7/+9.6) + battle6 (角度36, 按住2.0s=力度35.9)
K = 0.0186951412
G = 4.28938354
V0K = 144.787175
WIND_MULT = 1.46643562
MUZZLE = 28.0
AIM_UP = 0.0          # 瞄准点 = 对手站立点(脚下), 不再抬高
TY_UP = 20.0          # 命中判定中心 = 脚底上 20px(仅记录, 不参与解算)
PX_PER_JU = 82.5      # 1 屏距 = 82.5 屏幕px (显示侧; 世界单位换算见 SCALE)
SCREEN_SCALE = 1.98  # 屏幕px / 世界单位(竞技模式实测)
FPS = 30.0
Q = 1.0 - K

# 世界坐标 -> 屏幕px / 距 的换算(Phase 5 动态标定, 默认值假设世界单位=px)
SCALE = {
    "world_px_per_unit": 1.0,   # 1 世界单位 = 多少屏幕px(缩放相关)
    "px_per_ju": PX_PER_JU,     # 1 距 = 多少屏幕px(缩放相关, 运行时自校准)
    "y_up": False,              # 角色坐标 y 轴向下(屏幕系): 我 y 更小 = 我站得更高
}

# 候选角度表(竞技模式拨盘实测范围 25..55, 见 amax/amin)
CANDIDATE_ANGLES = [25.0, 30.0, 35.0, 40.0, 45.0, 50.0, 55.0]


def point(x0: float, y0: float, theta_deg: float, force_pct: float, wind_action: float, n: float):
    """第 n 帧弹道点(y-down 屏幕坐标)。"""
    th = math.radians(theta_deg)
    f = max(0.0, min(1.0, force_pct / 100.0))
    vx0 = V0K * f * math.cos(th)
    vy0 = -V0K * f * math.sin(th)
    w = WIND_MULT * wind_action
    A = (1.0 - Q ** n) / K
    B = n - A
    return x0 + vx0 * A + w * B, y0 + vy0 * A + (G / K) * B


def land(x0: float, y0: float, theta_deg: float, force_pct: float, wind_action: float,
         ground_y: float, nmax: int = 3000):
    px, py = x0, y0
    for n in range(1, nmax):
        x, y = point(x0, y0, theta_deg, force_pct, wind_action, n)
        if y >= ground_y:
            frac = (ground_y - py) / (y - py) if y != py else 1.0
            return px + (x - px) * frac, n
        px, py = x, y
    return px, nmax


def track(x0: float, y0: float, theta_deg: float, force_pct: float, wind_action: float,
          ground_y: float, nmax: int = 3000, samples: int = 300):
    out = []
    for n in range(0, nmax):
        x, y = point(x0, y0, theta_deg, force_pct, wind_action, n)
        out.append((x, y))
        if y >= ground_y:
            break
    step = max(1, len(out) // samples)
    return out[::step]


# ================= 喷气背包(飞行)弹道 =================
#
# 真机双账号 (v261016.1, 马戏团地图) 实测 8 条完整轨迹拟合得出:
# 速度方向吻合残差 0.0~1.0 px/帧, 速度标定残差 rms 0.65 px/帧。
#
# 与"发射子弹"完全是另一套公式, 四点不同:
#   1. 没有空气阻力 —— 子弹是 K=0.0186951 的指数阻尼, 飞行是纯抛物线 (K=0)
#   2. 不受风力影响 —— 实测风 +1156 与 -1374 的样本残差无相关 (r=+0.22)
#   3. 初速更小     —— FLY_V0K=105.64, 子弹 V0K=144.787 (0.73 倍)
#   4. 重力更小     —— FLY_G=2.28, 子弹 G=4.28938 (0.532 倍)
#
# 发射角就是拨盘上的 actual 角(= weapon + terrain), 不需要再补偿。
#   v    = FLY_V0K * force             force 取 0..1
#   vx   = v*cos(theta);  vy = -v*sin(theta)   (屏幕 y 向下为正)
#   x(n) = x0 + vx*n
#   y(n) = y0 + vy*n + 0.5*FLY_G*n*n           n = 帧 (30fps)
FLY_V0K = 105.64
FLY_G = 2.28
FLY_FPS = 30.0

# 力度充电(按住发射键): 实测 force = 0.1940 * 按住秒数 - 0.0294
CHARGE_K = 0.1990
CHARGE_B = -0.0306


def fly_force_from_hold(hold_s: float) -> float:
    """按住 hold_s 秒会得到多大力度 (0..1)。"""
    return max(0.0, min(1.0, CHARGE_K * hold_s + CHARGE_B))


def fly_hold_for_force(force01: float) -> float:
    """要得到这个力度 (0..1) 需要按住多少秒。"""
    f = max(0.0, min(1.0, force01))
    return max(0.0, (f - CHARGE_B) / CHARGE_K)


def fly_full_power_hold() -> float:
    """满力需要的按住时长(秒)。"""
    return (1.0 - CHARGE_B) / CHARGE_K


def fly_step(angle_deg: float, force01: float, n: float) -> tuple[float, float]:
    """第 n 帧相对起飞点的位移 (dx, dy), 屏幕 y 向下为正。"""
    th = math.radians(angle_deg)
    v = FLY_V0K * force01
    vx, vy = v * math.cos(th), -v * math.sin(th)
    return vx * n, vy * n + 0.5 * FLY_G * n * n


def fly_range_px(angle_deg: float, force01: float) -> float:
    """平地(起落同高)水平射程, px。"""
    th = math.radians(angle_deg)
    v = FLY_V0K * force01
    return v * v * math.sin(2 * th) / FLY_G


def fly_apex_px(angle_deg: float, force01: float) -> float:
    """最高点相对起飞点的高度, px(正值=更高)。"""
    th = math.radians(angle_deg)
    vy = -FLY_V0K * force01 * math.sin(th)
    return vy * vy / (2.0 * FLY_G)


def fly_force_for_range(angle_deg: float, dist_px: float) -> float | None:
    """平地上飞 dist_px 需要多少力度 (0..1); 无解返回 None。"""
    th = math.radians(angle_deg)
    s = math.sin(2 * th)
    if abs(s) < 1e-9 or dist_px <= 0:
        return None
    v2 = dist_px * FLY_G / s
    if v2 <= 0:
        return None
    return math.sqrt(v2) / FLY_V0K


def solve_target(theta_deg: float, dist_ju: float, height_ju: float = 0.0, wind: float = 0.0):
    """给定角度, 解命中目标站立点所需力度(%); 无几何解返回 None。

    相对坐标: 角色为原点, 射击方向=+x; 目标=(dist_ju*PX_PER_JU, height_ju*PX_PER_JU+AIM_UP)。
    AIM_UP=0 -> 落点就是对手脚下(站立点)。
    """
    th = math.radians(theta_deg)
    x0 = MUZZLE * math.cos(th)
    y0 = -MUZZLE * math.sin(th)          # y-down
    tx = dist_ju * PX_PER_JU
    ty = -(height_ju * PX_PER_JU + AIM_UP)
    w = WIND_MULT * wind
    t = math.tan(th)

    def phi(n):
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
    f = (tx - x0 - w * (n - A)) / denom
    return f * 100.0


def wind_action_from_world(wind_world: float, target_right: bool) -> float:
    """游戏风力(世界 +x 为正) -> 顺射向为正的风力。"""
    return wind_world if target_right else -wind_world


def pick(self_screen, tap_screen, angle_deg: float, wind_world: float = 0.0,
         scale: float = SCREEN_SCALE) -> dict:
    """鼠标点选打点(Part 2): 屏幕上的"我脚下"和"目标点" -> 打过去需要的力度。

    屏幕位移 / scale = 世界位移(px); 世界 px / PX_PER_JU = 距。
    返回力度(%)、按住秒数、飞行力度(0..1)、方向、距离与高差。
    """
    dxw = (tap_screen[0] - self_screen[0]) / scale
    dyw = (tap_screen[1] - self_screen[1]) / scale      # 屏幕 y 向下
    right = dxw > 0
    w = wind_action_from_world(wind_world, right)
    dist_ju = abs(dxw) / PX_PER_JU
    height_ju = -dyw / PX_PER_JU                        # 目标更高 = 正
    f = solve_target(angle_deg, dist_ju, height_ju, w)
    straight = math.hypot(dxw, dyw)
    fly = fly_force_for_range(angle_deg, straight)
    return {
        "angle": angle_deg,
        "force": None if f is None else round(f, 1),
        "hold_s": None if f is None else round(fly_hold_for_force(f / 100.0), 2),
        "fly_force": None if fly is None else round(fly * 100.0, 1),
        "fly_hold_s": None if fly is None else round(fly_hold_for_force(fly), 2),
        "facing": "right" if right else "left",
        "dist_ju": round(dist_ju, 2),
        "height_ju": round(height_ju, 2),
        "dist_px": round(abs(dxw), 1),
        "wind_action": round(w, 2),
    }


def world_to_rel(self_xy: tuple[float, float], enemy_xy: tuple[float, float], facing: str) -> tuple[float, float]:
    """世界坐标 -> 相对(距, 高)。facing: 'right'|'left' 射击朝向。"""
    sx, sy = self_xy
    ex, ey = enemy_xy
    wpp = SCALE["world_px_per_unit"]
    dx_units = (ex - sx) if facing == "right" else (sx - ex)
    dy_units = (ey - sy) if SCALE["y_up"] else (sy - ey)
    dist_ju = abs(dx_units) * wpp / SCALE["px_per_ju"]
    height_ju = dy_units * wpp / SCALE["px_per_ju"]
    return dist_ju, height_ju


def recommend(dist_ju: float, height_ju: float, wind: float, min_force: float = 5.0,
              max_force: float = 100.0, angles=None, current: float | None = None):
    """候选角度力度表。angles 默认取 CANDIDATE_ANGLES(竞技模式 25..55); current=当前盘面角度。"""
    cand = [float(a) for a in (angles if angles is not None else CANDIDATE_ANGLES)]
    out = []
    for th in cand:
        f = solve_target(th, dist_ju, height_ju, wind)
        if f is None:
            out.append({"angle": th, "force": None, "ok": False, "reason": "no_geometric_solution"})
            continue
        ok = min_force <= f <= max_force
        out.append({"angle": th, "force": round(f, 1), "ok": ok,
                    "reason": "" if ok else ("over_max" if f > max_force else "under_min")})
    ok_list = [r for r in out if r["ok"]]
    ok_list.sort(key=lambda r: abs(r["force"] - 55.0))  # 中段力度精度优先
    res = {"candidates": out, "best": ok_list[0] if ok_list else None,
           "unreachable": not ok_list}
    if current is not None:
        f = solve_target(float(current), dist_ju, height_ju, wind)
        res["current"] = {"angle": float(current),
                          "force": None if f is None else round(f, 1),
                          "ok": f is not None and min_force <= f <= max_force}
    return res


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dist", type=float, help="目标距离(距)")
    ap.add_argument("--height", type=float, default=0.0, help="目标高差(距, 正=更高)")
    ap.add_argument("--wind", type=float, default=0.0, help="风力(游戏风值, 顺射向为正)")
    ap.add_argument("--self", dest="self_xy", help="自坐标 x,y")
    ap.add_argument("--enemy", dest="enemy_xy", help="敌坐标 x,y")
    ap.add_argument("--facing", choices=["right", "left"], default="right")
    ap.add_argument("--click", nargs=2, metavar=("SELFXY", "TAPXY"),
                    help="点选: '我脚下x,y' '目标x,y' (屏幕px)")
    ap.add_argument("--angle", type=float, default=45.0, help="当前角度(点选用)")
    ap.add_argument("--scale", type=float, default=SCREEN_SCALE, help="屏幕px/世界单位")
    args = ap.parse_args()
    if args.click:
        sx, sy = map(float, args.click[0].split(","))
        tx, ty = map(float, args.click[1].split(","))
        print(json.dumps(pick((sx, sy), (tx, ty), args.angle, args.wind, args.scale),
                         ensure_ascii=False, indent=1))
        return
    if args.self_xy and args.enemy_xy:
        sx, sy = map(float, args.self_xy.split(","))
        ex, ey = map(float, args.enemy_xy.split(","))
        dist_ju, height_ju = world_to_rel((sx, sy), (ex, ey), args.facing)
    elif args.dist is not None:
        dist_ju, height_ju = args.dist, args.height
    else:
        ap.print_help()
        return
    rec = recommend(dist_ju, height_ju, args.wind)
    rec["dist_ju"] = round(dist_ju, 2)
    rec["height_ju"] = round(height_ju, 2)
    print(json.dumps(rec, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
