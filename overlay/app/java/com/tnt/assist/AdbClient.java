package com.tnt.assist;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/** 极简 ADB 客户端: 连本机 adbd(127.0.0.1:5555), 用 "root:" 服务把 adbd 提权成 root,
 *  然后以 root 身份跑 shell 命令(读游戏内存需要 root)。
 *  只实现 CNXN/OPEN/WRTE/OKAY/CLSE, 不依赖 adb 可执行文件。 */
public class AdbClient {
    private static final int A_CNXN = 0x4e584e43;
    private static final int A_AUTH = 0x48545541;
    private static final int A_OPEN = 0x4e45504f;
    private static final int A_OKAY = 0x59414b4f;
    private static final int A_CLSE = 0x45534c43;
    private static final int A_WRTE = 0x45545257;

    public interface LineSink {
        void onLine(String line);
    }

    private Socket sock;
    private InputStream in;
    private OutputStream out;
    private int nextId = 1;

    public synchronized void connect(int port, int timeoutMs) throws Exception {
        close();
        Socket s = new Socket();
        s.connect(new InetSocketAddress("127.0.0.1", port), timeoutMs);
        s.setSoTimeout(timeoutMs);
        s.setTcpNoDelay(true);
        sock = s;
        in = s.getInputStream();
        out = s.getOutputStream();
        nextId = 1;
        send(A_CNXN, 0x01000000, 4096, "host::\0".getBytes("UTF-8"));
        while (true) {
            int[] h = readHeader();
            byte[] d = readData(h[3]);
            if (h[0] == A_CNXN) return;
            if (h[0] == A_AUTH) throw new Exception("adbd 需要授权密钥");
        }
    }

    public synchronized void close() {
        try { if (sock != null) sock.close(); } catch (Exception ignored) {}
        sock = null;
        in = null;
        out = null;
    }

    /** 提权: 已经是 root 就返回 true; 否则发 root: 让 adbd 重启成 root 再连。 */
    public boolean ensureRoot() throws Exception {
        String id = exec("shell:id", 4000);
        if (id != null && id.contains("uid=0")) return true;
        try { exec("root:", 3000); } catch (Exception ignored) {}
        Thread.sleep(2200);
        connect(5555, 3000);
        id = exec("shell:id", 4000);
        return id != null && id.contains("uid=0");
    }

    /** 执行一条 shell 命令, 收集全部输出。 */
    public synchronized String exec(String service, int timeoutMs) throws Exception {
        int id = nextId++;
        send(A_OPEN, id, 0, (service + "\0").getBytes("UTF-8"));
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            int[] h = readHeader();
            byte[] d = readData(h[3]);
            if (h[2] != id) continue;          // 别的流的残留包, 丢掉
            if (h[0] == A_WRTE) {
                bos.write(d);
                send(A_OKAY, id, h[1], new byte[0]);
            } else if (h[0] == A_CLSE) {
                break;
            } else if (h[0] == A_OKAY) {
                continue;
            }
        }
        String out = new String(bos.toByteArray(), "UTF-8");
        android.util.Log.d("TNTASSIST", "exec[" + service + "] -> " + out.length() + "B "
                + out.substring(0, Math.min(100, out.length())).replace('\n', '|'));
        return out;
    }

    /** 流式执行(用于 memeat watch): 每读到一行回调一次; 空闲超时抛 SocketTimeoutException。 */
    public synchronized void stream(String service, LineSink sink, int idleMs) throws Exception {
        int id = nextId++;
        send(A_OPEN, id, 0, (service + "\0").getBytes("UTF-8"));
        Socket sk = sock;
        if (sk == null) throw new java.io.IOException("adb 连接已关闭");
        sk.setSoTimeout(idleMs);
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        while (true) {
            int[] h = readHeader();
            byte[] d = readData(h[3]);
            if (h[2] != id) continue;
            if (h[0] == A_WRTE) {
                buf.write(d);
                send(A_OKAY, id, h[1], new byte[0]);
                byte[] all = buf.toByteArray();
                int start = 0;
                for (int i = 0; i < all.length; i++) {
                    if (all[i] == '\n') {
                        sink.onLine(new String(all, start, i - start, "UTF-8"));
                        start = i + 1;
                    }
                }
                buf.reset();
                buf.write(all, start, all.length - start);
            } else if (h[0] == A_CLSE) {
                return;
            }
        }
    }

    private void send(int cmd, int a0, int a1, byte[] data) throws Exception {
        java.io.OutputStream o = out;
        if (o == null) throw new java.io.IOException("adb 连接已关闭");
        ByteArrayOutputStream bos = new ByteArrayOutputStream();
        int sum = 0;
        for (byte b : data) sum += (b & 0xFF);
        writeInt(bos, cmd);
        writeInt(bos, a0);
        writeInt(bos, a1);
        writeInt(bos, data.length);
        writeInt(bos, sum);
        writeInt(bos, cmd ^ 0xFFFFFFFF);
        bos.write(data);
        o.write(bos.toByteArray());
        o.flush();
    }

    private static void writeInt(ByteArrayOutputStream bos, int v) {
        bos.write(v & 0xFF);
        bos.write((v >> 8) & 0xFF);
        bos.write((v >> 16) & 0xFF);
        bos.write((v >> 24) & 0xFF);
    }

    private int[] readHeader() throws Exception {
        java.io.InputStream s = in;
        if (s == null) throw new java.io.IOException("adb 连接已关闭");
        byte[] hdr = new byte[24];
        int got = 0;
        while (got < 24) {
            int n = s.read(hdr, got, 24 - got);
            if (n <= 0) throw new Exception("adb 连接已断开");
            got += n;
        }
        int cmd = leInt(hdr, 0);
        int a0 = leInt(hdr, 4);
        int a1 = leInt(hdr, 8);
        int len = leInt(hdr, 12);
        int magic = leInt(hdr, 20);
        if ((cmd ^ 0xFFFFFFFF) != magic) throw new Exception("adb 包校验失败");
        return new int[]{cmd, a0, a1, len};
    }

    private byte[] readData(int len) throws Exception {
        if (len <= 0) return new byte[0];
        java.io.InputStream s = in;
        if (s == null) throw new java.io.IOException("adb 连接已关闭");
        byte[] d = new byte[len];
        int got = 0;
        while (got < len) {
            int n = s.read(d, got, len - got);
            if (n <= 0) throw new Exception("adb 数据中断");
            got += n;
        }
        return d;
    }

    private static int leInt(byte[] b, int off) {
        return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8)
                | ((b[off + 2] & 0xFF) << 16) | ((b[off + 3] & 0xFF) << 24);
    }
}
