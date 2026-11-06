package com.tnt.assist;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/** 控制面板: 开关悬浮辅助层。 */
public class MainActivity extends Activity {
    private static final int REQ_OVERLAY = 1001;
    private TextView status;

    @Override
    protected void onCreate(Bundle s) {
        super.onCreate(s);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(48, 48, 48, 48);
        root.setBackgroundColor(0xFF1B2430);

        TextView title = new TextView(this);
        title.setText("TNT 辅助 (内存读取 + 瞄准解算)");
        title.setTextSize(22);
        title.setTextColor(0xFFFFFFFF);
        root.addView(title);

        status = new TextView(this);
        status.setText(OverlayService.running ? "状态: 运行中" : "状态: 未启动");
        status.setTextSize(16);
        status.setTextColor(0xFF88DDFF);
        root.addView(status);

        Button perm = new Button(this);
        perm.setText("1. 授权悬浮窗权限");
        perm.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent i = new Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivityForResult(i, REQ_OVERLAY);
            }
        });
        root.addView(perm);

        Button start = new Button(this);
        start.setText("2. 开启辅助悬浮层");
        start.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Intent i = new Intent(MainActivity.this, OverlayService.class);
                if (Build.VERSION.SDK_INT >= 26) startForegroundService(i);
                else startService(i);
                status.setText("状态: 运行中");
            }
        });
        root.addView(start);

        Button stop = new Button(this);
        stop.setText("关闭辅助并退出");
        stop.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                stopService(new Intent(MainActivity.this, OverlayService.class));
                finish();
            }
        });
        root.addView(stop);

        TextView hint = new TextView(this);
        hint.setText("\n本地模式: App 自己连本机 adbd(127.0.0.1:5555) 提权读取内存,\n"
                + "不需要 PC。模拟器需开启 root(本机已开启)。\n"
                + "游戏内进入对战后自动显示角度/力度/敌人列表。\n"
                + "PC 模式仍保留: 连不上本机 adbd 时自动回退到 8765 端口。");
        hint.setTextSize(13);
        hint.setTextColor(0xFF9AB0C0);
        root.addView(hint);

        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (status != null)
            status.setText(OverlayService.running ? "状态: 运行中" : "状态: 未启动");
    }
}
