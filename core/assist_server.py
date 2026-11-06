# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""TNT 辅助主循环(Phase 4/7): 采集 -> 解算 -> 推送 overlay。

数据流:
  device_reader.stream()  (设备端 watch 常驻, ~17Hz)
    -> aim_solver 当前角度所需力度
    -> JSON 行 -> TCP :28080
    -> adb reverse tcp:8765 tcp:28080 -> overlay APK (模拟器内悬浮层)

用法:
  uv run core/assist_server.py --push            # 主循环(含 overlay 推送)
  uv run core/assist_server.py --once            # 单帧输出(调试)
前置: adb reverse tcp:8765 tcp:28080
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from core.aim_solver import PX_PER_JU, solve_target, world_to_rel  # noqa: E402
from core.device_reader import DeviceError, ensure_reverse, read_once, stream  # noqa: E402


_prev_uids: set = set()
_stable_uids: set = set()


def _stabilize(roles: list) -> list:
    """过滤面板重建瞬间的残留/垃圾角色节点: 只保留连续两帧都出现的 uid。"""
    global _prev_uids, _stable_uids
    cur = {r["uid"] for r in roles}
    _stable_uids = (_stable_uids & cur) | (cur & _prev_uids)
    _prev_uids = cur
    out = [r for r in roles if r["uid"] in _stable_uids]
    return out or roles


def build_frame(st: dict | None) -> dict:
    if not st or st.get("state") != "battle":
        detail = (st or {}).get("detail", "waiting")
        return {"ts": round(time.time(), 3),
                "state": (st or {}).get("state", "idle"),
                "detail": detail,
                "message": (st or {}).get("message", "等待进入对战...")}

    frame: dict = {"ts": round(time.time(), 3), "state": "battle"}
    me = (st["self"]["x"], st["self"]["y"])
    raw_actual = int(st.get("actual_angle", st.get("angle", -999)))
    weapon = int(st.get("weapon_angle", raw_actual))
    terrain = float(st.get("terrain_angle", 0.0))
    # 反抛: 盘面 117°朝右 = 物理上的 63°朝左。盘面角度超过 90° 时,
    # 水平射向翻转, 用 180-angle 作为求解角度。
    reverse = raw_actual > 90 or raw_actual < 0
    if raw_actual > 90:
        actual = 180 - raw_actual
    elif raw_actual < 0:
        actual = -raw_actual
    else:
        actual = raw_actual
    panel_facing = st.get("facing")
    if panel_facing not in ("left", "right"):
        panel_facing = None
    shot_facing = panel_facing
    if reverse and panel_facing:
        shot_facing = "left" if panel_facing == "right" else "right"
    wind10 = st.get("wind10")
    wind = None if wind10 is None else round(wind10 / 10.0, 1)
    frame.update({
        "round": st["round"],
        "can_fire": st["can_fire"],
        # 左下圆盘中心 = 武器角度 + 地形旋转 (BattleConsole::updateAngle 同款公式)
        "actual_angle": actual,
        "raw_angle": raw_actual,
        "reverse": reverse,
        "weapon_angle": weapon,
        "terrain_angle": round(terrain, 2),
        "force_pct": round(st["force"] * 100, 1) if st["force"] >= 0 else None,
        "self": st["self"],
        "roles": st["roles"],
        # 风: BattleWind+0xC8 的当前数字文本 + +0xE0 箭头朝向(负风)
        # 读不到风时不要输出 null(悬浮层会显示 "null"); 用 "--" 明确未知。
        "wind": "--" if wind is None else wind,
        "wind_text": st.get("wind_text"),
        "scale_note": f"1距={PX_PER_JU:g}px(待校准)",
    })
    me_uid = int(st.get("self_uid") or 0)
    roles = _stabilize(st.get("roles") or [])
    # 武器角度上下限: 运行时从内存读(角度对象 +0xA4/+0xA8), 不写死
    amin = int(st.get("ang_min") or 0)
    amax = int(st.get("ang_max") or 0)
    if not (0 < amin < amax <= 90):
        amin, amax = 45, 78
    # 盘面可调的是“实际角度”: 武器限位 + 当前地形角(限制在 1..89, 避免极端地形越界)
    amin_actual = max(1, min(89, amin + int(round(terrain))))
    amax_actual = max(amin_actual, min(89, amax + int(round(terrain))))
    frame["ang_min"], frame["ang_max"] = amin_actual, amax_actual
    frame["ang_note"] = f"武器{amin}-{amax}° 实际{amin_actual}-{amax_actual}°"
    self_team = int(st.get("self_team") or 0)
    team_values = {r.get("team") for r in roles}
    # 0 既可能是真实队伍(4v4 实测), 也可能是尚未分配; 只有出现非 0 队伍或
    # self_team 非 0 时才认为队伍信息有效。
    team_known = st.get("self_team") is not None and (
        self_team != 0 or any(t not in (0, None) for t in team_values))
    if me_uid:
        if team_known:
            enemies = [r for r in roles
                       if r["uid"] != me_uid and r.get("team") != self_team]
            teammates = [r for r in roles
                         if r["uid"] != me_uid and r.get("team") == self_team]
        else:
            enemies = [r for r in roles if r["uid"] != me_uid]
            teammates = []
    else:
        # 兜底: 内存里没读到 uid 时按坐标排除自己
        enemies = [r for r in roles if (r["x"], r["y"]) != (me[0], me[1])]
        teammates = []
    frame["teammates"] = len(teammates)
    frame["enemy_count"] = len(enemies)
    if not enemies:
        frame["target_note"] = "角色刷新中..."
        frame["target"] = None
        frame["targets"] = []
    else:
        def solve_one(enemy: dict) -> dict:
            facing = "right" if enemy["x"] >= me[0] else "left"
            dist_ju, height_ju = world_to_rel(me, (enemy["x"], enemy["y"]), facing)
            # 游戏风值是“世界向右为正”的风向。顺风/逆风取决于射击方向：
            # 敌人在右时 +风=顺风；敌人在左时 +风=逆风。求解器约定
            # wind_action 正=顺射向，所以按 facing 翻转符号。
            wind_action = (wind or 0.0) if facing == "right" else -(wind or 0.0)
            wnow = wind or 0.0
            # solver 的 wind_action 已经是“相对射向”的风: 正=顺风。
            wind_kind = ("无" if abs(wnow) < 0.05
                         else ("顺" if wind_action > 0 else "逆"))
            # 反抛时物理射向是盘面朝向的反向; 敌人若在反方向, 这一发
            # 不可能打中(不是力度问题), 明确标成 reverse_away。
            behind = bool(reverse and panel_facing and facing != shot_facing)
            f = None if behind else solve_target(float(actual), dist_ju,
                                                 height_ju, wind_action)
            force = None if f is None else round(f, 1)
            ok = force is not None and 5.0 <= force <= 100.0
            current = {"angle": float(actual), "force": force, "ok": ok}
            cur = dict(current)
            if behind:
                cur["reason"] = "reverse_away"
            elif force is None:
                cur["reason"] = "no_geometric_solution"
            elif not ok:
                cur["reason"] = "over_max" if force > 100.0 else "under_min"
            rec = {"angle": float(actual), "force": force, "ok": ok,
                   "current": cur, "candidates": [],
                   "best": cur if ok else None, "unreachable": not ok}
            if behind:
                rec["reason"] = "reverse_away"
            elif force is None:
                rec["reason"] = "no_geometric_solution"
            elif not ok:
                rec["reason"] = "over_max" if force > 100.0 else "under_min"
            # height_ju: 负数=目标在我下方(我更高)
            if abs(height_ju) < 0.2:
                hnote = "同高"
            else:
                hnote = ("我高" if height_ju < 0 else "我低") + f"{abs(height_ju):.1f}"
            return {"uid": enemy["uid"], "name": enemy.get("name", ""),
                    "pos": [enemy["x"], enemy["y"]],
                    "facing": facing, "dist_ju": round(dist_ju, 2),
                    "height_ju": round(height_ju, 2),
                    "height_note": hnote,
                    "wind_action": round(wind_action, 1),
                    "wind_kind": wind_kind, "solve": rec}

        enemies.sort(key=lambda r: (r["x"] - me[0]) ** 2 + (r["y"] - me[1]) ** 2)
        targets = [solve_one(e) for e in enemies]
        frame["facing"] = shot_facing or targets[0]["facing"]
        frame["target"] = {k: targets[0][k] for k in
                           ("uid", "name", "pos", "facing", "dist_ju",
                            "height_ju", "height_note")}
        frame["solve"] = targets[0]["solve"]
        frame["targets"] = targets
        frame["enemy_count"] = len(targets)
    return frame


class OverlayPusher:
    def __init__(self, port: int = 28080) -> None:
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(1)
        self.srv.settimeout(0.05)
        self.conn: socket.socket | None = None
        self.frames = 0
        print(f"[pusher] listening 127.0.0.1:{port} "
              f"(device connects to 8765 via adb reverse)", flush=True)

    def push(self, frame: dict) -> None:
        if self.conn is None:
            try:
                c, addr = self.srv.accept()
                c.settimeout(0.2)
                self.conn = c
                print(f"[pusher] overlay connected {addr}", flush=True)
            except (socket.timeout, OSError):
                pass
        if self.conn is not None:
            try:
                self.conn.sendall((json.dumps(frame, ensure_ascii=False) + "\n").encode())
                self.frames += 1
                if self.frames % 200 == 0:
                    print(f"[pusher] {self.frames} frames state={frame.get('state')}",
                          flush=True)
            except (BrokenPipeError, ConnectionResetError, OSError):
                print("[pusher] overlay disconnected", flush=True)
                try:
                    self.conn.close()
                finally:
                    self.conn = None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--push", action="store_true")
    ap.add_argument("--port", type=int, default=28080)
    ap.add_argument("--hz-idle", type=float, default=2.0)
    ap.add_argument("--hz-battle", type=float, default=30.0)
    args = ap.parse_args()

    if args.once:
        print(json.dumps(build_frame(read_once()), ensure_ascii=False, indent=1))
        return

    pusher = OverlayPusher(args.port) if args.push else None
    last_reverse = 0.0
    try:
        for st in stream(args.hz_idle, args.hz_battle):
            now = time.time()
            if args.push and now - last_reverse > 10:
                try:
                    ensure_reverse()
                    last_reverse = now
                except DeviceError as exc:
                    print(f"[reverse] {exc}", flush=True)
            frame = build_frame(st)
            if pusher:
                pusher.push(frame)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
