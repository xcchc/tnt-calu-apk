package com.tnt.assist;

/** 弹道解算(与 core/aim_solver.py 同一套物理模型与常量)。
 *  v261016.1 复标定(服务器权威弹道 39 点重拟合, 2026-11-02):
 *  每帧速度 x Q, v0 = V0K*force, 风加速度 = WIND_MULT*wind_action。
 *  残差: 出厂常量 6.45 px -> 新常量 2.07 px rms(单发最优 1.01 px)。 */
public final class Solver {
    public static final double K = 0.0186951412;
    public static final double G = 4.28938354;
    public static final double V0K = 144.787175;
    public static final double WIND_MULT = 1.46643562;
    public static final double MUZZLE = 28.0;
    /** 瞄准点 = 对手站立点(脚下), 不再抬高。 */
    public static final double AIM_UP = 0.0;
    /** 命中判定中心 = 脚底上 20px(仅记录, 不参与解算)。 */
    public static final double TY_UP = 20.0;
    public static final double PX_PER_JU = 82.5;
    public static final double Q = 1.0 - K;
    /** 屏幕px / 世界单位(竞技模式实测 0.648; 可被运行时标定覆盖)。 */
    public static final double SCREEN_SCALE = 1.98;
    /** 游戏里力度可设范围(%) */
    public static final double MIN_FORCE = 5.0;
    public static final double MAX_FORCE = 100.0;

    private Solver() {}

    /** 备选解: angle 为局部角度(目标恒在右), force 为力度%。 */
    public static final class Sol {
        public final double angle;
        public final double force;
        Sol(double angle, double force) {
            this.angle = angle;
            this.force = force;
        }
    }

    private static double phi(double n, double y0, double t, double tx, double x0, double w, double ty) {
        double A = (1.0 - Math.pow(Q, n)) / K;
        double B = n - A;
        return y0 - t * (tx - x0 - w * B) + (G / K) * B - ty;
    }

    /** 给定角度求命中所需力度(%), 无几何解返回 NaN。theta 可负(枪口朝下)。wind 正=顺射向。 */
    public static double solveTarget(double thetaDeg, double distJu, double heightJu, double wind) {
        double th = Math.toRadians(thetaDeg);
        double x0 = MUZZLE * Math.cos(th);
        double y0 = -MUZZLE * Math.sin(th);
        double tx = distJu * PX_PER_JU;
        double ty = -(heightJu * PX_PER_JU + AIM_UP);
        double w = WIND_MULT * wind;
        double t = Math.tan(th);
        double lo = 0.05, hi = 3000.0;
        double flo = phi(lo, y0, t, tx, x0, w, ty);
        double fhi = phi(hi, y0, t, tx, x0, w, ty);
        if (Double.isNaN(flo) || Double.isNaN(fhi) || flo * fhi > 0) return Double.NaN;
        for (int i = 0; i < 80; i++) {
            double mid = (lo + hi) / 2.0;
            double fm = phi(mid, y0, t, tx, x0, w, ty);
            if (flo * fm <= 0) {
                hi = mid;
                fhi = fm;
            } else {
                lo = mid;
                flo = fm;
            }
        }
        double n = (lo + hi) / 2.0;
        double A = (1.0 - Math.pow(Q, n)) / K;
        double denom = V0K * Math.cos(th) * A;
        if (Math.abs(denom) < 1e-9) return Double.NaN;
        return (tx - x0 - w * (n - A)) / denom * 100.0;
    }

    /**
     * 当前角度打不到时, 在 -88~88° 里找最接近的可打角度(力度必须落在 5~100%)。
     * 返回 null 表示这个距离/高度怎么调角度都打不到。
     */
    public static Sol scanBest(double thetaLocal, double distJu, double heightJu, double wind) {
        double bestA = Double.NaN, bestF = 0.0, bestD = 1e9;
        for (double a = -88.0; a <= 88.0 + 1e-9; a += 1.0) {
            if (Math.abs(a - thetaLocal) < 0.5) continue;   // 当前角度已经试过
            double f = solveTarget(a, distJu, heightJu, wind);
            if (Double.isNaN(f) || f < MIN_FORCE || f > MAX_FORCE) continue;
            double d = Math.abs(a - thetaLocal);
            if (d < bestD) { bestD = d; bestA = a; bestF = f; }
        }
        if (Double.isNaN(bestA)) return null;
        return new Sol(bestA, bestF);
    }

    // ================= 鼠标点选打点 (Part 2) =================
    //
    // 用法: 先在屏幕上点自己脚下(selfX,selfY), 再点目标点(tapX,tapY)。
    //   世界位移 = 屏幕位移 / scale      (竞技模式实测 scale = 0.648)
    //   水平距离(距) = |dx| / PX_PER_JU ; 高差(距) = -dy / PX_PER_JU  (正 = 目标更高)
    //   力度 = solveTarget(当前角度, 距离, 高差, 顺射向风)
    //   飞行力度 = flyForceForRange(当前角度, 直线距离)
    public static final class Pick {
        public final double force;      // 打到该点的力度 %(NaN = 无解)
        public final double flyForce;   // 飞到该点的力度 0..1(NaN = 无解)
        public final double holdS;      // 打到该点需要按住多少秒
        public final double distJu;     // 水平距离(距)
        public final double heightJu;   // 高差(距, 正 = 目标更高)
        public final double distPx;     // 水平距离(世界 px)
        public final boolean right;     // 目标在右侧?

        Pick(double force, double flyForce, double holdS, double distJu, double heightJu,
             double distPx, boolean right) {
            this.force = force;
            this.flyForce = flyForce;
            this.holdS = holdS;
            this.distJu = distJu;
            this.heightJu = heightJu;
            this.distPx = distPx;
            this.right = right;
        }
    }

    /** 点选解算。scale = 屏幕px / 世界单位(默认 SCREEN_SCALE)。 */
    public static Pick pick(double selfX, double selfY, double tapX, double tapY,
                            double angleDeg, double wind, double scale) {
        double s = (scale > 0.05 && scale < 20.0) ? scale : SCREEN_SCALE;
        double dxw = (tapX - selfX) / s;
        double dyw = (tapY - selfY) / s;                 // 屏幕 y 向下
        double distJu = Math.abs(dxw) / PX_PER_JU;
        double heightJu = -dyw / PX_PER_JU;              // 目标更高 = 正
        double f = solveTarget(angleDeg, distJu, heightJu, wind);
        double straight = Math.sqrt(dxw * dxw + dyw * dyw);
        double fly = flyForceForRange(angleDeg, straight);
        double hold = Double.isNaN(f) ? Double.NaN : flyHoldForForce(f / 100.0);
        return new Pick(f, fly, hold, distJu, heightJu, Math.abs(dxw), dxw > 0);
    }

    /** 点选解算: 风取"顺射向为正"(目标在左则风反向)。 */
    public static Pick pick(double selfX, double selfY, double tapX, double tapY,
                            double angleDeg, double windWorld, double scale, boolean flipWind) {
        double w = flipWind ? -windWorld : windWorld;
        return pick(selfX, selfY, tapX, tapY, angleDeg, w, scale);
    }

    // ================= 喷气背包(飞行)弹道 =================
    //
    // 真机双账号 (v261016.1, 马戏团地图) 实测 8 条完整轨迹拟合得出。
    // 速度方向吻合残差 0.0~1.0 px/帧, 速度标定残差 rms 0.65 px/帧。
    //
    // 与"发射子弹"完全是另一套公式, 四点不同:
    //   1. 没有空气阻力   —— 子弹是 K=0.0186951 的指数阻尼, 飞行是纯抛物线 (K=0)
    //   2. 不受风力影响   —— 实测风 +1156 与 -1374 的样本残差无相关 (r=+0.22)
    //   3. 初速更小       —— FLY_V0K=105.64, 子弹 V0K=144.787 (0.73 倍)
    //   4. 重力更小       —— FLY_G=2.28, 子弹 G=4.28938 (0.532 倍)
    //
    // 发射角就是拨盘上的 actual 角(已经等于 weapon + terrain), 不需要再补偿。
    //   v   = FLY_V0K * force            force 取 0..1
    //   vx  = v * cos(theta)
    //   vy  = -v * sin(theta)            屏幕 y 向下为正
    //   x(n) = x0 + vx * n
    //   y(n) = y0 + vy * n + 0.5 * FLY_G * n * n      n = 帧 (30fps)
    public static final double FLY_V0K = 105.64;
    public static final double FLY_G = 2.28;
    public static final double FLY_FPS = 30.0;

    // 力度充电(按住发射键): 实测 force = 0.1940 * 按住秒数 - 0.0294
    public static final double CHARGE_K = 0.1990;
    public static final double CHARGE_B = -0.0306;

    /** 按住多少秒能得到这个力度(0..1)。 */
    public static double flyHoldForForce(double force01) {
        double f = Math.max(0.0, Math.min(1.0, force01));
        return Math.max(0.0, (f - CHARGE_B) / CHARGE_K);
    }

    /** 按住 holdS 秒会得到多大力度(0..1)。 */
    public static double flyForceFromHold(double holdS) {
        return Math.max(0.0, Math.min(1.0, CHARGE_K * holdS + CHARGE_B));
    }

    /** 满力需要的按住时长(秒)。 */
    public static double flyFullPowerHold() {
        return (1.0 - CHARGE_B) / CHARGE_K;
    }

    /** 第 n 帧的位置偏移 {dx, dy}。 */
    public static double[] flyStep(double angleDeg, double force01, double n) {
        double th = Math.toRadians(angleDeg);
        double v = FLY_V0K * force01;
        double vx = v * Math.cos(th);
        double vy = -v * Math.sin(th);
        return new double[]{vx * n,
                            vy * n + 0.5 * FLY_G * n * n};
    }

    /** 平地上(起落同高)的飞行水平射程, 单位 px。 */
    public static double flyRangePx(double angleDeg, double force01) {
        double th = Math.toRadians(angleDeg);
        double v = FLY_V0K * force01;
        return v * v * Math.sin(2 * th) / FLY_G;
    }

    /** 最高点相对起飞点的高度, 单位 px(正值=更高)。 */
    public static double flyApexPx(double angleDeg, double force01) {
        double th = Math.toRadians(angleDeg);
        double vy = -FLY_V0K * force01 * Math.sin(th);
        return vy * vy / (2.0 * FLY_G);
    }

    /** 平地上要飞 distPx 需要多少力度(0..1), 无解返回 NaN。 */
    public static double flyForceForRange(double angleDeg, double distPx) {
        double th = Math.toRadians(angleDeg);
        double s = Math.sin(2 * th);
        if (Math.abs(s) < 1e-9 || distPx <= 0) return Double.NaN;
        double v2 = distPx * FLY_G / s;
        if (v2 <= 0) return Double.NaN;
        return Math.sqrt(v2) / FLY_V0K;
    }
}
