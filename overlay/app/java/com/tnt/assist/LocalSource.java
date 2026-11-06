package com.tnt.assist;

import android.content.Context;
import android.util.Log;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.net.SocketTimeoutException;

/** 本地模式: 不依赖 PC。通过本机 adbd(127.0.0.1:5555) 把 adbd 提权成 root,
 *  跑设备上的 tnt_memeat 读游戏内存, 在 App 内解算并直接刷新悬浮层。 */
public class LocalSource implements Runnable {
    private static final String TAG = "TNTASSIST";
    private static final String BIN = "/data/local/tmp/tnt_memeat";
    private static final String PKG = "com.grapefruit.game.tnt";

    private final Context ctx;
    private final AssistView view;
    private volatile boolean alive = true;
    public volatile long lastFrameAt = 0;
    public volatile String lastError = "";
    private int frameNo = 0;

    public LocalSource(Context ctx, AssistView view) {
        this.ctx = ctx;
        this.view = view;
    }

    public void stop() {
        alive = false;
    }

    @Override
    public void run() {
        while (alive) {
            AdbClient adb = new AdbClient();
            try {
                adb.connect(5555, 2500);
                if (!adb.ensureRoot()) {
                    report("本地模式需要 root", "adbd 提权失败(root: 被拒绝)");
                    sleep(4000);
                    continue;
                }
                installBinary(adb);
                while (alive) {
                    String pid = findPid(adb);
                    if (pid.isEmpty()) {
                        post(FrameBuilder.simple("idle", "等待进入对战...", "not_in_battle"));
                        sleep(1500);
                        continue;
                    }
                    final AdbClient conn = adb;
                    final long streamStart = System.currentTimeMillis();
                    try {
                        adb.stream("shell:" + BIN + " watch " + pid + " 2 30",
                                new AdbClient.LineSink() {
                                    @Override
                                    public void onLine(String line) {
                                        // 每 20s 主动回收一次: 游戏重启后 pid 会变, 必须重新解析
                                        if (System.currentTimeMillis() - streamStart > 20000) {
                                            conn.close();
                                            return;
                                        }
                                        JSONObject st = FrameBuilder.parseLine(line);
                                        if (st == null) return;
                                        post(FrameBuilder.build(st));
                                    }
                                }, 6000);
                    } catch (SocketTimeoutException te) {
                        Log.w(TAG, "watch idle timeout, reconnect");
                        adb.close();          // 断开后设备端的 watch 会被 adbd 收掉
                        adb.connect(5555, 2500);
                    } catch (java.io.IOException io) {
                        // 主动回收连接(或对端断开): 重连后继续, 不算错误
                        adb.close();
                        adb.connect(5555, 2500);
                    }
                    sleep(400);
                }
            } catch (Exception e) {
                Log.e(TAG, "local error: " + e);
                report("本地读取异常", String.valueOf(e.getMessage()));
                sleep(2500);
            } finally {
                adb.close();
            }
        }
    }

    private void post(JSONObject frame) {
        lastFrameAt = System.currentTimeMillis();
        final String json = frame.toString();
        if (frameNo++ % 50 == 0) Log.d(TAG, "frame " + json);
        view.post(new Runnable() {
            @Override
            public void run() {
                view.updateJson(json);
            }
        });
    }

    private void report(String message, String detail) {
        lastError = detail;
        post(FrameBuilder.simple("error", message, detail));
    }

    private void installBinary(AdbClient adb) throws Exception {
        File f = new File(ctx.getFilesDir(), "tnt_memeat");
        if (!f.exists() || f.length() == 0) {
            InputStream is = ctx.getAssets().open("tnt_memeat");
            FileOutputStream fos = new FileOutputStream(f);
            byte[] buf = new byte[65536];
            int n;
            while ((n = is.read(buf)) > 0) fos.write(buf, 0, n);
            fos.close();
            is.close();
        }
        String remote = adb.exec("shell:stat -c %s " + BIN + " 2>/dev/null", 4000);
        if (remote == null || !remote.trim().equals(String.valueOf(f.length()))) {
            adb.exec("shell:cp " + f.getAbsolutePath() + " " + BIN + " && chmod 755 " + BIN, 20000);
            Log.i(TAG, "installed " + BIN);
        }
    }

    private String findPid(AdbClient adb) throws Exception {
        String out = adb.exec("shell:pidof " + PKG, 5000);
        if (out == null) return "";
        String[] parts = out.trim().split("\\s+");
        for (String p : parts) {
            if (p.isEmpty()) continue;
            String n = adb.exec("shell:grep -c libgame /proc/" + p + "/maps 2>/dev/null", 4000);
            if (n != null && n.trim().matches("\\d+") && Integer.parseInt(n.trim()) > 0) return p;
        }
        return parts.length > 0 ? parts[0].trim() : "";
    }

    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }
}
