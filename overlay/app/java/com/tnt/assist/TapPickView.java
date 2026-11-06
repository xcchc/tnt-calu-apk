package com.tnt.assist;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

/** 全屏点选层: 点任意处 = 要打的位置, 直接出力度/角度。
 *  绿圈 = 自机在屏幕上的位置(镜头锚点), 可拖动对准脚下; 右上角 × 退出。
 *  点选层不把触摸传给游戏(竞技模式本身没有点标功能)。 */
public class TapPickView extends View {

    public interface Listener {
        /** 点目标: self=(ax,ay) 自机屏幕锚点, tap=(tx,ty) 目标。返回要显示的行。 */
        String[] onPick(float ax, float ay, float tx, float ty);

        /** 用户把绿圈拖到了新位置(自机脚下)。 */
        void onSelfMoved(float ax, float ay);

        /** 点到了右上角 ×。 */
        void onExit();
    }

    private final Listener listener;
    private final Paint dot = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint cross = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint box = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint txt = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint hint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF closeRect = new RectF();
    private final RectF infoRect = new RectF();
    private float ax = -1f, ay = -1f;      // 自机锚点
    private float tx = -1f, ty = -1f;      // 目标
    private boolean dragging;
    private String[] info;

    public TapPickView(Context c, Listener l) {
        super(c);
        this.listener = l;
        dot.setStyle(Paint.Style.FILL);
        cross.setStyle(Paint.Style.STROKE);
        cross.setStrokeWidth(4f);
        box.setStyle(Paint.Style.FILL);
        txt.setColor(Color.WHITE);
        txt.setTextSize(30f);
        txt.setShadowLayer(6f, 2f, 2f, Color.BLACK);
        hint.setColor(Color.WHITE);
        hint.setTextSize(34f);
        hint.setShadowLayer(8f, 2f, 2f, Color.BLACK);
        setBackgroundColor(Color.TRANSPARENT);
    }

    /** 设置自机锚点(屏幕px)。 */
    public void setSelf(float x, float y) {
        ax = x;
        ay = y;
        invalidate();
    }

    public float selfX() {
        return ax;
    }

    public float selfY() {
        return ay;
    }

    /** 结果行(可空)。 */
    public void setResult(float tapX, float tapY, String[] lines) {
        this.tx = tapX;
        this.ty = tapY;
        this.info = lines;
        invalidate();
    }

    public void reset() {
        tx = ty = -1f;
        info = null;
        invalidate();
    }

    private boolean nearSelf(float x, float y) {
        float dx = x - ax, dy = y - ay;
        return dx * dx + dy * dy < 70f * 70f;
    }

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        String s = "点任意处 = 打那里 · 拖绿圈到脚下 = 对齐镜头 · × 退出";
        hint.setColor(0xFF7FE3FF);
        c.drawText(s, 40f, 90f, hint);

        closeRect.set(w - 120f, 30f, w - 20f, 130f);
        box.setColor(0x66000000);
        c.drawRoundRect(closeRect, 16f, 16f, box);
        cross.setColor(0xFFFF5555);
        c.drawLine(closeRect.left + 28f, closeRect.top + 28f, closeRect.right - 28f, closeRect.bottom - 28f, cross);
        c.drawLine(closeRect.right - 28f, closeRect.top + 28f, closeRect.left + 28f, closeRect.bottom - 28f, cross);

        if (ax >= 0f) {
            dot.setColor(dragging ? 0xFFAAFF44 : 0xFF33FF66);
            c.drawCircle(ax, ay, 10f, dot);
            cross.setColor(dragging ? 0xFFAAFF44 : 0xFF33FF66);
            c.drawCircle(ax, ay, 34f, cross);
            txt.setColor(0xFF33FF66);
            c.drawText("我", ax + 40f, ay + 10f, txt);
        }
        if (tx >= 0f) {
            cross.setColor(0xFFFF3355);
            c.drawCircle(tx, ty, 26f, cross);
            c.drawLine(tx - 44f, ty, tx + 44f, ty, cross);
            c.drawLine(tx, ty - 44f, tx, ty + 44f, cross);
        }
        if (info != null && tx >= 0f) {
            float lh = 38f;
            float bw = 0f;
            for (String line : info) {
                bw = Math.max(bw, txt.measureText(line));
            }
            float left = Math.min(tx + 40f, w - bw - 60f);
            float top = Math.min(ty + 40f, h - lh * info.length - 40f);
            infoRect.set(left - 16f, top - 34f, left + bw + 16f, top + lh * info.length + 6f);
            box.setColor(0xAA000000);
            c.drawRoundRect(infoRect, 12f, 12f, box);
            txt.setColor(Color.WHITE);
            for (int i = 0; i < info.length; i++) {
                c.drawText(info[i], left, top + lh * i, txt);
            }
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        float x = e.getX(), y = e.getY();
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                if (closeRect.contains(x, y)) {
                    return true;
                }
                dragging = ax >= 0f && nearSelf(x, y);
                if (dragging) {
                    invalidate();
                }
                return true;
            case MotionEvent.ACTION_MOVE:
                if (dragging) {
                    ax = x;
                    ay = y;
                    invalidate();
                }
                return true;
            case MotionEvent.ACTION_UP:
                if (closeRect.contains(x, y)) {
                    listener.onExit();
                    return true;
                }
                if (dragging) {
                    dragging = false;
                    ax = x;
                    ay = y;
                    if (listener != null) {
                        listener.onSelfMoved(ax, ay);
                        if (tx >= 0f) {
                            setResult(tx, ty, listener.onPick(ax, ay, tx, ty));
                        }
                    }
                    invalidate();
                    return true;
                }
                if (listener != null) {
                    setResult(x, y, listener.onPick(ax, ay, x, y));
                }
                return true;
            default:
                return true;
        }
    }
}
