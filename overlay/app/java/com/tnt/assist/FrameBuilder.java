package com.tnt.assist;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/** 把 tnt_memeat 的一行状态解析成 state, 再组装成悬浮层要的 frame(与 PC 端 assist_server 输出一致)。 */
public final class FrameBuilder {
    private static Set<Integer> prevUids = new HashSet<Integer>();
    private static Set<Integer> stableUids = new HashSet<Integer>();

    /**
     * 每个 uid 上一帧的 (sprite 坐标, role 坐标)。
     *
     * 实测结论(v261016.1, 真机对战):
     *  - 正常状态两者完全一致;
     *  - 飞行期间对端的 BattleRole+0x88 冻结在起飞点(实测偏差 361px),
     *    而 sprite 的 QGraphicsItem 位置由服务器下发的轨迹动画驱动, 是准确的;
     *  - 隐身时对端收不到位置包, role 同样不动, sprite 靠轨迹动画跟随。
     * 所以默认用 sprite; 只有 sprite 本帧没动而 role 动了时才改用 role,
     * 覆盖 onRoleHideChanged 把 sprite 冻结的极端情况。
     */
    private static Map<Integer, int[]> prevSprite = new HashMap<Integer, int[]>();
    private static Map<Integer, int[]> prevRole = new HashMap<Integer, int[]>();
    /** sprite 最后一次变化的时间戳, 用来判断隐身期间画面位置是否已经过期。 */
    private static Map<Integer, Long> lastMoveAt = new HashMap<Integer, Long>();

    /** 视角: false = 以自己为参照; true = 以当前行动的队友为参照(由悬浮层“我”按钮切换)。 */
    public static volatile boolean viewMate = false;

    private FrameBuilder() {}

    public static JSONObject simple(String state, String message, String detail) {
        JSONObject o = new JSONObject();
        try {
            o.put("state", state);
            o.put("message", message);
            o.put("detail", detail);
        } catch (Exception ignored) {}
        return o;
    }

    public static JSONObject parseLine(String line) {
        if (line == null) return null;
        line = line.trim();
        if (line.isEmpty()) return null;
        Map<String, String> kv = new HashMap<String, String>();
        for (String tok : line.split("\\s+")) {
            int i = tok.indexOf('=');
            if (i > 0) kv.put(tok.substring(0, i), tok.substring(i + 1));
        }
        String state = kv.containsKey("state") ? kv.get("state")
                : (kv.containsKey("vptr_ok") ? "battle" : "");
        JSONObject st = new JSONObject();
        try {
            if ("idle".equals(state)) {
                st.put("state", "idle");
                return st;
            }
            if ("no_base".equals(state) || "no_maps".equals(state)) {
                st.put("state", "error");
                st.put("detail", state);
                return st;
            }
            if (!"battle".equals(state)) return null;
            if (!"1".equals(kv.get("vptr_ok"))) {
                st.put("state", "error");
                st.put("detail", "bad_vptr");
                return st;
            }
            st.put("state", "battle");
            st.put("round", (int) num(kv.get("round"), -1));
            int weapon = (int) num(kv.get("weapon"), (int) num(kv.get("angle"), -999));
            st.put("weapon_angle", weapon);
            st.put("actual_angle", (int) num(kv.get("actual"), weapon));
            st.put("terrain_angle", num(kv.get("terrain"), 0));
            st.put("facing", kv.containsKey("facing") ? kv.get("facing") : "");
            st.put("force", num(kv.get("force"), -1));
            double wind10 = num(kv.get("wind"), 0x7FFFFFFF);
            st.put("wind10", (wind10 >= 0x7FFFFFFF || wind10 <= -0x80000000) ? null : wind10);
            String[] self = (kv.containsKey("self") ? kv.get("self") : "0,0").split(",");
            st.put("self_x", self.length > 0 ? (int) num(self[0], 0) : 0);
            st.put("self_y", self.length > 1 ? (int) num(self[1], 0) : 0);
            st.put("self_uid", (int) num(kv.get("self_uid"), 0));
            st.put("self_team", kv.containsKey("self_team") ? (int) hex(kv.get("self_team")) : -1);
            st.put("myturn", (int) num(kv.get("myturn"), 0));
            st.put("act_uid", (int) num(kv.get("act_uid"), 0));
            st.put("act_team", kv.containsKey("act_team") ? (int) hex(kv.get("act_team")) : -1);
            String[] act = (kv.containsKey("act") ? kv.get("act") : "0,0").split(",");
            st.put("act_x", act.length > 0 ? (int) num(act[0], 0) : 0);
            st.put("act_y", act.length > 1 ? (int) num(act[1], 0) : 0);
            JSONArray roles = new JSONArray();
            String raw = kv.containsKey("roles") ? kv.get("roles") : "";
            for (String item : raw.split(",")) {
                if (item.isEmpty()) continue;
                String[] p = item.split(":");
                if (p.length < 3) continue;
                int uid = (int) num(p[0], 0);
                int x = (int) num(p[1], 0);
                int y = (int) num(p[2], 0);
                int team = p.length >= 4 ? (int) hex(p[3]) : 0;
                String name = "";
                if (p.length >= 5 && !"-".equals(p[4])) name = unhex(p[4]);
                boolean dead = p.length >= 6 && num(p[5], 0) != 0;
                boolean hide = p.length >= 7 && num(p[6], 0) != 0;
                boolean titan = p.length >= 8 && num(p[7], 0) != 0;
                // p[8],p[9] = BattleRole+0x88 的原始坐标。飞行/隐身时它会停在
                // 起飞点(实测偏差可达 361px), 而 x,y 取自 sprite 才是画面位置。
                int roleX = p.length >= 10 ? (int) num(p[8], 0) : x;
                int roleY = p.length >= 10 ? (int) num(p[9], 0) : y;
                if (uid <= 0) continue;
                if (x < -20000 || x > 20000 || y < -20000 || y > 20000) continue;
                JSONObject r = new JSONObject();
                r.put("uid", uid);
                r.put("x", x);
                r.put("y", y);
                r.put("team", team);
                r.put("name", name);
                r.put("dead", dead);
                r.put("hide", hide);
                r.put("titan", titan);
                r.put("role_x", roleX);
                r.put("role_y", roleY);
                // 两个来源不一致时, sprite 是画面位置, role 是服务器同步点。
                // 飞行/隐身期间 role 会冻结, 因此以 sprite 为准; 只有 sprite
                // 明显过期(与 role 差很大且自身没在动)时才回退。
                r.put("pos_split", Math.abs(roleX - x) > 3 || Math.abs(roleY - y) > 3);
                roles.put(r);
            }
            st.put("roles", roles);
        } catch (Exception e) {
            return null;
        }
        return st;
    }

    /** 组装悬浮层 frame。 */
    public static JSONObject build(JSONObject st) {
        JSONObject f = new JSONObject();
        try {
            if (st == null) return simple("idle", "等待进入对战...", "waiting");
            String state = st.optString("state", "idle");
            if (!"battle".equals(state)) {
                if ("error".equals(state)) {
                    return simple("error", "读取失败", st.optString("detail", "?"));
                }
                return simple("idle", "等待进入对战...", "not_in_battle");
            }
            f.put("state", "battle");
            int rawActual = st.optInt("actual_angle", -999);
            int weapon = st.optInt("weapon_angle", rawActual);
            double terrain = st.optDouble("terrain_angle", 0);
            // 反抛(>90° 或 <-90°) 折回对侧; -90~90 的负角度是炮口朝下, 原样保留
            boolean reverse = rawActual > 90 || rawActual < -90;
            int actual = rawActual > 90 ? 180 - rawActual
                    : (rawActual < -90 ? -180 - rawActual : rawActual);
            if (actual < -90) actual = -90;
            if (actual > 90) actual = 90;
            String panelFacing = st.optString("facing", "");
            if (!"left".equals(panelFacing) && !"right".equals(panelFacing)) panelFacing = null;
            String shotFacing = panelFacing;
            if (reverse && panelFacing != null) shotFacing = "right".equals(panelFacing) ? "left" : "right";
            Double wind = null;
            if (!st.isNull("wind10")) wind = Math.round(st.optDouble("wind10", 0) / 10.0 * 10) / 10.0;
            double force = st.optDouble("force", -1);

            f.put("round", st.optInt("round", -1));
            f.put("actual_angle", actual);
            f.put("raw_angle", rawActual);
            f.put("reverse", reverse);
            f.put("weapon_angle", weapon);
            f.put("terrain_angle", Math.round(terrain * 100) / 100.0);
            f.put("force_pct", force >= 0 ? Math.round(force * 1000) / 10.0 : JSONObject.NULL);
            f.put("wind", wind == null ? "--" : wind);
            f.put("facing", shotFacing != null ? shotFacing : "right");

            int meX = st.optInt("self_x", 0), meY = st.optInt("self_y", 0);
            int meUid = st.optInt("self_uid", 0);
            JSONArray roles = st.optJSONArray("roles");
            List<JSONObject> list = stabilize(roles);

            int selfTeam = st.has("self_team") ? st.optInt("self_team", -1) : -1;
            boolean teamKnown = selfTeam >= 0;

            // ---- 视角: 默认自己; “我”按钮可切到当前行动的队友 ----
            int vpX = meX, vpY = meY;
            String viewName = "";
            boolean mateView = false;
            int actUid = st.optInt("act_uid", 0);
            int actTeam = st.has("act_team") ? st.optInt("act_team", -1) : -1;
            int actX = st.optInt("act_x", 0), actY = st.optInt("act_y", 0);
            JSONObject actRole = null;
            for (JSONObject r : list) {
                if (actUid != 0 && r.optInt("uid", 0) == actUid) { actRole = r; break; }
            }
            boolean mateOk = actUid != 0 && actUid != meUid && !(actRole != null && actRole.optBoolean("dead", false))
                    && teamKnown && actTeam == selfTeam
                    && Math.abs(actX) < 20000 && Math.abs(actY) < 20000;
            if (viewMate && mateOk) {
                mateView = true;
                vpX = actX;
                vpY = actY;
                viewName = actRole == null ? "" : actRole.optString("name", "");
            }
            f.put("view", mateView ? "mate" : "self");
            f.put("view_name", viewName);
            f.put("mate_ok", mateOk);

            // 放大镜变身: 被判定的"我"是否变大。变大 = 精灵 2× 渲染, 距离与高度按 ÷2 计入弹道。
            JSONObject vpRole = null;
            if (mateView) {
                vpRole = actRole;
            } else {
                for (JSONObject r : list) {
                    if (meUid != 0 && r.optInt("uid", 0) == meUid) { vpRole = r; break; }
                }
            }
            boolean selfTitan = vpRole != null && vpRole.optBoolean("titan", false);
            f.put("self_titan", selfTitan);
            f.put("myturn", st.optInt("myturn", 0) != 0);

            List<JSONObject> enemies = new ArrayList<JSONObject>();
            int teammates = 0;
            for (JSONObject r : list) {
                int uid = r.optInt("uid", 0);
                if (meUid != 0 ? uid == meUid
                        : (r.optInt("x", 0) == meX && r.optInt("y", 0) == meY)) continue;
                if (teamKnown && r.optInt("team", -1) == selfTeam) {
                    teammates++;
                } else {
                    pickFreshPos(r);
                    enemies.add(r);
                }
            }
            f.put("teammates", teammates);
            f.put("enemy_count", enemies.size());

            if (enemies.isEmpty()) {
                f.put("targets", new JSONArray());
                f.put("target_note", "角色刷新中...");
                return f;
            }
            // 按距离排序(以当前视角为参照)
            final int fx = vpX, fy = vpY;
            java.util.Collections.sort(enemies, new java.util.Comparator<JSONObject>() {
                @Override
                public int compare(JSONObject a, JSONObject b) {
                    double da = Math.pow(a.optInt("x", 0) - fx, 2) + Math.pow(a.optInt("y", 0) - fy, 2);
                    double db = Math.pow(b.optInt("x", 0) - fx, 2) + Math.pow(b.optInt("y", 0) - fy, 2);
                    return Double.compare(da, db);
                }
            });

            JSONArray targets = new JSONArray();
            for (JSONObject e : enemies) {
                int ex = e.optInt("x", 0), ey = e.optInt("y", 0);
                String facing = ex >= vpX ? "right" : "left";
                // 变大后相对位置减半: 距离和高度都 ÷2, 同样的力度会飞得更远。
                double titanK = selfTitan ? 0.5 : 1.0;
                double distJu = Math.abs(ex - vpX) / Solver.PX_PER_JU * titanK;
                double heightJu = (vpY - ey) / Solver.PX_PER_JU * titanK;   // 正=目标在我上方
                double wnow = wind == null ? 0.0 : wind;
                double windAction = "right".equals(facing) ? wnow : -wnow;
                String windKind = Math.abs(wnow) < 0.05 ? "无" : (windAction > 0 ? "顺" : "逆");
                boolean behind = reverse && panelFacing != null && !facing.equals(shotFacing);

                double fv = behind ? Double.NaN
                        : Solver.solveTarget(actual, distJu, heightJu, windAction);
                Double forceV = Double.isNaN(fv) ? null : Math.round(fv * 10) / 10.0;
                boolean ok = forceV != null && forceV >= Solver.MIN_FORCE && forceV <= Solver.MAX_FORCE;
                String reason = behind ? "reverse_away"
                        : (forceV == null ? "no_geometric_solution"
                        : (!ok ? (forceV > Solver.MAX_FORCE ? "over_max" : "under_min") : ""));
                String hnote = Math.abs(heightJu) < 0.2 ? "同高"
                        : ((heightJu < 0 ? "我高" : "我低") + String.format("%.1f", Math.abs(heightJu)));

                JSONObject t = new JSONObject();
                t.put("uid", e.optInt("uid", 0));
                t.put("name", e.optString("name", ""));
                t.put("dead", e.optBoolean("dead", false));
                t.put("hide", e.optBoolean("hide", false));
                t.put("titan", e.optBoolean("titan", false));
                JSONArray pos = new JSONArray();
                pos.put(ex);
                pos.put(ey);
                t.put("pos", pos);
                t.put("facing", facing);
                t.put("dist_ju", Math.round(distJu * 100) / 100.0);
                t.put("height_ju", Math.round(heightJu * 100) / 100.0);
                t.put("height_note", hnote);
                t.put("wind_action", Math.round(windAction * 10) / 10.0);
                t.put("wind_kind", windKind);
                t.put("angle", actual);
                t.put("force", forceV == null ? JSONObject.NULL : forceV);
                t.put("ok", ok);
                if (!reason.isEmpty()) t.put("reason", reason);
                targets.put(t);
            }
            f.put("targets", targets);
        } catch (Exception e) {
            return simple("error", "组包失败", String.valueOf(e));
        }
        return f;
    }

    /**
     * 在 sprite 与 BattleRole 两个坐标来源之间挑当前可信的那个。
     *
     * 双端实测 (v261016.1, 马戏团地图) 的结论与早期反编译推断相反:
     *
     *   观察方看对端 (对端飞行后):
     *     sprite=(107,919)  role=(1157,469)   gap=1142px   <- role 冻结在开局点
     *   观察方看对端 (对端隐身中, hide=1):
     *     sprite=(414,59)   role=(414,38)     gap=21px     <- sprite 仍然准确
     *   各自看自己: sprite == role, gap=0
     *
     * 但这只在"可见"时成立。后续真实对局实测 (双端 12 Hz, 6 段隐身)
     * 推翻了上面"隐身时 sprite 仍然准确"那一条: 隐身期间 sprite 会停在飞行
     * 弧线中途不再前进, 与真实位置差 290-507 px; role 差 431-867 px。两个来源
     * 都不可信, 因为服务端在隐身期间压根不发位置包 —— 解除隐身的瞬间 sprite
     * 一帧内精确归位 (误差 0.0) 就是最直接的证据。
     *
     * 所以策略改成: 永远用 sprite (实测 6/6 都比 role 接近真实位置), 不再用
     * role 兜底; 隐身时额外上报 pos_stale / stale_ms, 让界面知道这个点是
     * "最后一次看到的坐标"而不是当前位置。
     */
    private static void pickFreshPos(JSONObject r) {
        int uid = r.optInt("uid", 0);
        if (uid <= 0) return;
        int sx = r.optInt("x", 0), sy = r.optInt("y", 0);
        int rx = r.optInt("role_x", sx), ry = r.optInt("role_y", sy);
        int[] ps = prevSprite.get(uid);
        int[] pr = prevRole.get(uid);
        boolean spriteMoved = ps != null && (ps[0] != sx || ps[1] != sy);
        boolean roleMoved = pr != null && (pr[0] != rx || pr[1] != ry);
        prevSprite.put(uid, new int[]{sx, sy});
        prevRole.put(uid, new int[]{rx, ry});

        boolean hidden = r.optBoolean("hide", false);
        long now = System.currentTimeMillis();
        if (spriteMoved || lastMoveAt.get(uid) == null) lastMoveAt.put(uid, now);
        long ageMs = now - lastMoveAt.get(uid);

        // 实测结论 (真实对局, 双端 12 Hz, 6 段隐身):
        //   隐身中 sprite 冻结在飞行弧线中途, 误差 290-507 px;
        //   role 全程冻结, 误差 431-867 px —— sprite 6/6 都更接近, role 从不更准。
        //   解除隐身的瞬间 sprite 一帧内精确归位 (误差 0.0), 说明服务端在隐身期间
        //   根本不发这个人的位置包, 客户端是真的"不知道", 不是算错了。
        // 2026-10-28 内存取证补充: 隐身+飞行期间在观察方 695 MB 匿名内存里搜真实坐标
        //   0 命中 (±2 px 容差也是 0), 对照: 画面坐标 1 命中; 落点同样 0 命中 ——
        //   客户端确实没有这份数据。隐身者开炮的瞬间真值确实会进内存 (3 个 i32 拷贝,
        //   活约 3 s), 但**画面精灵不一定跟着更新** (实测有一次 7 s 内都没动),
        //   而且这些拷贝没有稳定结构可读, 所以不能据此判定"新鲜"。
        // 结论: 隐身期间一律标记过期; 解除隐身 (hide=0) 后精灵才可信 (实测 0 px)。
        try {
            r.put("pos_src", "sprite");
            if (hidden) {
                r.put("pos_stale", true);
                r.put("stale_ms", (int) Math.min(ageMs, 600000L));
            }
        } catch (Exception ignored) {}
    }

    /** 过滤面板重建瞬间的残留角色: 只保留连续两帧都出现的 uid。 */
    private static List<JSONObject> stabilize(JSONArray roles) {
        List<JSONObject> out = new ArrayList<JSONObject>();
        if (roles == null) return out;
        Set<Integer> cur = new HashSet<Integer>();
        for (int i = 0; i < roles.length(); i++) {
            JSONObject r = roles.optJSONObject(i);
            if (r != null) cur.add(r.optInt("uid", 0));
        }
        Set<Integer> next = new HashSet<Integer>();
        for (Integer u : cur) if (stableUids.contains(u)) next.add(u);
        for (Integer u : prevUids) if (cur.contains(u)) next.add(u);
        stableUids = next;
        prevUids = cur;
        for (int i = 0; i < roles.length(); i++) {
            JSONObject r = roles.optJSONObject(i);
            if (r != null && stableUids.contains(r.optInt("uid", 0))) out.add(r);
        }
        if (out.isEmpty()) {
            for (int i = 0; i < roles.length(); i++) {
                JSONObject r = roles.optJSONObject(i);
                if (r != null) out.add(r);
            }
        }
        return out;
    }

    private static double num(String s, double dflt) {
        if (s == null) return dflt;
        try { return Double.parseDouble(s); } catch (Exception e) { return dflt; }
    }

    private static long hex(String s) {
        if (s == null) return 0;
        try { return Long.parseLong(s, 16); } catch (Exception e) { return 0; }
    }

    private static String unhex(String s) {
        try {
            byte[] b = new byte[s.length() / 2];
            for (int i = 0; i < b.length; i++) {
                b[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
            }
            return new String(b, "UTF-8");
        } catch (Exception e) {
            return "";
        }
    }
}
