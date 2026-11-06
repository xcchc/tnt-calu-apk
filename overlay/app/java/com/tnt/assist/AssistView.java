/*
 * Decompiled with CFR 0.152.
 * 
 * Could not load the following classes:
 *  android.content.Context
 *  android.content.SharedPreferences
 *  android.graphics.Canvas
 *  android.graphics.Color
 *  android.graphics.Paint
 *  android.graphics.Paint$Align
 *  android.graphics.Paint$FontMetrics
 *  android.graphics.Paint$Style
 *  android.graphics.RectF
 *  android.graphics.Typeface
 *  android.os.SystemClock
 *  android.view.MotionEvent
 *  android.view.View
 *  org.json.JSONArray
 *  org.json.JSONObject
 */
package com.tnt.assist;

import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;
import com.tnt.assist.FrameBuilder;
import java.util.ArrayList;
import org.json.JSONArray;
import org.json.JSONObject;

public class AssistView
extends View {
    private static final String PREFS = "assist_overlay";
    private static final float BASE_LINE_H = 21.0f;
    private static final float BASE_PAD = 8.0f;
    private static final float BASE_TEXT = 12.0f;
    private static final float BASE_ACCENT = 13.0f;
    private static final float BASE_BIG = 14.0f;
    private static final float BASE_CTRL_H = 24.0f;
    private static final float BASE_GRIP = 26.0f;
    private static final float MIN_SCALE = 0.6f;
    private static final float MAX_SCALE = 2.6f;
    private final Paint bg = new Paint(1);
    private final Paint text = new Paint(1);
    private final Paint accent = new Paint(1);
    private final Paint big = new Paint(1);
    private final Paint shadow = new Paint(1);
    private final Paint ctrlText = new Paint(1);
    private final Paint ctrlBg = new Paint(1);
    private final Paint ctrlBgOff = new Paint(1);
    private final Paint ctrlBgOn = new Paint(1);
    private final Paint ctrlLine = new Paint(1);
    private final RectF[] ctrlRects = new RectF[6];
    private final RectF gripRect = new RectF();
    private final RectF ctrlBarRect = new RectF();
    private final String[] ctrlLabels = new String[]{"\u5c0f", "\u5927", "\u91cd\u7f6e", "\u7b80", "\u6211", "\u70b9"};
    private String[] lines = new String[]{"TNT \u8f85\u52a9\u5df2\u542f\u52a8", "\u7b49\u5f85\u8fdb\u5165\u5bf9\u6218..."};
    private long lastAt = 0L;
    private String dtText = "--";
    private SharedPreferences prefs;
    private ScaleListener scaleListener;
    private PickListener pickListener;
    private ResizeListener resizeListener;
    private float scale = 1.0f;
    private float LINE_H = 21.0f;
    private float PAD = 8.0f;
    private float CTRL_H = 24.0f;
    private float GRIP = 26.0f;
    private JSONObject lastFrame;
    private boolean concise;
    private boolean mateOk;
    private boolean mateView;
    private boolean btnOn3;
    private boolean btnOn4;
    private boolean cornerDrag;
    private float downX;
    private float downY;
    private float cRawX0;
    private float cRawY0;
    private float cDownX;
    private float cDownY;
    private float cStartW;
    private float cStartH;
    private float cAnchorX;
    private float cAnchorY;
    private float cStartScale;

    public AssistView(Context context) {
        super(context);
        this.bg.setColor(Color.argb((int)36, (int)18, (int)28, (int)42));
        this.shadow.setColor(Color.argb((int)190, (int)0, (int)0, (int)0));
        this.shadow.setStyle(Paint.Style.STROKE);
        this.shadow.setTypeface(Typeface.MONOSPACE);
        this.text.setColor(Color.argb((int)215, (int)235, (int)245, (int)255));
        this.text.setTypeface(Typeface.MONOSPACE);
        this.accent.setColor(Color.argb((int)230, (int)125, (int)220, (int)255));
        this.accent.setTypeface(Typeface.MONOSPACE);
        this.accent.setFakeBoldText(true);
        this.big.setColor(Color.argb((int)230, (int)255, (int)220, (int)105));
        this.big.setTypeface(Typeface.MONOSPACE);
        this.big.setFakeBoldText(true);
        this.ctrlText.setColor(Color.argb((int)235, (int)235, (int)245, (int)255));
        this.ctrlText.setTypeface(Typeface.MONOSPACE);
        this.ctrlText.setTextAlign(Paint.Align.CENTER);
        this.ctrlBg.setColor(Color.argb((int)70, (int)125, (int)220, (int)255));
        this.ctrlBgOff.setColor(Color.argb((int)30, (int)125, (int)220, (int)255));
        this.ctrlBgOn.setColor(Color.argb((int)150, (int)90, (int)200, (int)255));
        this.ctrlLine.setColor(Color.argb((int)200, (int)180, (int)215, (int)245));
        this.ctrlLine.setStrokeWidth(1.4f);
        for (int i = 0; i < this.ctrlRects.length; ++i) {
            this.ctrlRects[i] = new RectF();
        }
        this.prefs = context.getSharedPreferences(PREFS, 0);
        this.concise = this.prefs.getBoolean("concise", false);
        FrameBuilder.viewMate = this.prefs.getBoolean("view_mate", false);
        this.setScale(this.prefs.getFloat("scale", 1.0f), false);
    }

    public void setPickListener(PickListener l) {
        this.pickListener = l;
    }

    public JSONObject lastFrameObj() {
        return this.lastFrame;
    }

    public void setScaleListener(ScaleListener scaleListener) {
        this.scaleListener = scaleListener;
    }

    public void setResizeListener(ResizeListener resizeListener) {
        this.resizeListener = resizeListener;
    }

    private void applyScale() {
        this.LINE_H = 21.0f * this.scale;
        this.PAD = 8.0f * this.scale;
        this.CTRL_H = 24.0f * this.scale;
        this.GRIP = Math.max(20.0f, 26.0f * this.scale);
        this.text.setTextSize(15.0f * this.scale);
        this.accent.setTextSize(17.0f * this.scale);
        this.big.setTextSize(18.0f * this.scale);
        this.ctrlText.setTextSize(15.0f * this.scale);
        this.shadow.setStrokeWidth(2.2f * this.scale);
    }

    private void setScale(float f, boolean bl) {
        this.scale = Math.max(0.6f, Math.min(2.6f, f));
        this.applyScale();
        this.requestLayout();
        this.invalidate();
        if (bl) {
            this.prefs.edit().putFloat("scale", this.scale).apply();
            if (this.scaleListener != null) {
                this.scaleListener.onScaleChanged(this.scale);
            }
        }
    }

    private float measuredWidth() {
        float f = 0.0f;
        for (String string2 : this.lines) {
            f = Math.max(f, this.text.measureText(string2));
            f = Math.max(f, this.accent.measureText(string2));
            f = Math.max(f, this.big.measureText(string2));
        }
        return f;
    }

    private float barWidth() {
        float f = 0.0f;
        float f2 = 4.0f * this.scale;
        for (int i = 0; i < this.ctrlLabels.length; ++i) {
            float f3 = this.ctrlText.measureText(this.ctrlLabels[i]);
            f += Math.max(20.0f * this.scale, f3 + 10.0f * this.scale);
        }
        return f += f2 * (float)(this.ctrlLabels.length - 1);
    }

    private float panelWidth() {
        float f = this.GRIP + this.measuredWidth() + this.PAD;
        float f2 = this.GRIP + this.barWidth() + this.PAD;
        return Math.max(f, f2);
    }

    private void layoutControls(float f, float f2) {
        float f3 = 4.0f * this.scale;
        float f4 = Math.max(12.0f, this.CTRL_H - 6.0f * this.scale);
        float f5 = this.GRIP;
        for (int i = 0; i < this.ctrlLabels.length; ++i) {
            float f6 = this.ctrlText.measureText(this.ctrlLabels[i]);
            float f7 = Math.max(20.0f * this.scale, f6 + 10.0f * this.scale);
            float f8 = f2 + (this.CTRL_H - f4) / 2.0f;
            this.ctrlRects[i].set(f5, f8, f5 + f7, f8 + f4);
            f5 += f7 + f3;
        }
        this.gripRect.set(0.0f, 0.0f, this.GRIP, this.GRIP);
    }

    void updateJson(String string2) {
        long l = SystemClock.elapsedRealtime();
        if (this.lastAt > 0L) {
            long l2 = l - this.lastAt;
            this.dtText = l2 < 1000L ? l2 + "ms" : String.format("%.1fs", (double)l2 / 1000.0);
        }
        this.lastAt = l;
        try {
            this.lastFrame = new JSONObject(string2);
        }
        catch (Exception exception) {
            this.lastFrame = null;
            this.lines = new String[]{"parse error", exception.toString()};
            this.requestLayout();
            this.invalidate();
            return;
        }
        this.render();
    }

    private void render() {
        JSONObject jSONObject = this.lastFrame;
        if (jSONObject == null) {
            return;
        }
        String string2 = jSONObject.optString("state", "idle");
        if ("idle".equals(string2)) {
            this.mateOk = false;
            this.mateView = false;
            this.btnOn4 = false;
            this.lines = new String[]{"TNT \u8f85\u52a9", "\u672a\u6218(\u68c0\u6d4b\u4e2d)...  " + this.dtText};
            this.invalidate();
            return;
        }
        if ("error".equals(string2) || "stall".equals(string2)) {
            this.mateOk = false;
            this.mateView = false;
            this.btnOn4 = false;
            this.lines = new String[]{"TNT \u8f85\u52a9 \u00b7 \u8bfb\u53d6\u5f02\u5e38", jSONObject.optString("message", "\u65e0\u6cd5\u8bfb\u53d6\u6e38\u620f\u72b6\u6001"), "\u539f\u56e0: " + jSONObject.optString("detail", "?") + "  " + this.dtText};
            this.requestLayout();
            this.invalidate();
            return;
        }
        ArrayList<String> arrayList = new ArrayList<String>();
        this.mateOk = jSONObject.optBoolean("mate_ok", false);
        this.mateView = "mate".equals(jSONObject.optString("view"));
        this.btnOn3 = this.concise;
        this.btnOn4 = this.mateView;
        String string3 = "right".equals(jSONObject.optString("facing")) ? "\u2192" : ("left".equals(jSONObject.optString("facing")) ? "\u2190" : "?");
        String string4 = jSONObject.optString("actual_angle") + "\u00b0" + string3;
        String string5 = this.mateView ? "\u00b7\u961f\u53cb\u89c6\u89d2" : "";
        String string6 = jSONObject.optBoolean("self_titan", false) ? "\u53d8\u5927" : "\u6b63\u5e38";
        arrayList.add(String.format("\u56de\u5408%d%s   \u98ce%s %s", jSONObject.optInt("round"), string5, jSONObject.optString("wind", "--"), this.dtText));
        arrayList.add(String.format("%s  \u529b%s  %s", string4, jSONObject.optString("force_pct"), string6));
        JSONArray jSONArray = jSONObject.optJSONArray("targets");
        if (jSONArray == null || jSONArray.length() == 0) {
            arrayList.add(jSONObject.optString("target_note", "\u76ee\u6807: \u65e0"));
        } else {
            String string7;
            JSONObject object;
            int n;
            int n2 = jSONArray.length();
            String[] stringArray = new String[n2];
            float f = 0.0f;
            for (n = 0; n < n2; ++n) {
                object = jSONArray.optJSONObject(n);
                if (object == null) {
                    object = new JSONObject();
                }
                if ((string7 = object.optString("name", "").trim()).isEmpty()) {
                    string7 = "\u654c" + (n + 1);
                }
                stringArray[n] = string7;
                float f2 = this.text.measureText(string7);
                if (!(f2 > f)) continue;
                f = f2;
            }
            for (n = 0; n < n2; ++n) {
                stringArray[n] = this.padName(stringArray[n], f);
            }
            if (this.concise) {
                for (n = 0; n < n2; n += 2) {
                    StringBuilder sb = new StringBuilder();
                    sb.append(this.cell(jSONArray, n, stringArray[n]));
                    if (n + 1 < n2) {
                        sb.append("  ");
                        sb.append(this.cell(jSONArray, n + 1, stringArray[n + 1]));
                    }
                    arrayList.add(sb.toString());
                }
            } else {
                for (n = 0; n < n2; ++n) {
                    object = jSONArray.optJSONObject(n);
                    if (object == null) continue;
                    JSONArray jaPos = object.optJSONArray("pos");
                    String string8 = jaPos != null && jaPos.length() >= 2 ? String.format("(%d,%d)", jaPos.optInt(0), jaPos.optInt(1)) : "";
                    String string9 = "right".equals(object.optString("facing")) ? "\u2192" : "\u2190";
                    String[] stringArray2 = this.angleForce((JSONObject)object);
                    String string10 = object.optBoolean("dead", false) ? "\uff08\u6b7b\uff09" : "";
                    String string11 = object.optString("wind_kind", "");
                    String string12 = string11.isEmpty() ? "" : "(" + string11 + ")";
                    String string13 = this.stateTag((JSONObject)object);
                    arrayList.add(String.format("\u654c%d %s %s \u8ddd%s %s %s\u00b0%s %s%s%s%s", n + 1, stringArray[n], string8, object.optString("dist_ju"), object.optString("height_note", "?"), stringArray2[0], string9, stringArray2[1], string10, string13, string12));
                }
            }
        }
        this.lines = arrayList.toArray(new String[0]);
        this.requestLayout();
        this.invalidate();
    }

    private String cell(JSONArray jSONArray, int n, String string2) {
        JSONObject jSONObject = jSONArray.optJSONObject(n);
        if (jSONObject == null) {
            return "";
        }
        String[] stringArray = this.angleForce(jSONObject);
        String string3 = "right".equals(jSONObject.optString("facing")) ? "\u2192" : "\u2190";
        String string4 = jSONObject.optBoolean("dead", false) ? "\uff08\u6b7b\uff09" : "";
        return String.format("%s %s\u00b0%s %s%s%s", string2, stringArray[0], string3, stringArray[1], string4, this.stateTag(jSONObject));
    }

    private String stateTag(JSONObject jSONObject) {
        StringBuilder stringBuilder = new StringBuilder();
        if (jSONObject.optBoolean("hide", false)) {
            stringBuilder.append("\uff08\u9690\uff09");
        }
        if (jSONObject.optBoolean("titan", false)) {
            stringBuilder.append("\uff08\u955c\uff09");
        }
        return stringBuilder.toString();
    }

    private String[] angleForce(JSONObject jSONObject) {
        double d;
        double d2 = jSONObject.optDouble("angle", 0.0);
        double d3 = d = jSONObject.isNull("force") ? Double.NaN : jSONObject.optDouble("force", Double.NaN);
        if (!Double.isNaN(d) && d > 0.05) {
            return new String[]{AssistView.trimNum(d2), String.format("%.1f\u529b", d)};
        }
        return new String[]{AssistView.trimNum(d2), "\u6253\u4e0d\u5230"};
    }

    private String padName(String string2, float f) {
        StringBuilder stringBuilder = new StringBuilder(string2);
        float f2 = Math.max(1.0f, this.text.measureText(" "));
        int n = 0;
        while (this.text.measureText(stringBuilder.toString()) + f2 * 0.5f < f && n++ < 40) {
            stringBuilder.append(' ');
        }
        return stringBuilder.toString();
    }

    private static String trimNum(double d) {
        if (Math.abs(d - Math.rint(d)) < 0.05) {
            return String.valueOf((long)Math.rint(d));
        }
        return String.format("%.1f", d);
    }

    boolean isInControlArea(float f, float f2) {
        return this.ctrlBarRect.contains(f, f2) || this.gripRect.contains(f, f2);
    }

    public boolean onTouchEvent(MotionEvent motionEvent) {
        switch (motionEvent.getActionMasked()) {
            case 0: {
                if (this.gripRect.contains(motionEvent.getX(), motionEvent.getY())) {
                    this.startCornerDrag(motionEvent);
                    return true;
                }
                this.downX = motionEvent.getX();
                this.downY = motionEvent.getY();
                return true;
            }
            case 2: {
                if (this.cornerDrag) {
                    this.updateCornerDrag(motionEvent);
                    return true;
                }
                return true;
            }
            case 1: 
            case 3: {
                if (this.cornerDrag) {
                    this.cornerDrag = false;
                    this.prefs.edit().putFloat("scale", this.scale).apply();
                    return true;
                }
                if (motionEvent.getActionMasked() == 1) {
                    for (int i = 0; i < this.ctrlRects.length; ++i) {
                        if (!this.ctrlRects[i].contains(motionEvent.getX(), motionEvent.getY())) continue;
                        this.onCtrl(i);
                        return true;
                    }
                }
                return true;
            }
        }
        return true;
    }

    private void onCtrl(int n) {
        switch (n) {
            case 0: {
                this.setScale(this.scale - 0.15f, true);
                break;
            }
            case 1: {
                this.setScale(this.scale + 0.15f, true);
                break;
            }
            case 2: {
                this.setScale(1.0f, true);
                break;
            }
            case 3: {
                this.concise = !this.concise;
                this.prefs.edit().putBoolean("concise", this.concise).apply();
                this.render();
                break;
            }
            case 5: {
                if (this.pickListener != null) {
                    this.pickListener.onTogglePick();
                }
                break;
            }
            case 4: {
                if (!this.mateOk) {
                    return;
                }
                FrameBuilder.viewMate = !FrameBuilder.viewMate;
                this.prefs.edit().putBoolean("view_mate", FrameBuilder.viewMate).apply();
                this.render();
                break;
            }
        }
    }

    private void startCornerDrag(MotionEvent motionEvent) {
        this.cornerDrag = true;
        this.cRawX0 = motionEvent.getRawX();
        this.cRawY0 = motionEvent.getRawY();
        this.cDownX = motionEvent.getX();
        this.cDownY = motionEvent.getY();
        this.cStartW = this.getWidth() > 0 ? (float)this.getWidth() : this.panelWidth();
        this.cStartH = this.getHeight() > 0 ? (float)this.getHeight() : this.PAD + (float)this.lines.length * this.LINE_H + this.CTRL_H + this.PAD;
        this.cAnchorX = this.cRawX0 + (this.cStartW - this.cDownX);
        this.cAnchorY = this.cRawY0 + (this.cStartH - this.cDownY);
        this.cStartScale = this.scale;
    }

    private void updateCornerDrag(MotionEvent motionEvent) {
        float f = motionEvent.getRawX();
        float f2 = motionEvent.getRawY();
        float f3 = this.cAnchorX - f;
        float f4 = this.cAnchorY - f2;
        if (f3 < 40.0f) {
            f3 = 40.0f;
        }
        if (f4 < 20.0f) {
            f4 = 20.0f;
        }
        float f5 = (float)Math.hypot(this.cStartW - this.cDownX, this.cStartH - this.cDownY);
        float f6 = (float)Math.hypot(f3, f4);
        float f7 = f5 > 1.0f ? f6 / f5 : 1.0f;
        float f8 = Math.max(0.6f, Math.min(2.6f, this.cStartScale * f7));
        if (Math.abs(f8 - this.scale) > 0.004f) {
            this.scale = f8;
            this.applyScale();
            this.requestLayout();
            this.invalidate();
        }
        int n = Math.round(f - this.cDownX);
        int n2 = Math.round(f2 - this.cDownY);
        if (n < 0) {
            n = 0;
        }
        if (n2 < 0) {
            n2 = 0;
        }
        if (this.resizeListener != null) {
            this.resizeListener.onResize(this.scale, n, n2);
        }
    }

    protected void onMeasure(int n, int n2) {
        float f = this.panelWidth();
        float f2 = this.PAD + (float)this.lines.length * this.LINE_H;
        float f3 = f2 + this.CTRL_H + this.PAD;
        this.layoutControls(f, f2);
        this.ctrlBarRect.set(0.0f, f2, f, f2 + this.CTRL_H);
        this.setMeasuredDimension((int)Math.ceil(f), (int)Math.ceil(f3));
    }

    protected void onDraw(Canvas canvas) {
        int n;
        float f = this.panelWidth();
        float f2 = this.PAD + (float)this.lines.length * this.LINE_H;
        float f3 = f2 + this.CTRL_H + this.PAD;
        this.layoutControls(f, f2);
        this.ctrlBarRect.set(0.0f, f2, f, f2 + this.CTRL_H);
        canvas.drawRoundRect(0.0f, 0.0f, f, f3, 10.0f * this.scale, 10.0f * this.scale, this.bg);
        float f4 = this.PAD + 17.0f * this.scale;
        for (n = 0; n < this.lines.length; ++n) {
            Paint paint = this.text;
            if (n == 0) {
                paint = this.accent;
            } else if (this.lines[n].startsWith(">>")) {
                paint = this.big;
            }
            this.shadow.setTextSize(paint.getTextSize());
            this.shadow.setFakeBoldText(paint.isFakeBoldText());
            canvas.drawText(this.lines[n], this.GRIP, f4, this.shadow);
            canvas.drawText(this.lines[n], this.GRIP, f4, paint);
            f4 += this.LINE_H;
        }
        canvas.drawRoundRect(0.0f, 0.0f, this.GRIP, this.GRIP, 4.0f * this.scale, 4.0f * this.scale, this.ctrlBg);
        for (n = 0; n < 3; ++n) {
            float f5 = this.GRIP * (0.3f + (float)n * 0.24f);
            canvas.drawLine(0.0f, this.GRIP - f5, f5, 0.0f, this.ctrlLine);
        }
        canvas.drawLine(this.GRIP, f2, f - this.PAD, f2, this.ctrlLine);
        Paint.FontMetrics fontMetrics = this.ctrlText.getFontMetrics();
        for (int i = 0; i < this.ctrlLabels.length; ++i) {
            RectF rectF = this.ctrlRects[i];
            Paint paint = this.ctrlBg;
            if (i == 3) {
                paint = this.btnOn3 ? this.ctrlBgOn : this.ctrlBgOff;
            } else if (i == 4) {
                paint = !this.mateOk ? this.ctrlBgOff : (this.btnOn4 ? this.ctrlBgOn : this.ctrlBg);
            }
            canvas.drawRoundRect(rectF, 4.0f * this.scale, 4.0f * this.scale, paint);
            float f6 = rectF.centerY() - (fontMetrics.ascent + fontMetrics.descent) / 2.0f;
            canvas.drawText(this.ctrlLabels[i], rectF.centerX(), f6, this.ctrlText);
        }
    }

    public static interface PickListener {
        void onTogglePick();
    }

    public static interface ScaleListener {
        public void onScaleChanged(float var1);
    }

    public static interface ResizeListener {
        public void onResize(float var1, int var2, int var3);
    }
}

