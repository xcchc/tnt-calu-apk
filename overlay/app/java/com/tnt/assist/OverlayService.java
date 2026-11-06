package com.tnt.assist;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.IBinder;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowManager;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.InetSocketAddress;
import java.net.Socket;

/** 前台服务: 持有悬浮层, 从 PC 端 assist_server (adb reverse tcp:8765) 收 JSON 更新显示。 */
public class OverlayService extends Service {
    static volatile boolean running = false;
    private AssistView view;
    private WindowManager wm;
    private WindowManager.LayoutParams lp;
    private SharedPreferences prefs;
    private volatile boolean alive;
    private Thread netThread;
    private LocalSource local;
    private int dragStartX, dragStartY;
    private float touchStartX, touchStartY;
    private boolean controlTouch;
    private TapPickView pickView;

    @Override
    public void onCreate() {
        super.onCreate();
        running = true;
        alive = true;

        if (Build.VERSION.SDK_INT >= 26) {
            NotificationChannel ch = new NotificationChannel("assist", "TNT辅助",
                    NotificationManager.IMPORTANCE_LOW);
            getSystemService(NotificationManager.class).createNotificationChannel(ch);
            Notification n = new Notification.Builder(this, "assist")
                    .setContentTitle("TNT 辅助运行中")
                    .setSmallIcon(android.R.drawable.ic_menu_compass)
                    .build();
            startForeground(1, n);
        }

        wm = (WindowManager) getSystemService(WINDOW_SERVICE);
        view = new AssistView(this);
        prefs = getSharedPreferences("assist_overlay", MODE_PRIVATE);
        lp = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                Build.VERSION.SDK_INT >= 26
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.START;
        lp.x = prefs.getInt("x", 12);
        lp.y = prefs.getInt("y", 320);
        view.setResizeListener(new AssistView.ResizeListener() {
            @Override
            public void onResize(float scale, int newX, int newY) {
                // 角落拖动: 面板等比缩放, 左上角跟手移动
                lp.x = newX;
                lp.y = newY;
                try { wm.updateViewLayout(view, lp); } catch (Exception ignored) {}
            }
        });
        view.setScaleListener(new AssistView.ScaleListener() {
            @Override
            public void onScaleChanged(float scale) {
                // 面板尺寸变了, 让系统重新按 wrap_content 量一次窗口
                view.post(new Runnable() {
                    @Override
                    public void run() {
                        try { wm.updateViewLayout(view, lp); } catch (Exception ignored) {}
                    }
                });
            }
        });
        view.setOnTouchListener(new View.OnTouchListener() {
            @Override
            public boolean onTouch(View v, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        // 底部控制条(小/大/重置/手柄)交给 View 自己处理
                        if (v instanceof AssistView
                                && ((AssistView) v).isInControlArea(e.getX(), e.getY())) {
                            controlTouch = true;
                            return false;
                        }
                        controlTouch = false;
                        touchStartX = e.getRawX();
                        touchStartY = e.getRawY();
                        dragStartX = lp.x;
                        dragStartY = lp.y;
                        return true;
                    case MotionEvent.ACTION_MOVE:
                        if (controlTouch) return false;
                        lp.x = dragStartX + (int) (e.getRawX() - touchStartX);
                        lp.y = dragStartY + (int) (e.getRawY() - touchStartY);
                        if (lp.x < 0) lp.x = 0;
                        if (lp.y < 0) lp.y = 0;
                        wm.updateViewLayout(view, lp);
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (controlTouch) {
                            controlTouch = false;
                            return false;
                        }
                        prefs.edit().putInt("x", lp.x).putInt("y", lp.y).apply();
                        return true;
                }
                return false;
            }
        });
        wm.addView(view, lp);

        view.setPickListener(new AssistView.PickListener() {
            @Override
            public void onTogglePick() {
                togglePick();
            }
        });

        // 本地模式优先: App 自己读内存 + 解算, 不需要 PC
        local = new LocalSource(this, view);
        new Thread(local, "assist-local").start();

        netThread = new Thread(new Runnable() {
            @Override
            public void run() { netLoop(); }
        }, "assist-net");
        netThread.start();
    }

    private void netLoop() {
        int fails = 0;
        while (alive) {
            // 本地模式正常时完全不碰 PC 通道
            if (local != null && System.currentTimeMillis() - local.lastFrameAt < 3000) {
                fails = 0;
                try { Thread.sleep(3000); } catch (InterruptedException ie) { return; }
                continue;
            }
            try (Socket sock = new Socket()) {
                sock.connect(new InetSocketAddress("127.0.0.1", 8765), 1500);
                fails = 0;
                BufferedReader rd = new BufferedReader(
                        new InputStreamReader(sock.getInputStream(), "UTF-8"));
                String line;
                while (alive && (line = rd.readLine()) != null) {
                    final String json = line;
                    view.post(new Runnable() {
                        @Override
                        public void run() { view.updateJson(json); }
                    });
                }
            } catch (Exception e) {
                fails++;
                if (fails == 3 && (local == null
                        || System.currentTimeMillis() - local.lastFrameAt > 5000)) {
                    view.post(new Runnable() {
                        @Override
                        public void run() {
                            view.updateJson("{\"state\":\"error\",\"message\":\"读取失败\","
                                    + "\"detail\":\"本地模式未启动, PC 也没连上\"}");
                        }
                    });
                }
                try { Thread.sleep(1500); } catch (InterruptedException ie) { return; }
            }
        }
    }

    @Override
    public void onDestroy() {
        alive = false;
        running = false;
        if (local != null) local.stop();
        try {
            WindowManager wm = (WindowManager) getSystemService(WINDOW_SERVICE);
            if (view != null) wm.removeView(view);
        } catch (Exception ignored) {}
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent) { return null; }

    // ================= 点选打点(Part 2) =================

    /** 打开/关闭全屏点选层。 */
    private void togglePick() {
        if (pickView != null) {
            closePick();
            return;
        }
        pickView = new TapPickView(this, new TapPickView.Listener() {
            @Override
            public String[] onPick(float ax, float ay, float tx, float ty) {
                return computePick(ax, ay, tx, ty);
            }

            @Override
            public void onSelfMoved(float ax, float ay) {
                prefs.edit().putFloat("selfx", ax).putFloat("selfy", ay).apply();
            }

            @Override
            public void onExit() {
                closePick();
            }
        });
        pickView.setSelf(prefs.getFloat("selfx", 960f), prefs.getFloat("selfy", 700f));
        WindowManager.LayoutParams pl = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                Build.VERSION.SDK_INT >= 26
                        ? WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                        : WindowManager.LayoutParams.TYPE_PHONE,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        pl.gravity = Gravity.TOP | Gravity.START;
        try {
            wm.addView(pickView, pl);
        } catch (Exception e) {
            pickView = null;
        }
    }

    private void closePick() {
        if (pickView != null) {
            try {
                wm.removeView(pickView);
            } catch (Exception ignored) {
            }
            pickView = null;
        }
    }

    /** 屏幕两点 -> 力度/角度/飞行力度。角度与风取面板当前状态。 */
    private String[] computePick(float sx, float sy, float tx, float ty) {
        double ang = 45.0;
        double wind = 0.0;
        JSONObject f = view.lastFrameObj();
        if (f != null) {
            ang = f.optDouble("actual_angle", 45.0);
            wind = f.optDouble("wind", 0.0);
        }
        Solver.Pick p = Solver.pick(sx, sy, tx, ty, ang, wind, Solver.SCREEN_SCALE);
        String dir = p.right ? "右" : "左";
        String l1 = String.format("距离 %.1f 距  高差 %+.1f 距", p.distJu, p.heightJu);
        String l2 = Double.isNaN(p.force)
                ? String.format("角度 %.0f° 打不到(需调角度)", ang)
                : String.format("角度 %.0f°  力度 %.1f%%  按住 %.2fs", ang, p.force, p.holdS);
        String l3 = Double.isNaN(p.flyForce)
                ? "飞过去: 该角度无解"
                : String.format("飞过去: 力度 %.0f%% (按住 %.2fs)", p.flyForce * 100.0,
                        Solver.flyHoldForForce(p.flyForce));
        String l4 = "方向 " + dir + "   屏幕比例 " + Solver.SCREEN_SCALE;
        return new String[]{l1, l2, l3, l4};
    }
}
