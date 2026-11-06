/* tnt_memeat —— TNT 战斗内存读取器 (探测/取证用, arm64)
 *
 * 目标版本: TNT v261016.1 (lib/arm64-v8a/libgame_arm64-v8a.so)
 *
 * 读内存只用 process_vm_readv, 绝不碰 /proc/PID/mem:
 *   /proc/PID/mem 会触发爱加密壳的反 dump 逻辑, 进程直接自杀;
 *   process_vm_readv 走的是同一套 procfs 接口但没有 mem 文件的读写语义, 壳不察觉。
 *
 * 输出都是单行 k=v, 由设备端常驻进程流式吐出;
 * 上层 core/device_reader.py 用一次 adb shell 调用接收 (实测 17 Hz, 58 ms/帧)。
 *
 * 编译 (zig 0.13, 静态链, 产物约 1.4 MB):
 *   zig cc -target aarch64-linux-musl -O2 -static -o tnt_memeat tnt_memeat.c
 *
 * 子命令:
 *   tnt_memeat dump  <pid> <hex_addr> <dec_size>          原始字节 -> stdout
 *   tnt_memeat batch <pid> <hex_addr:dec_size> [...]      多段原始字节拼接输出
 *   tnt_memeat scan  <pid> <hex_addr> <dec_size> <hex_u64> [max_hits]
 *                                                         在区域内找给定的 8 字节 qword
 *   tnt_memeat battle <pid> <hex_base> <hex_console> <hex_panel>
 *                                                         按已知对象地址打印一帧 k=v 状态
 *   tnt_memeat selftest                                    自读自校验, 验证读取通道
 *
 * 退出码: 0 成功 / 1 参数错误 / 2 读取失败 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <math.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <time.h>

/* memmem 在 bionic libc 的公开头文件里没有声明, 这里自己声明;
 * 不用 _GNU_SOURCE 是为了不影响已包含头文件里的 int 类型定义。 */
extern void *memmem(const void *haystack, size_t haystacklen,
                    const void *needle, size_t needlelen);

#ifndef SYS_process_vm_readv
#define SYS_process_vm_readv 270
#endif
#ifndef SYS_process_vm_writev
#define SYS_process_vm_writev 271
#endif

/* ---- 极简 MD5, 只为复算 BattleWind::hashWind(int) ---- */
typedef struct {
    uint32_t h[4];
    uint64_t bytes;
    unsigned char buf[64];
} TntMd5;

static uint32_t tnt_rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

static void tnt_md5_block(TntMd5 *m, const unsigned char *p)
{
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
        0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
        0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
        0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const unsigned char S[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    uint32_t M[16];
    for (int i = 0; i < 16; i++)
        M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    uint32_t a = m->h[0], b = m->h[1], c = m->h[2], d = m->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7*i) & 15; }
        uint32_t t = d;
        d = c; c = b;
        b = b + tnt_rotl(a + f + K[i] + M[g], S[i]);
        a = t;
    }
    m->h[0] += a; m->h[1] += b; m->h[2] += c; m->h[3] += d;
}

static void tnt_md5_init(TntMd5 *m)
{
    m->h[0] = 0x67452301; m->h[1] = 0xefcdab89;
    m->h[2] = 0x98badcfe; m->h[3] = 0x10325476;
    m->bytes = 0;
}

static void tnt_md5_update(TntMd5 *m, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t fill = (size_t)(m->bytes & 63);
    m->bytes += len;
    if (fill) {
        size_t need = 64 - fill;
        if (need > len) need = len;
        memcpy(m->buf + fill, p, need);
        p += need; len -= need;
        if (fill + need == 64) tnt_md5_block(m, m->buf);
        else return;
    }
    while (len >= 64) { tnt_md5_block(m, p); p += 64; len -= 64; }
    if (len) memcpy(m->buf, p, len);
}

static void tnt_md5_final(TntMd5 *m, unsigned char out[16])
{
    uint64_t bits = m->bytes * 8;
    unsigned char pad[80] = {0x80};
    size_t fill = (size_t)(m->bytes & 63);
    size_t padlen = (fill < 56) ? (56 - fill) : (120 - fill);
    for (int i = 0; i < 8; i++) pad[padlen + i] = (unsigned char)(bits >> (8*i));
    tnt_md5_update(m, pad, padlen + 8);
    for (int i = 0; i < 4; i++) {
        out[i*4]   = (unsigned char)(m->h[i]);
        out[i*4+1] = (unsigned char)(m->h[i] >> 8);
        out[i*4+2] = (unsigned char)(m->h[i] >> 16);
        out[i*4+3] = (unsigned char)(m->h[i] >> 24);
    }
}

static void tnt_md5(const void *data, size_t len, unsigned char out[16])
{
    TntMd5 m; tnt_md5_init(&m); tnt_md5_update(&m, data, len); tnt_md5_final(&m, out);
}

static ssize_t pv_read(pid_t pid, unsigned long addr, void *buf, size_t n)
{
    struct iovec local = { .iov_base = buf, .iov_len = n };
    struct iovec remote = { .iov_base = (void *)addr, .iov_len = n };
    return syscall(SYS_process_vm_readv, pid, &local, 1, &remote, 1, 0UL);
}

static int read_region(pid_t pid, unsigned long addr, unsigned char *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > 1048576) chunk = 1048576;
        ssize_t n = pv_read(pid, addr + done, buf + done, chunk);
        if (n <= 0) {
            fprintf(stderr, "process_vm_readv(0x%lx, %zu) failed: %s\n",
                    addr + done, chunk, strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static ssize_t pv_write(pid_t pid, unsigned long addr, const void *buf, size_t n)
{
    struct iovec local = { .iov_base = (void *)buf, .iov_len = n };
    struct iovec remote = { .iov_base = (void *)addr, .iov_len = n };
    return syscall(SYS_process_vm_writev, pid, &local, 1, &remote, 1, 0UL);
}

static int write_region(pid_t pid, unsigned long addr, const unsigned char *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        size_t chunk = size - done;
        if (chunk > 1048576) chunk = 1048576;
        ssize_t n = pv_write(pid, addr + done, buf + done, chunk);
        if (n <= 0) {
            fprintf(stderr, "process_vm_writev(0x%lx, %zu) failed: %s\n",
                    addr + done, chunk, strerror(errno));
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int write_all(const unsigned char *buf, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(1, buf + done, size - done);
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

/* 结构体偏移表 (v261016.1, 与 docs/layout-v261016.json 对应)。
 * 全部是模块内相对偏移; 运行时地址 = module_base + 偏移。
 * 来源: 运行时内存镜像提取的符号表 + 战斗实测验证。 */
#define VT_CONSOLE        0x156E188ULL
#define OFF_CANFIRE       0x21
#define OFF_ROUND         0x28
#define OFF_ANGLE_OBJ     0x60
#define OFF_SELF_OBJ      0xA0
#define OFF_ACT_ROLE      0x90   /* BattleConsole::setAct 存的是当前行动者 shared_ptr<BattleRole> (可能是队友) */
#define OFF_MYTURN        0x20   /* setAct 里的布尔位: 非 0 == 轮到我出手 */
#define OFF_CTX_OBJ       0xC8
#define OFF_ANGLE_LABEL   0xE0   /* 角度标签对象指针: 串在 angle_obj 上, 实测不可靠, 仅保留备查 */
#define OFF_TERRAIN_OBJ   0xE8   /* 地形对象: 取其 QGraphicsItem rotation 作为地形角 */
#define OFF_FORCE_OBJ     0x100
#define OFF_ANGLE_VAL     0xA0   /* angle_obj +0xA0: 武器角度 (45..78) */
#define OFF_FORCE_VAL     0x20   /* force_obj +0x20: 力度 f64 */
#define OFF_SELF_ROLE     0xC8   /* self_obj +0xC8: 自己的 BattleRole* */
#define OFF_ROLE_X        0x88
#define OFF_ROLE_Y        0x8C
#define OFF_ROLE_UID      0x68
#define OFF_ROLE_TEAM     0xA8    /* BattlePlayer::setTeam 写入的队伍号 (int) */
#define OFF_ROLE_NAME     0xD8    /* BattlePlayer -> QString name */
#define OFF_ROLE_HP       0x60    /* BattleRole -> hp (int); setHp 写入, <1 视为阵亡 */
#define OFF_ROLE_HP_MAX   0x6C    /* BattleRole -> maxHp (int) */
#define OFF_SPRITE_ROLE   0xC8    /* BattleSpriteRole -> BattleRole* (精灵与角色对象的桥) */
#define SINST_CONFIG      0x1598F68ULL
#define OFF_CONFIG_WIND   0x470   /* Config+0x470 —— 已证伪: 不是风力, 两场战斗全程恒为 69 */
#define OFF_PANEL_HASH    0x90
#define OFF_PANEL_WIND    0x208  /* PanelBattle -> BattleWind* (showWind 之后 setValue) */
#define OFF_BW_TEXT       0xA8   /* BattleWind -> QByteArray 哈希 (setValue 里 x21 = x19 + 0xA8) */
#define OFF_BW_TEXT_OLD   0xC8   /* BattleWind -> 旧版文字 QString (v260906.1 布局; 本版本已迁到 +0xA8) */
#define OFF_BW_TEXT_ITEM  0xE8   /* BattleWind -> QGraphicsTextItem* (setPlainText) */
#define OFF_BW_ARROW      0xE0   /* BattleWind -> BattleWindArrow* (m11 < 0 表示风朝左) */
#define OFF_QGI_DPTR      0x08   /* QGraphicsItem -> d_ptr */
#define OFF_QGID_TRANSFORM 0x68  /* QGraphicsItemPrivate -> TransformData* */
#define OFF_TRANSFORM_ROT 0x60   /* TransformData -> rotation(double) */

/* 以下函数定义在文件后段, 这里先给前向声明。
 * probe 模式会先粗扫再逐个下钻, 所以这些帮助函数要提前可见。 */
static uint64_t find_module_base(pid_t pid);
static void hints_load(pid_t pid);
static void scan_instances(pid_t pid, uint64_t base, uint64_t *console,
                           uint64_t *panel, size_t budget);

/* 读 QGraphicsItem 的旋转角与 m11:
 *   item -> d_ptr(+0x08) -> TransformData(+0x68) -> {m11, rotation}
 * 返回 0 成功, -1 失败。 */
static int read_qg_transform(pid_t pid, uint64_t item, double *rot, double *m11)
{
    uint64_t dptr = 0, td = 0;
    *rot = 0.0;
    *m11 = 1.0;
    if (!item) return -1;
    if (pv_read(pid, item + OFF_QGI_DPTR, &dptr, 8) != 8 || !dptr) return -1;
    if (pv_read(pid, dptr + OFF_QGID_TRANSFORM, &td, 8) == 8 && td) {
        if (pv_read(pid, td, m11, 8) != 8) return -1;
        if (pv_read(pid, td + OFF_TRANSFORM_ROT, rot, 8) != 8) return -1;
        return 0;
    }
    /* 退化路径: 这个 Qt 版本把 QTransform 直接内联在 QGraphicsItemPrivate +0xA0:
     * m11 +0xA0, m12 +0xA8, m21 +0xB0, m22 +0xB8。
     * rotation = atan2(-m12, m11), 与 QGraphicsItem::rotation 等价。 */
    double m12 = 0.0, m21 = 0.0, m22 = 0.0;
    if (pv_read(pid, dptr + 0xA0, m11, 8) != 8) return -1;
    if (pv_read(pid, dptr + 0xA8, &m12, 8) != 8) return -1;
    if (pv_read(pid, dptr + 0xB0, &m21, 8) != 8) return -1;
    if (pv_read(pid, dptr + 0xB8, &m22, 8) != 8) return -1;
    *rot = atan2(-m12, *m11) * 180.0 / 3.14159265358979323846;
    (void)m21; (void)m22;
    return 0;
}

/* PanelBattle 的角色表: uid -> sprite(BattleSpriteRole);
 * sprite+0xC8 就是 BattleRole, 位置字段与之 +0x88 相同。
 * 精灵由服务器轨迹动画驱动, 角色对象由同步包驱动, 两者会分叉,
 * 所以这里把两份坐标都读出来交给上层判断。 */
static int read_role_live_pos(pid_t pid, uint64_t role, int32_t xy[2])
{
    /* 读 BattlePlayer+0x88 的服务器同步坐标; 飞行/隐身期间会冻结 */
    return pv_read(pid, role + OFF_ROLE_X, xy, 8) == 8;
}

/* BattleSpritePlayer/Self 是 QGraphicsItem: 基类 +0x10, d_ptr 在 +0x18;
 * QGraphicsItemPrivate::pos(QPointF) 在 d_ptr+0x58/+0x60, 由 move/moveTo 更新。
 * 这是画面上的位置, 服务器轨迹动画会逐帧改它;
 * 与 BattlePlayer+0x88 (服务器同步点) 是两个独立来源。 */
static int read_sprite_item_pos(pid_t pid, uint64_t sprite, int32_t xy[2])
{
    if (!sprite) return 0;
    uint64_t d = 0;
    double p[2];
    if (pv_read(pid, sprite + 0x18, &d, 8) != 8 || !d) return 0;
    if (pv_read(pid, d + 0x58, p, sizeof p) != (ssize_t)sizeof p) return 0;
    if (!(p[0] > -20000.0 && p[0] < 20000.0 &&
          p[1] > -20000.0 && p[1] < 20000.0)) return 0;
    xy[0] = (int32_t)(p[0] >= 0 ? p[0] + 0.5 : p[0] - 0.5);
    xy[1] = (int32_t)(p[1] >= 0 ? p[1] + 0.5 : p[1] - 0.5);
    return 1;
}

/* Qt5 QString -> 数字文本。风力在 UI 上可能显示成 "3.7" / "3d7"
 * (d 当小数点) / "-3.7", 这里只提取数字与分隔符字符。 */
static int read_qstring_number(pid_t pid, uint64_t qs, char *out, int cap)
{
    uint64_t d = 0;
    int32_t size = 0;
    int64_t offset = 0;
    if (!qs || pv_read(pid, qs, &d, 8) != 8 || !d) return 0;
    if (pv_read(pid, d + 4, &size, 4) != 4 || size <= 0 || size > 128) return 0;
    if (pv_read(pid, d + 0x10, &offset, 8) != 8 || offset < 0 || offset > 0x1000) return 0;
    uint16_t buf[129];
    if (pv_read(pid, d + (uint64_t)offset, buf, (size_t)size * 2) != (ssize_t)size * 2)
        return 0;
    int n = 0, started = 0;
    for (int i = 0; i < size && n < cap - 1; i++) {
        uint16_t c = buf[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == ',' || c == 'd' || c == 'D' ||
            c == '-' || c == '+') {
            out[n++] = (char)c;
            started = 1;
        } else if (started && n > 0) {
            break;
        }
    }
    out[n] = 0;
    return n > 0;
}

/* 把风力文本解析成 wind10 (十分之一单位): "3.7" -> 37, "3" -> 30。
 * 无小数点且绝对值 <= 20 时按"个位是风力"处理。 */
static int parse_wind10(const char *s, int32_t *out)
{
    if (!s || !*s) return 0;
    int neg = 0, n = 0, sep = 0, frac_digits = 0;
    long whole = 0, frac = 0;
    const char *p = s;
    if (*p == '-' || *p == '+') { neg = (*p == '-'); p++; }
    for (; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            n++;
            if (!sep) whole = whole * 10 + (*p - '0');
            else if (frac_digits < 3) { frac = frac * 10 + (*p - '0'); frac_digits++; }
        } else if (*p == '.' || *p == ',' || *p == 'd' || *p == 'D') {
            sep = 1;
        } else {
            break;
        }
    }
    if (!n) return 0;
    long v;
    if (sep) {
        while (frac_digits < 1) { frac *= 10; frac_digits++; }
        v = whole * 10 + frac;
    } else if (whole <= 20 && whole >= -20) {
        v = whole * 10;             /* "3" -> 3.0 */
    } else {
        v = whole;                  /* 无小数点且绝对值 > 20, 直接当作 wind10 原值 (如 "37") */
    }
    *out = (int32_t)(neg ? -v : v);
    return 1;
}

/* 读一个候选 QString, 解析后必须落在风力合理区间才采信 */
static int read_wind_candidate(pid_t pid, uint64_t qs, char *out, int cap, int32_t *val)
{
    int32_t v = 0;
    if (!read_qstring_number(pid, qs, out, cap)) return 0;
    /* 排除 > 20.0 的读数 (例如 53.9), 并限制上限 200 个十分位 */
    if (!parse_wind10(out, &v) || v < 0 || v > 1000) return 0;
    *val = v;
    return 1;
}

static void dbg_scan_numbers(pid_t pid, uint64_t base, size_t len,
                             const char *tag, int depth)
{
    if (!base || len > 0x400) return;
    unsigned char blk[0x400];
    if (pv_read(pid, base, blk, len) != (ssize_t)len) return;
    for (size_t off = 0; off + 8 <= len; off += 4) {
        uint64_t p;
        memcpy(&p, blk + off, 8);
        char txt[64];
        int32_t v = 0;
        if (read_wind_candidate(pid, base + off, txt, sizeof txt, &v))
            fprintf(stderr, "windnum %s+%zx=%s/%d\n", tag, off, txt, v);
        if (depth > 0 && p >= 0x700000000000ULL && p <= 0x7fffffffffffULL) {
            char sub[64];
            snprintf(sub, sizeof sub, "%s+%zx>", tag, off);
            dbg_scan_numbers(pid, p, 0x100, sub, depth - 1);
        }
    }
}


/* 扫描内存里的 UTF-16 文本 (QTextDocument / QGraphicsTextItem 的文本区):
 * BattleWind::setValue() 会把风力写进文档, 值是 int 的十进制文本。
 * 遍历所有 2 字节对齐位置, 要求"数字 + 结尾 0x00"交替出现。
 * v261016: 文本项在 +0xE8 指向 QGraphicsTextItem, 文本在其 QTextDocument 里。 */
static int scan_utf16_wind(pid_t pid, uint64_t base, size_t len,
                           char *out, int cap, int32_t *val)
{
    if (!base || !len) return 0;
    if (len > 0x8000) len = 0x8000;
    unsigned char *blk = malloc(len);
    if (!blk) return 0;
    if (pv_read(pid, base, blk, len) != (ssize_t)len) {
        free(blk);
        return 0;
    }
    for (size_t i = 0; i + 4 < len; i++) {
        size_t j = i;
        char tmp[32];
        int n = 0, digits = 0;
        if (blk[j] == '-' || blk[j] == '+') {
            if (j + 1 >= len || blk[j + 1] != 0) continue;
            tmp[n++] = (char)blk[j++];
        }
        while (j + 1 < len && blk[j + 1] == 0 && n + 1 < (int)sizeof tmp) {
            unsigned char c = blk[j];
            if (c >= '0' && c <= '9') {
                tmp[n++] = (char)c;
                digits = 1;
            } else if (c == '.' || c == ',') {
                tmp[n++] = (char)c;
            } else {
                break;
            }
            j += 2;
        }
        if (!digits || n == 0) continue;
        tmp[n] = 0;
        int32_t v = 0;
        if (parse_wind10(tmp, &v) && v >= 0 && v <= 200) {
            if (out && cap) snprintf(out, (size_t)cap, "%s", tmp);
            *val = v;
            free(blk);
            return 1;
        }
    }
    free(blk);
    return 0;
}


static int scan_wind_pointers(pid_t pid, uint64_t base, size_t len,
                              char *out, int cap, int32_t *val)
{
    if (!base || !len) return 0;
    if (len > 0x1000) len = 0x1000;
    unsigned char *blk = malloc(len);
    if (!blk) return 0;
    if (pv_read(pid, base, blk, len) != (ssize_t)len) {
        free(blk);
        return 0;
    }
    for (size_t off = 0; off + 8 <= len; off += 8) {
        uint64_t p = 0;
        memcpy(&p, blk + off, 8);
        if (p < 0x700000000000ULL || p > 0x7fffffffffffULL) continue;
        if (read_wind_candidate(pid, p, out, cap, val)) {
            free(blk);
            return 1;
        }
        if (scan_utf16_wind(pid, p, 0x400, out, cap, val)) {
            free(blk);
            return 1;
        }
    }
    free(blk);
    return 0;
}


static int read_wind_from_doc(pid_t pid, uint64_t doc,
                              char *out, int cap, int32_t *val)
{
    uint64_t priv = 0, pt = 0;
    if (!doc || pv_read(pid, doc + 8, &priv, 8) != 8 || !priv) return 0;
    if (scan_utf16_wind(pid, priv, 0x400, out, cap, val)) return 1;
    if (pv_read(pid, priv + 0x60, &pt, 8) != 8 || !pt) return 0;
    int64_t off = 0;
    if (pv_read(pid, pt + 0x10, &off, 8) == 8 &&
        off > -0x100000 && off < 0x100000) {
        uint64_t textbase = pt + off;
        if (scan_utf16_wind(pid, textbase, 0x4000, out, cap, val)) return 1;
    }
    if (scan_utf16_wind(pid, pt, 0x2000, out, cap, val)) return 1;
    if (scan_wind_pointers(pid, pt, 0x800, out, cap, val)) return 1;
    return 0;
}

static int read_wind_from_item_ptr(pid_t pid, uint64_t item,
                                   char *out, int cap, int32_t *val)
{
    uint64_t priv = 0, doc = 0;
    if (!item) return 0;
    if (scan_utf16_wind(pid, item, 0x800, out, cap, val)) return 1;
    if (scan_wind_pointers(pid, item, 0x800, out, cap, val)) return 1;
    if (pv_read(pid, item + 0x20, &priv, 8) == 8 && priv) {
        if (scan_utf16_wind(pid, priv, 0x400, out, cap, val)) return 1;
        if (scan_wind_pointers(pid, priv, 0x400, out, cap, val)) return 1;
        if (pv_read(pid, priv, &doc, 8) == 8 && doc) {
            if (read_wind_from_doc(pid, doc, out, cap, val)) return 1;
            if (scan_utf16_wind(pid, doc, 0x8000, out, cap, val)) return 1;
            if (scan_wind_pointers(pid, doc, 0x1000, out, cap, val)) return 1;
        }
    }
    return 0;
}

static int read_wind_from_text_item(pid_t pid, uint64_t obj,
                                    char *out, int cap, int32_t *val)
{
    uint64_t item = 0, priv = 0, doc = 0;
    if (pv_read(pid, obj + 0xE8, &item, 8) != 8 || !item) return 0;
    if (read_wind_from_item_ptr(pid, item, out, cap, val)) return 1;
    if (scan_utf16_wind(pid, item, 0x800, out, cap, val)) return 1;
    if (scan_wind_pointers(pid, item, 0x800, out, cap, val)) return 1;
    if (pv_read(pid, item + 0x20, &priv, 8) == 8 && priv) {
        if (scan_utf16_wind(pid, priv, 0x400, out, cap, val)) return 1;
        if (scan_wind_pointers(pid, priv, 0x400, out, cap, val)) return 1;
        if (pv_read(pid, priv, &doc, 8) == 8 && doc) {
            if (read_wind_from_doc(pid, doc, out, cap, val)) return 1;
            if (scan_utf16_wind(pid, doc, 0x8000, out, cap, val)) return 1;
            if (scan_wind_pointers(pid, doc, 0x1000, out, cap, val)) return 1;
        }
    }
    return 0;
}

/* 在 len 字节窗口里按 4 字节步进找 qword 指针,
 * 逐个当 QString 试着读风力; 命中即返回 1。 */
static int scan_wind_qwords(pid_t pid, uint64_t base, size_t len,
                            char *out, int cap, int32_t *val)
{
    if (!base || len > 0x1000) return 0;
    unsigned char blk[0x1000];
    if (pv_read(pid, base, blk, len) != (ssize_t)len) return 0;
    for (size_t off = 0; off + 8 <= len; off += 4) {
        uint64_t p;
        memcpy(&p, blk + off, 8);
        if (p < 0x700000000000ULL || p > 0x7fffffffffffULL) continue;
        if (read_wind_candidate(pid, base + off, out, cap, val)) return 1;
    }
    return 0;
}

/* BattleWind 结构 (PanelBattle+0x208 -> BattleWind*):
 *   +0xA8  QByteArray —— 风力哈希, 用于反查真实数值 (本版本主通道)
 *   +0xC8  旧版 QString —— setValue() 早期版本写这里, 本版本已迁移
 *   +0xE0  BattleWindArrow —— m11 < 0 表示风朝左 (负风)
 * 输出 wind10 (3.7 -> 37); 读不到时返回哨兵 0x7fffffff。 */
#define WIND_HINT "/data/local/tmp/tnt_memeat.wind"

/* BattleWind::hashWind(int) = MD5(4 字节小端 int), 存在 +0xA8 的 QByteArray 里。
 * 哈希不可逆, 所以预生成整数空间 (-2000..2000) 的 MD5 表来查表还原;
 * v261016 的 QByteArray 数据布局: size@+4, 数据偏移@+0x10。 */
static int read_qbytearray(pid_t pid, uint64_t qb, unsigned char *out, int cap, int *out_len)
{
    uint64_t d = 0;
    if (!qb || pv_read(pid, qb, &d, 8) != 8 || !d) return 0;
    unsigned char head[0x18];
    if (pv_read(pid, d, head, sizeof head) != (ssize_t)sizeof head) return 0;
    int32_t size = 0;
    int64_t off = 0;
    memcpy(&size, head + 4, 4);
    memcpy(&off, head + 0x10, 8);
    if (size <= 0 || size > cap || off < 0 || off > 0x10000) return 0;
    if (pv_read(pid, d + (uint64_t)off, out, (size_t)size) != size) return 0;
    *out_len = size;
    return 1;
}

static int32_t wind_hash_lookup(const unsigned char hash[16], int *ok)
{
    enum { WMIN = -2000, WMAX = 2000, WN = WMAX - WMIN + 1 };
    static unsigned char table[WN][16];
    static int built = 0;
    if (!built) {
        for (int i = 0; i < WN; i++) {
            int32_t w = WMIN + i;
            tnt_md5(&w, 4, table[i]);
        }
        built = 1;
    }
    for (int i = 0; i < WN; i++) {
        if (memcmp(hash, table[i], 16) == 0) {
            *ok = 1;
            return WMIN + i;
        }
    }
    *ok = 0;
    return 0;
}

static int recover_wind_from_hash(pid_t pid, uint64_t obj, int32_t *val)
{
    unsigned char hash[16];
    int n = 0;
    if (!read_qbytearray(pid, obj + OFF_BW_TEXT, hash, sizeof hash, &n) || n != 16)
        return 0;
    int ok = 0;
    int32_t w = wind_hash_lookup(hash, &ok);
    if (!ok) return 0;
    *val = w;
    return 1;
}

static uint64_t wind_hint_load(pid_t pid)
{
    FILE *f = fopen(WIND_HINT, "r");
    if (!f) return 0;
    int saved = 0;
    unsigned long long obj = 0;
    if (fscanf(f, "%d %llx", &saved, &obj) != 2 || saved != (int)pid) obj = 0;
    fclose(f);
    return (uint64_t)obj;
}

static void wind_hint_save(pid_t pid, uint64_t obj)
{
    FILE *f = fopen(WIND_HINT, "w");
    if (!f) return;
    fprintf(f, "%d %llx\n", (int)pid, (unsigned long long)obj);
    fclose(f);
}

/* 找 BattleWind 对象。顺序:
 *   1) 读缓存地址 (上次成功的位置), 校验 vtable == _ZTV10BattleWind+0x10;
 *   2) 缓存失效则全堆扫 vtable 指针。
 * 命中后还要能解出风力, 否则继续扫 (vtable 可能被复用到别的对象)。
 * 返回第一个命中地址 (即使解不出风力), 没有则 0。 */
static uint64_t find_wind_object(pid_t pid, uint64_t base)
{
    uint64_t want = base + 0x1558a38ULL;
    uint64_t cached = wind_hint_load(pid);
    if (cached) {
        uint64_t vt = 0;
        if (pv_read(pid, cached, &vt, 8) == 8 && vt == want) {
            /* 缓存地址可用: vtable 对得上, 且确实能从中还原出风力 */
            int32_t wv = 0;
            if (recover_wind_from_hash(pid, cached, &wv))
                return cached;
        }
    }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned char *buf = malloc(1 << 20);
    if (!buf) { fclose(f); return 0; }

    uint64_t first = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5 || perms[0] != 'r' || perms[1] != 'w') continue;
        unsigned long sz = hi - lo;
        if (sz < 0x1000) continue;
        for (unsigned long p = 0; p + 8 <= sz; p += (1 << 20)) {
            size_t want_bytes = sz - p;
            if (want_bytes > (1 << 20)) want_bytes = 1 << 20;
            if (read_region(pid, lo + p, buf, want_bytes) != 0) break;
            for (size_t i = 0; i + 8 <= want_bytes; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != want) continue;
                uint64_t addr = lo + p + i;
                if (!first) first = addr;
                /* 全堆扫到一个 vtable 命中点; 先记下第一个, 再验证能否还原风力。
                 * 能还原就写缓存并直接返回 (省掉后续 region 的扫描)。 */
                int32_t wv = 0;
                if (recover_wind_from_hash(pid, addr, &wv)) {
                    wind_hint_save(pid, addr);
                    free(buf);
                    fclose(f);
                    return addr;
                }
            }
        }
    }
    free(buf);
    fclose(f);
    /* 扫完都没解出风力: 返回第一个 vtable 命中点,
     * 让上层用文本/箭头等其它通道再试。 */
    return first;
}

/* 读当前风力 (wind10, 即 0.1 单位)。
 * 依次尝试: 哈希反查 -> 文本项 -> 箭头控件 -> 兜底深扫。
 * 参数 panel 可为 0 (进战斗前), 此时直接用模块基址全堆找。 */
static int32_t read_wind10(pid_t pid, uint64_t panel, uint64_t module_base, char *text, int cap)
{
    text[0] = 0;
    uint64_t obj = 0;
    if (panel) {
        uint64_t vt = 0;
        if (pv_read(pid, panel + OFF_PANEL_WIND, &obj, 8) == 8 && obj &&
            pv_read(pid, obj, &vt, 8) == 8 && vt == module_base + 0x1558a38ULL) {
            /* 首选通道: PanelBattle+0x208 指向的对象, vtable 必须是 BattleWind */
        } else {
            obj = 0;
        }
    }
    if (!obj) {
        static time_t last_scan;
        time_t now = time(NULL);
        if (last_scan && now - last_scan < 5) return 0x7fffffff;  /* 全堆扫描很贵, 5 秒内不重复做 */
        last_scan = now;
        obj = find_wind_object(pid, module_base);
    }
    if (!obj) {
        /* 找不到 BattleWind 对象: 报未知, 让上层显示 "--" */
        return 0x7fffffff;
    }

    char mag[64];
    int32_t wind_val = 0;
    /* 通道 1: QByteArray 里的 MD5 哈希反查 (最快最准) */
    int ok = 0;
    if (recover_wind_from_hash(pid, obj, &wind_val)) {
        ok = 1;
        snprintf(mag, sizeof mag, "%s%d.%d", wind_val < 0 ? "-" : "",
                 abs(wind_val) / 10, abs(wind_val) % 10);
    }
    if (!ok) ok = read_wind_from_text_item(pid, obj, mag, sizeof mag, &wind_val);
    if (!ok) {
        uint64_t arrow = 0;
        if (pv_read(pid, obj + OFF_BW_ARROW, &arrow, 8) == 8 && arrow) {
            uint64_t avt = 0;
            pv_read(pid, arrow, &avt, 8);
            if (avt == module_base + 0x1558878ULL) {
                /* 通道 2: 风向箭头控件上挂的多份 QString 副本 */
                ok = read_wind_candidate(pid, arrow + 0x98, mag, sizeof mag, &wind_val);
                if (!ok) {
                    uint64_t item = 0;
                    if (pv_read(pid, arrow + 0xD8, &item, 8) == 8)
                        ok = read_wind_from_item_ptr(pid, item, mag, sizeof mag, &wind_val);
                }
                if (!ok) ok = read_wind_candidate(pid, arrow + 0x80, mag, sizeof mag, &wind_val);
                if (!ok) ok = read_wind_candidate(pid, arrow + 0xC8, mag, sizeof mag, &wind_val);
            }
        }
    }
    if (!ok) {
        /* 通道 3: 风力对象里挂的 QGraphicsSimpleTextItem (item/d_ptr 两级) */
        static const int items[] = {0xd0, 0xd8, 0xe8, 0xf8};
        for (unsigned i = 0; i < 4 && !ok; i++) {
            uint64_t item = 0, dptr = 0;
            pv_read(pid, obj + items[i], &item, 8);
            if (item) pv_read(pid, item + 0x10, &dptr, 8);
            if (dptr) {
                ok = read_wind_candidate(pid, dptr + 0x8, mag, sizeof mag, &wind_val);
                if (!ok) ok = read_wind_candidate(pid, dptr + 0x10, mag, sizeof mag, &wind_val);
                if (!ok) ok = read_wind_candidate(pid, dptr + 0x18, mag, sizeof mag, &wind_val);
            }
        }
    }
    /* 通道 4: 兜底, 深扫对象窗口里所有 qword 指针指向的 QString */
    if (!ok) {
        ok = scan_wind_qwords(pid, obj, 0x200, mag, sizeof mag, &wind_val);
        if (!ok) {
            static const int items[] = {0xe8, 0xf8};
            for (unsigned i = 0; i < 2 && !ok; i++) {
                uint64_t item = 0, dptr = 0;
                pv_read(pid, obj + items[i], &item, 8);
                if (item) {
                    ok = scan_wind_qwords(pid, item, 0x300, mag, sizeof mag, &wind_val);
                    if (!ok) pv_read(pid, item + 0x10, &dptr, 8);
                }
                if (!ok && dptr) ok = scan_wind_qwords(pid, dptr, 0x300, mag, sizeof mag, &wind_val);
            }
        }
    }
    if (getenv("TNT_WIND_DBG")) {
        uint64_t vt = 0, a8 = 0;
        pv_read(pid, obj, &vt, 8);
        pv_read(pid, obj + OFF_BW_TEXT, &a8, 8);
        fprintf(stderr, "winddbg obj=%llx vt=%llx a8=%llx ok=%d mag=%s\n",
                (unsigned long long)obj, (unsigned long long)vt,
                (unsigned long long)a8, ok, ok ? mag : "-");
        if (a8) {
            unsigned char hb[48] = {0};
            pv_read(pid, a8, hb, sizeof hb);
            fprintf(stderr, "winddbg qhead=");
            for (int i = 0; i < 40; i++) fprintf(stderr, "%02x", hb[i]);
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "winddbg fields:");
        for (int fo = 0x80; fo <= 0x118; fo += 8) {
            uint64_t fv = 0;
            pv_read(pid, obj + fo, &fv, 8);
            fprintf(stderr, " %x=%llx", fo, (unsigned long long)fv);
        }
        fprintf(stderr, "\n");
        static const int child_offs[] = {0x90, 0x98, 0xc8, 0xd0, 0xd8, 0x108};
        for (unsigned ci = 0; ci < sizeof child_offs / sizeof child_offs[0]; ci++) {
            uint64_t cp = 0;
            pv_read(pid, obj + child_offs[ci], &cp, 8);
            fprintf(stderr, "winddbg child[%x]=%llx:", child_offs[ci], (unsigned long long)cp);
            for (int j = 0; j < 10 && cp; j++) {
                uint64_t cv = 0;
                if (pv_read(pid, cp + j * 8, &cv, 8) != 8) break;
                fprintf(stderr, " %llx", (unsigned long long)cv);
            }
            fprintf(stderr, "\n");
        }
        dbg_scan_numbers(pid, obj, 0x120, "O", 1);
        static const int qoffs[] = {0xa8, 0xb0, 0xc8, 0xd0};
        for (unsigned k = 0; k < 4; k++) {
            char qtxt[64] = {0};
            int qok = read_qstring_number(pid, obj + qoffs[k], qtxt, sizeof qtxt);
            fprintf(stderr, "winddbg q%x ok=%d txt=%s\n", qoffs[k], qok,
                    qok ? qtxt : "-");
        }
        uint64_t arrow = 0;
        pv_read(pid, obj + OFF_BW_ARROW, &arrow, 8);
        /* 调试输出: 把箭头控件的候选指针逐个打出来 (TNT_WIND_DBG) */
        for (int ti = 0; ti < 3; ti++) {
            static const int ioffs[] = {0xe8, 0xf0, 0xf8};
            uint64_t item = 0, dptr = 0;
            pv_read(pid, obj + ioffs[ti], &item, 8);
            if (item) pv_read(pid, item + 0x10, &dptr, 8);
            uint64_t bases[2] = {item, dptr};
            for (int bi = 0; bi < 2; bi++) {
                if (!bases[bi]) continue;
                unsigned char blk[0x300];
                if (pv_read(pid, bases[bi], blk, sizeof blk) != (ssize_t)sizeof blk) continue;
                for (int off = 0; off + 8 <= (int)sizeof blk; off += 8) {
                    uint64_t p;
                    memcpy(&p, blk + off, 8);
                    if (p < 0x700000000000ULL || p > 0x7fffffffffffULL) continue;
                    int32_t sz = 0; int64_t qo = 0;
                    pv_read(pid, p + 4, &sz, 4);
                    pv_read(pid, p + 0x10, &qo, 8);
                    if (sz < 1 || sz > 16 || qo < 0 || qo > 0x100) continue;
                    uint16_t tb[17] = {0};
                    if (pv_read(pid, p + qo, tb, (size_t)sz * 2) != (ssize_t)sz * 2) continue;
                    char s[32]; int n = 0, numeric = 1;
                    for (int k = 0; k < sz; k++) {
                        uint16_t c = tb[k];
                        if ((c >= '0' && c <= '9') || c == '.' || c == 'd' || c == '-') s[n++] = (char)c;
                        else { numeric = 0; break; }
                    }
                    if (numeric && n > 0) {
                        s[n] = 0;
                        fprintf(stderr, "winddbg num item%x base%d off=%x p=%llx sz=%d txt=%s\n",
                                ioffs[ti], bi, off, (unsigned long long)p, sz, s);
                    }
                }
            }
        }
        static const int titems[] = {0xe8, 0xf0, 0xf8};
        for (unsigned k = 0; k < 3; k++) {
            uint64_t item = 0, dptr = 0, q = 0;
            pv_read(pid, obj + titems[k], &item, 8);
            if (item) pv_read(pid, item + 0x10, &dptr, 8);
            if (dptr) pv_read(pid, dptr + 0x8, &q, 8);
            int32_t sz = 0; int64_t qoff = 0;
            unsigned char tb[32] = {0};
            if (q) {
                pv_read(pid, q + 4, &sz, 4);
                pv_read(pid, q + 0x10, &qoff, 8);
                if (sz >= 0 && sz < 128 && qoff >= 0 && qoff < 0x100)
                    pv_read(pid, q + qoff, tb, sizeof tb);
            }
            fprintf(stderr, "winddbg item+%x item=%llx d=%llx q=%llx sz=%d off=%lld raw=",
                    titems[k], (unsigned long long)item, (unsigned long long)dptr,
                    (unsigned long long)q, sz, (long long)qoff);
            for (int i = 0; i < 24; i++) fprintf(stderr, "%02x", tb[i]);
            fprintf(stderr, "\n");
        }
        uint64_t avt = 0;
        pv_read(pid, arrow, &avt, 8);
        uint64_t aq80 = 0, aq98 = 0;
        if (arrow) {
            pv_read(pid, arrow + 0x80, &aq80, 8);
            pv_read(pid, arrow + 0x98, &aq98, 8);
        }
        fprintf(stderr, "winddbg arrow=%llx avt=%llx aq80=%llx aq98=%llx\n",
                (unsigned long long)arrow, (unsigned long long)avt,
                (unsigned long long)aq80, (unsigned long long)aq98);
        if (arrow) {
            for (int off = 0; off < 0x100; off += 8) {
                uint64_t p = 0;
                pv_read(pid, arrow + off, &p, 8);
                if (p < 0x700000000000ULL || p > 0x7fffffffffffULL) continue;
                int32_t sz = 0; int64_t qoff = 0;
                pv_read(pid, p + 4, &sz, 4);
                pv_read(pid, p + 0x10, &qoff, 8);
                if (sz < 1 || sz > 32 || qoff < 0 || qoff > 0x100) continue;
                unsigned char tb[64] = {0};
                pv_read(pid, p + qoff, tb, sizeof tb);
                fprintf(stderr, "winddbg cand arrow+%x p=%llx sz=%d off=%lld raw=", off,
                        (unsigned long long)p, sz, (long long)qoff);
                for (int i = 0; i < 24; i++) fprintf(stderr, "%02x", tb[i]);
                fprintf(stderr, "\n");
            }
        }
    }
    if (!ok) {
        /* 所有通道都失败: 风力未知 */
        return 0x7fffffff;
    }
    snprintf(text, (size_t)cap, "%s", mag);

    int32_t v = wind_val;
    if (v >= 0) {
        uint64_t arrow = 0;
        double rot = 0.0, m11 = 1.0;
        if (pv_read(pid, obj + OFF_BW_ARROW, &arrow, 8) == 8 && arrow &&
            read_qg_transform(pid, arrow + 0x10, &rot, &m11) == 0) {
            if (m11 < 0.0) v = -v;
        }
    }
    return v;
}

/* BattlePlayer+0xD8 是 Qt5 QString(name); QString 数据布局:
 * (size@+4, data offset@+0x10), UTF-16 编码。
 * 名字里可能有 ':' / ',', 会和 roles= 的分隔符冲突,
 * 所以整体转成十六进制 ASCII 再输出, 由上层解码。 */
static int read_role_name_hex(pid_t pid, uint64_t role, char *out, size_t cap)
{
    if (out && cap) out[0] = 0;
    if (!role || !out || cap < 3) return 0;
    uint64_t d = 0;
    if (pv_read(pid, role + OFF_ROLE_NAME, &d, 8) != 8 || !d) return 0;
    int32_t size = 0;
    int64_t off = 0;
    if (pv_read(pid, d + 4, &size, 4) != 4 ||
        pv_read(pid, d + 0x10, &off, 8) != 8) return 0;
    if (size < 1 || size > 64 || off < 0 || off > 0x100) return 0;
    uint16_t w[64];
    if (pv_read(pid, d + off, w, (size_t)size * 2) != (ssize_t)size * 2) return 0;
    unsigned char u8[256];
    size_t n = 0;
    for (int i = 0; i < size; i++) {
        uint32_t cp = w[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < size &&
            w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (w[++i] - 0xDC00);
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = '?';
        }
        if (cp < 0x80) {
            u8[n++] = (unsigned char)cp;
        } else if (cp < 0x800) {
            u8[n++] = (unsigned char)(0xC0 | (cp >> 6));
            u8[n++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            u8[n++] = (unsigned char)(0xE0 | (cp >> 12));
            u8[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            u8[n++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else {
            u8[n++] = (unsigned char)(0xF0 | (cp >> 18));
            u8[n++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            u8[n++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            u8[n++] = (unsigned char)(0x80 | (cp & 0x3F));
        }
        if (n + 4 >= sizeof u8) break;
    }
    if (!n) return 0;
    size_t p = 0;
    for (size_t i = 0; i < n && p + 3 < cap; i++) {
        static const char hx[] = "0123456789abcdef";
        out[p++] = hx[u8[i] >> 4];
        out[p++] = hx[u8[i] & 15];
    }
    out[p] = 0;
    return p > 0;
}

static int battle_fields(pid_t pid, uint64_t base, uint64_t console, uint64_t panel)
{
    uint64_t vptr = 0;
    if (pv_read(pid, console, &vptr, 8) != 8) {
        printf("vptr_ok=0 console_unreadable=1\n");
        return 2;
    }
    printf("vptr_ok=%d ", vptr == base + VT_CONSOLE + 0x10);

    unsigned char head[0x108];
    if (pv_read(pid, console + 0x20, head, sizeof head) != (ssize_t)sizeof head) {
        printf("console_unreadable=1\n");
        return 2;
    }
    uint8_t canfire = head[OFF_CANFIRE - 0x20];
    uint32_t round;      memcpy(&round, head + OFF_ROUND - 0x20, 4);
    uint64_t angle_obj;  memcpy(&angle_obj, head + OFF_ANGLE_OBJ - 0x20, 8);
    uint64_t self_obj;   memcpy(&self_obj, head + OFF_SELF_OBJ - 0x20, 8);
    uint64_t force_obj;  memcpy(&force_obj, head + OFF_FORCE_OBJ - 0x20, 8);
    uint64_t terrain_obj; memcpy(&terrain_obj, head + OFF_TERRAIN_OBJ - 0x20, 8);
    uint64_t angle_label; memcpy(&angle_label, head + OFF_ANGLE_LABEL - 0x20, 8);
    printf("round=%u canfire=%u ", round, canfire);

    int32_t angle = -999, amax = 0, amin = 0;
    double force = -1;
    if (angle_obj) {
        pv_read(pid, angle_obj + OFF_ANGLE_VAL, &angle, 4);
        pv_read(pid, angle_obj + OFF_ANGLE_VAL + 4, &amax, 4);   /* +0xA0+4: 角度上限 (amax) */
        pv_read(pid, angle_obj + OFF_ANGLE_VAL + 8, &amin, 4);   /* +0xA0+8: 角度下限 (amin) */
        if (amax < 0 || amax > 180) amax = 0;
        if (amin < 0 || amin > 180) amin = 0;
    }
    if (force_obj) pv_read(pid, force_obj + OFF_FORCE_VAL, &force, 8);

    /* 实际发射角 = 武器角度 + 地形角 (带符号)。
     * 地形角来自地形 QGraphicsItem 的 rotation, 符号由 m11 决定;
     * 与 BattleConsole::updateAngle 同一套算法。
     * 盘面显示的就是 actual 角, 所以解算直接用它, 不要再加偏置。 */
    double terrain_rot = 0.0, angle_m11 = 1.0, terrain_m11 = 1.0;
    if (angle_obj) read_qg_transform(pid, angle_obj + 0x10, &terrain_rot, &angle_m11);
    if (terrain_obj) read_qg_transform(pid, terrain_obj + 0x10, &terrain_rot, &terrain_m11);
    double signed_terrain = (angle_m11 < 0.0) ? -terrain_rot : terrain_rot;
    int actual_angle = (int)((double)angle + signed_terrain);
    printf("angle=%d weapon=%d terrain=%.3f actual=%d amax=%d amin=%d force=%.6f facing=%s m11=%.2f ",
           actual_angle, angle, signed_terrain, actual_angle, amax, amin, force,
           angle_m11 < 0.0 ? "left" : "right", angle_m11);

    char wind_txt[64];
    int32_t wind = read_wind10(pid, panel, base, wind_txt, sizeof wind_txt);
    printf("wind=%d wind_txt=%s ", wind, wind_txt[0] ? wind_txt : "-");
    (void)angle_label;

    int32_t self_x = 0x7fffffff, self_y = 0x7fffffff;
    uint32_t self_uid = 0;
    uint64_t self_role = 0, self_team = 0;
    if (self_obj && pv_read(pid, self_obj + OFF_SELF_ROLE, &self_role, 8) == 8 && self_role) {
        int32_t xy[2], item_xy[2];
        if (read_role_live_pos(pid, self_role, xy)) { self_x = xy[0]; self_y = xy[1]; }
        if (read_sprite_item_pos(pid, self_obj, item_xy)) {
            self_x = item_xy[0]; self_y = item_xy[1];
        }
        pv_read(pid, self_role + OFF_ROLE_UID, &self_uid, 4);
        /* BattlePlayer+0xA8: setTeam 写入的 int 队伍号 */
        pv_read(pid, self_role + OFF_ROLE_TEAM, &self_team, 4);
    }
    printf("self=%d,%d self_uid=%u self_team=%llx ", self_x, self_y, self_uid,
           (unsigned long long)self_team);

    /* 当前行动者 (可能是队友): 队友回合时用它的坐标当视角原点;
     * console+0x90 是 setAct 存的 shared_ptr<BattleRole>。 */
    uint64_t act_role;   memcpy(&act_role, head + OFF_ACT_ROLE - 0x20, 8);
    uint8_t myturn = head[OFF_MYTURN - 0x20];
    uint32_t act_uid = 0;
    uint64_t act_team = 0;
    int32_t act_x = 0, act_y = 0;
    if (act_role) {
        int32_t xy[2];
        if (read_role_live_pos(pid, act_role, xy)) { act_x = xy[0]; act_y = xy[1]; }
        pv_read(pid, act_role + OFF_ROLE_UID, &act_uid, 4);
        pv_read(pid, act_role + OFF_ROLE_TEAM, &act_team, 4);
    }
    printf("act_uid=%u act=%d,%d act_team=%llx myturn=%u ", act_uid, act_x, act_y,
           (unsigned long long)act_team, myturn);

    printf("roles=");
    uint64_t hash_d = 0;
    if (panel && pv_read(pid, panel + OFF_PANEL_HASH, &hash_d, 8) == 8 && hash_d) {
        uint64_t buckets = 0;
        uint32_t nbuckets = 0;
        if (pv_read(pid, hash_d + 8, &buckets, 8) == 8 &&
            pv_read(pid, hash_d + 0x20, &nbuckets, 4) == 4 &&
            buckets && nbuckets && nbuckets < (1u << 16)) {
            int count = 0;
            for (uint32_t b = 0; b < nbuckets && count < 32; b++) {
                uint64_t node = 0;
                if (pv_read(pid, buckets + (uint64_t)b * 8, &node, 8) != 8) break;
                int guard = 0;
                while (node && node != hash_d && guard++ < 64) {
                    struct { uint64_t next; uint32_t pad; uint32_t key; uint64_t sprite; } nd;
                    if (pv_read(pid, node, &nd, sizeof nd) != (ssize_t)sizeof nd) break;
                    if (nd.sprite) {
                        uint64_t role = 0;
                        if (pv_read(pid, nd.sprite + OFF_SELF_ROLE, &role, 8) == 8 && role) {
                            int32_t xy[2], item_xy[2];
                            uint32_t ruid = 0;
                            /* role 上的 uid, 与 hash 的 key 必须一致才采信 (防重建期残留节点) */
                            uint64_t rteam = 0;
                            int pos_ok = read_role_live_pos(pid, role, xy);
                            /* 精灵坐标优先 (画面位置), role 坐标留作 role_x/role_y */
                            int32_t role_x = xy[0], role_y = xy[1];
                            if (pos_ok && read_sprite_item_pos(pid, nd.sprite, item_xy)) {
                                xy[0] = item_xy[0];
                                xy[1] = item_xy[1];
                            }
                            if (pos_ok &&
                                pv_read(pid, role + OFF_ROLE_UID, &ruid, 4) == 4 &&
                                ruid == nd.key && ruid > 0 && ruid < 0xFFFFFF00u) {
                                pv_read(pid, role + OFF_ROLE_TEAM, &rteam, 4);
                                if (getenv("TNT_POS_DBG")) {
                                    uint64_t svt = 0, sd = 0;
                                    int32_t livexy[2] = {0, 0};
                                    pv_read(pid, nd.sprite, &svt, 8);
                                    pv_read(pid, nd.sprite + 0x18, &sd, 8);
                                    pv_read(pid, nd.sprite + 0xfc, livexy, sizeof livexy);
                                    double sp[4] = {0, 0, 0, 0};
                                    if (sd) pv_read(pid, sd + 0x10, sp, sizeof sp);
                                    fprintf(stderr, "posdbg uid=%d sprite=%llx svt=%llx off=%llx role=%llx sd=%llx sp=%.2f,%.2f,%.2f,%.2f rolexy=%d,%d live=%d,%d\n",
                                            ruid, (unsigned long long)nd.sprite,
                                            (unsigned long long)svt,
                                            (unsigned long long)(svt - base),
                                            (unsigned long long)role,
                                            (unsigned long long)sd, sp[0], sp[1], sp[2], sp[3],
                                            xy[0], xy[1], livexy[0], livexy[1]);
                                    for (int dj = 0; dj < 0x40 && sd; dj += 8) {
                                        uint64_t qv = 0;
                                        if (pv_read(pid, sd + dj, &qv, 8) != 8) break;
                                        double dv; memcpy(&dv, &qv, 8);
                                        fprintf(stderr, "posdbg2 uid=%d sd+%x q=%llx d=%.2f\n",
                                                ruid, dj, (unsigned long long)qv, dv);
                                    }
                                }
                                if (getenv("TNT_ROLE_DBG")) {
                                    uint64_t rbr = 0, rvt = 0; int32_t oldxy[2] = {0}, brxy[2] = {0};
                                    pv_read(pid, role, &rvt, 8);
                                    pv_read(pid, role + 0xC8, &rbr, 8);
                                    pv_read(pid, role + OFF_ROLE_X, oldxy, 8);
                                    if (rbr >= 0x700000000000ULL && rbr <= 0x7fffffffffffULL)
                                        pv_read(pid, rbr + OFF_ROLE_X, brxy, 8);
                                    fprintf(stderr, "roledbg uid=%d sprite=%llx vt=%llx vt_off=%llx br=%llx old=%d,%d brxy=%d,%d\n",
                                            ruid, (unsigned long long)role,
                                            (unsigned long long)rvt,
                                            (unsigned long long)(rvt - base),
                                            (unsigned long long)rbr, oldxy[0], oldxy[1],
                                            brxy[0], brxy[1]);
                                }
                                char namehex[192];
                                read_role_name_hex(pid, role, namehex, sizeof namehex);
                                /* 血量 -> 阵亡判定: hp < 1 且 maxHp 合理 */
                                int32_t rhp = -1, rhp_max = 0;
                                int dead = 0;
                                if (pv_read(pid, role + OFF_ROLE_HP, &rhp, 4) == 4 &&
                                    pv_read(pid, role + OFF_ROLE_HP_MAX, &rhp_max, 4) == 4) {
                                    if (rhp_max >= 1 && rhp_max <= 1000000 && rhp >= 0) {
                                        dead = (rhp < 1) ? 1 : 0;
                                    }
                                }
                                /* 标志字节: +0x48 = setHide(隐身), +0x4E = setTitan(放大镜变大)。
                                 *
                                 * roles= 输出格式 (逗号分隔条目, 冒号分隔字段):
                                 *   uid:sprite_x:sprite_y:team:name(hex):dead:hide:titan:role_x:role_y:hp:hpmax
                                 *
                                 * 注意第 6 个字段是 dead, 第 7 个才是 hide —— 解析错位会把整轮实验
                                 * 判成"隐身从未生效"。sprite 是画面位置, role 是服务器同步点,
                                 * 对端隐身/飞行期间两者会分叉, 由上层决定采信哪个。 */
                                unsigned char rflags[8] = {0};
                                pv_read(pid, role + 0x48, rflags, sizeof rflags);
                                printf("%s%u:%d:%d:%llx:%s:%d:%d:%d:%d:%d:%d:%d",
                                       count ? "," : "", nd.key, xy[0], xy[1],
                                       (unsigned long long)rteam,
                                       namehex[0] ? namehex : "-", dead,
                                       rflags[0] ? 1 : 0, rflags[6] ? 1 : 0,
                                       role_x, role_y, rhp, rhp_max);
                                count++;
                            }
                        }
                    }
                    node = nd.next;
                }
            }
        }
    }
    printf("\n");
    return 0;
}

static int mode_battle(pid_t pid, uint64_t base, uint64_t console, uint64_t panel)
{
    return battle_fields(pid, base, console, panel);
}

/* 按 int32 逐 4 字节打印一段内存, 用于人肉比对结构体 */
static void dump_ints(pid_t pid, uint64_t addr, int from, int to, const char *tag)
{
    if (!addr) { printf("%s=0\n", tag); return; }
    printf("%s=%llx", tag, (unsigned long long)addr);
    for (int off = from; off <= to; off += 4) {
        int32_t v = 0;
        if (pv_read(pid, addr + off, &v, 4) != 4) { printf(" ?"); continue; }
        printf(" %d", v);
    }
    printf("\n");
}

static int mode_probe(pid_t pid)
{
    uint64_t base = find_module_base(pid);
    if (!base) { printf("err=no_base\n"); return 2; }
    uint64_t console = 0, panel = 0;
    hints_load(pid);
    scan_instances(pid, base, &console, &panel, 0);
    if (!console) { printf("state=idle\n"); return 0; }

    uint64_t angle_obj = 0, self_obj = 0, self_role = 0;
    uint32_t round = 0, self_uid = 0;
    pv_read(pid, console + OFF_ROUND, &round, 4);
    pv_read(pid, console + OFF_ANGLE_OBJ, &angle_obj, 8);
    pv_read(pid, console + OFF_SELF_OBJ, &self_obj, 8);
    if (self_obj) pv_read(pid, self_obj + OFF_SELF_ROLE, &self_role, 8);
    if (self_role) pv_read(pid, self_role + OFF_ROLE_UID, &self_uid, 4);
    printf("round=%u console=%llx angle_obj=%llx self_obj=%llx self_role=%llx self_uid=%u\n",
           round, (unsigned long long)console, (unsigned long long)angle_obj,
           (unsigned long long)self_obj, (unsigned long long)self_role, self_uid);
    /* 转储角度对象的整数视图与浮点视图, 便于定位角度/限位字段 */
    dump_ints(pid, angle_obj, 0x00, 0xC0, "AO");
    if (angle_obj) {
        printf("AOF=%llx", (unsigned long long)angle_obj);
        for (int off = 0x00; off <= 0xC0; off += 4) {
            float f = 0;
            if (pv_read(pid, angle_obj + off, &f, 4) != 4) { printf(" ?"); continue; }
            if (f > -1e6f && f < 1e6f && f == f) printf(" %.4g", f);
            else printf(" -");
        }
        printf("\n");
    }
    /* 自己 role 的整数字段 (+0x60..+0xA0): +0x88/+0x8C 是世界坐标, dir/flip 决定朝向 */
    dump_ints(pid, self_role, 0x60, 0xA0, "SR");
    /* 遍历 PanelBattle 的 uid -> sprite 哈希表, 打印每个角色的坐标 */
    uint64_t hash_d = 0;
    if (panel && pv_read(pid, panel + OFF_PANEL_HASH, &hash_d, 8) == 8 && hash_d) {
        uint64_t buckets = 0; uint32_t nb = 0;
        if (pv_read(pid, hash_d + 8, &buckets, 8) == 8 &&
            pv_read(pid, hash_d + 0x20, &nb, 4) == 4) {
            for (uint32_t b = 0; b < nb; b++) {
                uint64_t node = 0;
                if (pv_read(pid, buckets + (uint64_t)b * 8, &node, 8) != 8) break;
                int g = 0;
                while (node && node != hash_d && g++ < 64) {
                    struct { uint64_t next; uint32_t pad; uint32_t key; uint64_t sprite; } nd;
                    if (pv_read(pid, node, &nd, sizeof nd) != (ssize_t)sizeof nd) break;
                    if (nd.sprite) {
                        uint64_t role = 0;
                        if (pv_read(pid, nd.sprite + OFF_SELF_ROLE, &role, 8) == 8 && role) {
                            int32_t xy[2] = {0, 0};
                            pv_read(pid, role + OFF_ROLE_X, xy, 8);
                            printf("role uid=%u x=%d y=%d role=%llx\n", nd.key, xy[0], xy[1],
                                   (unsigned long long)role);
                        }
                    }
                    node = nd.next;
                }
            }
        }
    }
    return 0;
}

/* 以下为全堆扫描工具, 供 Python 侧通过 adb shell 直接调用 (exec)。 */
#define VT_PANEL_VPTR_OFF 0x156AE70ULL
#define LIB_NAME "libgame_arm64-v8a.so"

static int skip_path(const char *path)
{
    static const char *bad[] = {".so", ".apk", ".jar", ".vdex", ".art", ".oat",
                                ".dex", ".ttf", ".dat", ".oat", NULL};
    for (int i = 0; bad[i]; i++)
        if (strstr(path, bad[i])) return 1;
    return 0;
}

/* 在缓冲区内找 8 字节对齐的 needle; 未对齐命中时右移 1 字节继续,
 * 避免漏掉跨边界的重复值。返回绝对地址, 0 表示没找到。 */
static unsigned long find_word(const unsigned char *buf, size_t len,
                               uint64_t needle, unsigned long base)
{
    const unsigned char *p = buf, *end = buf + len;
    while (p + 8 <= end) {
        const unsigned char *m = memmem(p, (size_t)(end - p), &needle, 8);
        if (!m) return 0;
        unsigned long addr = base + (unsigned long)(m - buf);
        if (!(addr & 7)) return addr;
        p = m + 1;
    }
    return 0;
}

static void scan_for(pid_t pid, unsigned long lo, unsigned long hi,
                     uint64_t nc, uint64_t np,
                     uint64_t *out_c, uint64_t *out_p)
{
    static unsigned char *buf;
    if (!buf && !(buf = malloc(1048576))) return;
    for (unsigned long a = lo; a + 8 <= hi; ) {
        size_t want = (size_t)(hi - a);
        if (want > 1048576) want = 1048576;
        ssize_t n = pv_read(pid, a, buf, want);
        if (n <= 0) { a += 0x1000; continue; }
        if (!*out_c) {
            unsigned long r = find_word(buf, (size_t)n, nc, a);
            if (r) *out_c = r;
        }
        if (!*out_p) {
            unsigned long r = find_word(buf, (size_t)n, np, a);
            if (r) *out_p = r;
        }
        if (*out_c && *out_p) return;
        a += (unsigned long)n;
        if ((size_t)n < want) a = (a + 0xFFF) & ~0xFFFUL;
    }
}

static uint64_t find_module_base(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint64_t base = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5) continue;
        if (off == 0 && perms[0] == 'r' && perms[1] == '-' &&
            n >= 6 && strstr(mp, LIB_NAME)) {
            base = lo;
            break;
        }
    }
    fclose(f);
    return base;
}

/* 实例扫描的地址提示缓存: 记录上次扫到对象的 region,
 * 下次优先扫这些区域, 省掉全堆遍历。最多存 6 段, 满了整体前移一格。
 * 记录里另有 scudo:primary 区的说明: 该区是 libc 的分配器主区,
 * 战斗对象通常落在较小的小块 region 里, 所以按大小给优先级。 */
#define HINT_FILE "/data/local/tmp/tnt_memeat.hints"
static unsigned long g_hint_lo[6], g_hint_hi[6];
static int g_hint_n;

static pid_t g_hint_pid;

static void hints_save(pid_t pid)
{
    FILE *f = fopen(HINT_FILE, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)pid);
    for (int i = 0; i < g_hint_n; i++)
        fprintf(f, "%lx %lx\n", g_hint_lo[i], g_hint_hi[i]);
    fclose(f);
}

static void hints_load(pid_t pid)
{
    g_hint_pid = pid;
    g_hint_n = 0;
    FILE *f = fopen(HINT_FILE, "r");
    if (!f) return;
    int saved_pid = 0;
    if (fscanf(f, "%d", &saved_pid) != 1 || saved_pid != (int)pid) {
        fclose(f);
        return;
    }
    unsigned long lo, hi;
    while (g_hint_n < 6 && fscanf(f, "%lx %lx", &lo, &hi) == 2) {
        g_hint_lo[g_hint_n] = lo;
        g_hint_hi[g_hint_n] = hi;
        g_hint_n++;
    }
    fclose(f);
    g_hint_pid = pid;
}

static void hint_add(unsigned long lo, unsigned long hi)
{
    for (int i = 0; i < g_hint_n; i++)
        if (g_hint_lo[i] == lo && g_hint_hi[i] == hi) return;
    if (g_hint_n >= 6) {          /* 缓存已满: 丢掉最旧的一条, 腾出最后一格 */
        for (int i = 1; i < 6; i++) {
            g_hint_lo[i - 1] = g_hint_lo[i];
            g_hint_hi[i - 1] = g_hint_hi[i];
        }
        g_hint_n = 5;
    }
    g_hint_lo[g_hint_n] = lo;
    g_hint_hi[g_hint_n] = hi;
    g_hint_n++;
    hints_save(g_hint_pid);
}

typedef struct { unsigned long lo, hi; int score; } reg_t;
static reg_t g_regs[4096];

static int reg_cmp(const void *a, const void *b)
{
    const reg_t *x = a, *y = b;
    if (x->score != y->score) return x->score < y->score ? -1 : 1;
    unsigned long sx = x->hi - x->lo, sy = y->hi - y->lo;
    return sx < sy ? -1 : (sx > sy ? 1 : 0);
}

/* 扫描所有可读写匿名 region, 找 vtable 等于
 *   BattleConsole(_ZTV13BattleConsole+0x10) 或
 *   PanelBattle(VT_PANEL_VPTR_OFF+0x10)
 * 的实例。先扫提示缓存; 没命中再全堆扫, 优先扫小块 region
 * (scudo:primary 里大块 region 放的是普通数据, 命中率低)。
 * budget = 本次扫描的字节预算, 0 表示不限。 */
static void scan_instances(pid_t pid, uint64_t base, uint64_t *console,
                           uint64_t *panel, size_t budget)
{
    uint64_t nc = base + VT_CONSOLE + 0x10;
    uint64_t np = base + VT_PANEL_VPTR_OFF + 0x10;
    uint64_t c = *console, p = *panel;

    for (int i = 0; i < g_hint_n && !(c && p); i++)
        scan_for(pid, g_hint_lo[i], g_hint_hi[i], nc, np, &c, &p);
    if (c && p) { *console = c; *panel = p; return; }

    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { *console = c; *panel = p; return; }

    int nreg = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && nreg < 4096) {
        unsigned long lo, hi;
        unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        unsigned long sz = hi - lo;
        if (sz < 0x1000) continue;
        if (n >= 6 && mp[0] && skip_path(mp)) continue;
        int score = 3;
        if (strstr(mp, "scudo:primary"))
            score = (sz <= (4UL << 20)) ? 1 : 2;
        g_regs[nreg].lo = lo;
        g_regs[nreg].hi = hi;
        g_regs[nreg].score = score;
        nreg++;
    }
    fclose(f);
    qsort(g_regs, (size_t)nreg, sizeof(reg_t), reg_cmp);

    size_t used = 0;
    for (int i = 0; i < nreg && !(c && p); i++) {
        if (budget && used >= budget) break;
        uint64_t c0 = c, p0 = p;
        scan_for(pid, g_regs[i].lo, g_regs[i].hi, nc, np, &c, &p);
        used += g_regs[i].hi - g_regs[i].lo;
        if (c != c0 || p != p0) hint_add(g_regs[i].lo, g_regs[i].hi);
    }
    *console = c;
    *panel = p;
}

/* 在整堆里搜一个 qword (通常传 vtable 指针), 按 kind 打印命中处的
 * 结构体摘要:
 *   kind=0 console (回合/角度/力度/uid)
 *   kind=1 panel   (风力对象指针)
 *   kind=2 wind    (风力对象各字段) */
static int mode_findall(pid_t pid, uint64_t needle, int kind)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "no maps\n"); return 2; }
    unsigned char *buf = malloc(1 << 20);
    if (!buf) { fclose(f); return 2; }
    int hits = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5 || perms[0] != 'r' || perms[1] != 'w') continue;
        unsigned long sz = hi - lo;
        if (sz < 0x1000) continue;
        for (unsigned long p = 0; p + 8 <= sz; p += (1 << 20)) {
            size_t want = sz - p;
            if (want > (1 << 20)) want = 1 << 20;
            if (read_region(pid, lo + p, buf, want) != 0) break;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != needle) continue;
                uint64_t addr = lo + p + i;
                if (kind == 0) {
                    uint32_t round = 0, canfire = 0;
                    int32_t angle = -999, amax = 0, amin = 0;
                    uint64_t angle_obj = 0, self_obj = 0, force_obj = 0;
                    double force = -1;
                    pv_read(pid, addr + OFF_ROUND, &round, 4);
                    pv_read(pid, addr + OFF_CANFIRE, &canfire, 1);
                    pv_read(pid, addr + OFF_ANGLE_OBJ, &angle_obj, 8);
                    pv_read(pid, addr + OFF_SELF_OBJ, &self_obj, 8);
                    pv_read(pid, addr + OFF_FORCE_OBJ, &force_obj, 8);
                    if (angle_obj) {
                        pv_read(pid, angle_obj + OFF_ANGLE_VAL, &angle, 4);
                        pv_read(pid, angle_obj + OFF_ANGLE_VAL + 4, &amax, 4);
                        pv_read(pid, angle_obj + OFF_ANGLE_VAL + 8, &amin, 4);
                    }
                    if (force_obj) pv_read(pid, force_obj + OFF_FORCE_VAL, &force, 8);
                    uint64_t self_role = 0; int32_t uid = 0;
                    if (self_obj) {
                        pv_read(pid, self_obj + OFF_SELF_ROLE, &self_role, 8);
                        if (self_role) pv_read(pid, self_role + OFF_ROLE_UID, &uid, 4);
                    }
                    printf("console=%llx round=%u canfire=%u angle=%d amax=%d amin=%d force=%.4f self=%llx uid=%d\n",
                           (unsigned long long)addr, round, canfire, angle, amax, amin,
                           force, (unsigned long long)self_obj, uid);
                } else if (kind == 1) {
                    uint64_t wind = 0;
                    pv_read(pid, addr + OFF_PANEL_WIND, &wind, 8);
                    printf("panel=%llx wind_obj=%llx\n",
                           (unsigned long long)addr, (unsigned long long)wind);
                } else {
                    uint64_t a8 = 0, c8 = 0, d0 = 0, e0 = 0, e8 = 0;
                    pv_read(pid, addr + OFF_BW_TEXT, &a8, 8);
                    pv_read(pid, addr + OFF_BW_TEXT_OLD, &c8, 8);
                    pv_read(pid, addr + 0xd0, &d0, 8);
                    pv_read(pid, addr + OFF_BW_ARROW, &e0, 8);
                    pv_read(pid, addr + OFF_BW_TEXT_ITEM, &e8, 8);
                    char txt[64] = {0};
                    int32_t wv = 0;
                    int ok = read_wind_candidate(pid, addr + 0xc8, txt, sizeof txt, &wv);
                    if (!ok) ok = read_wind_candidate(pid, addr + 0xd0, txt, sizeof txt, &wv);
                    if (!ok) ok = read_wind_candidate(pid, addr + OFF_BW_TEXT, txt, sizeof txt, &wv);
                    if (!ok) ok = read_wind_candidate(pid, addr + 0xb0, txt, sizeof txt, &wv);
                    printf("wind=%llx a8=%llx c8=%llx d0=%llx e0=%llx e8=%llx text=%s ok=%d w=%d\n",
                           (unsigned long long)addr, (unsigned long long)a8,
                           (unsigned long long)c8, (unsigned long long)d0,
                           (unsigned long long)e0, (unsigned long long)e8,
                           ok ? txt : "-", ok, ok ? wv : -1);
                }
                hits++;
            }
        }
    }
    free(buf);
    fclose(f);
    fprintf(stderr, "hits=%d\n", hits);
    return 0;
}

static int mode_findtext(pid_t pid, const char *needle)
{
    size_t nlen = strlen(needle);
    if (!nlen || nlen > 32) return 1;
    unsigned char pat[64];
    for (size_t i = 0; i < nlen; i++) {
        pat[i * 2] = (unsigned char)needle[i];
        pat[i * 2 + 1] = 0;
    }
    nlen *= 2;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { printf("err=no_maps\n"); return 2; }
    static unsigned char *buf;
    if (!buf && !(buf = malloc(1048576))) { fclose(f); return 2; }
    char line[512];
    long hits = 0;
    while (fgets(line, sizeof line, f) && hits < 200) {
        unsigned long lo, hi; unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (skip_path(mp)) continue;
        for (unsigned long a = lo; a < hi && hits < 200; ) {
            size_t want = (size_t)(hi - a);
            if (want > 1048576) want = 1048576;
            ssize_t got = pv_read(pid, a, buf, want);
            if (got <= 0) { a += 0x1000; continue; }
            for (size_t i = 0; i + nlen <= (size_t)got; i++) {
                if (buf[i] != pat[0]) continue;
                if (memcmp(buf + i, pat, nlen) != 0) continue;
                printf("text=%s data=%llx\n", needle,
                       (unsigned long long)(a + i));
                hits++;
            }
            a += (unsigned long)got;
        }
    }
    fclose(f);
    fprintf(stderr, "text_hits=%ld\n", hits);
    return 0;
}

static void sleep_frac(double sec)
{
    if (sec < 0.001) sec = 0.001;
    struct timespec ts;
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)((sec - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static int mode_state(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { printf("err=no_maps\n"); return 2; }

    uint64_t base = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &off, dev, mp);
        if (n < 5) continue;
        if (off == 0 && perms[0] == 'r' && perms[1] == '-' &&
            n >= 6 && strstr(mp, LIB_NAME)) {
            base = lo;
            break;
        }
    }
    fclose(f);
    if (!base) { printf("err=no_base\n"); return 2; }

    uint64_t console = 0, panel = 0;
    hints_load(pid);
    scan_instances(pid, base, &console, &panel, 0);

    if (!console) {
        printf("state=idle base=%llx\n", (unsigned long long)base);
        return 0;
    }
    printf("state=battle base=%llx console=%llx panel=%llx ",
           (unsigned long long)base, (unsigned long long)console,
           (unsigned long long)panel);
    return battle_fields(pid, base, console, panel);
}

/* watch 模式的节流策略: 由 device_reader.py 经 adb 拉起常驻,
 * 未进战斗按 hz_idle 慢跑, 进战斗后按 hz_battle 全速。
 * 下面一节是 Q1 的飞行探针, 与 watch 共用基址扫描逻辑。 */
/* ---- Q1 probe: per-frame sampling of one uid's sprite / role / anim obj ----
 *
 * usage: tnt_memeat fly PID UID [hz] [secs]
 *
 * Prints sprite QGraphicsItem pos, role+0x88 pos, hide/titan bytes, then the
 * diff of the 0x500-byte window at the BattleSprite object plus the diff of the
 * Flying animation object (vtable offset 0x154d7f8) when one is reachable.
 */
#define FLY_VT_OFF 0x154d7f8ULL

static int sprite_for_uid(pid_t pid, uint64_t panel, uint32_t uid,
                          uint64_t *sprite_out, uint64_t *role_out)
{
    uint64_t hash_d = 0;
    if (!panel) return 0;
    if (pv_read(pid, panel + OFF_PANEL_HASH, &hash_d, 8) != 8 || !hash_d) return 0;
    uint64_t buckets = 0;
    uint32_t nbuckets = 0;
    if (pv_read(pid, hash_d + 8, &buckets, 8) != 8 || !buckets) return 0;
    if (pv_read(pid, hash_d + 0x20, &nbuckets, 4) != 4) return 0;
    if (!nbuckets || nbuckets > (1u << 16)) return 0;
    for (uint32_t b = 0; b < nbuckets; b++) {
        uint64_t node = 0;
        if (pv_read(pid, buckets + (uint64_t)b * 8, &node, 8) != 8) break;
        int guard = 0;
        while (node && node != hash_d && guard++ < 64) {
            struct { uint64_t next; uint32_t pad; uint32_t key; uint64_t sprite; } nd;
            if (pv_read(pid, node, &nd, sizeof nd) != (ssize_t)sizeof nd) break;
            if (nd.key == uid && nd.sprite) {
                uint64_t role = 0;
                pv_read(pid, nd.sprite + OFF_SELF_ROLE, &role, 8);
                *sprite_out = nd.sprite;
                *role_out = role;
                return 1;
            }
            node = nd.next;
        }
    }
    return 0;
}

static void fly_decode(int idx, uint64_t q, uint64_t base, uint64_t flyvt)
{
    double d;
    int32_t i0, i1;
    memcpy(&d, &q, 8);
    memcpy(&i0, &q, 4);
    unsigned char tmp[8];
    memcpy(tmp, &q, 8);
    memcpy(&i1, tmp + 4, 4);
    printf(" +%03x q=%llx", idx * 8, (unsigned long long)q);
    if (d > -30000.0 && d < 30000.0 && fabs(d) > 1e-6 && d == d)
        printf(" d=%.3f", d);
    if (i0 > -100000 && i0 < 100000 && i1 > -100000 && i1 < 100000)
        printf(" i=(%d,%d)", i0, i1);
    if (q == flyvt) printf(" <<FLYVT");
    (void)base;
}

static int mode_fly(pid_t pid, uint32_t uid, double hz, double secs)
{
    uint64_t base = find_module_base(pid);
    if (!base) { printf("err=no_base\n"); return 2; }
    uint64_t console = 0, panel = 0, sprite = 0, role = 0;
    hints_load(pid);
    scan_instances(pid, base, &console, &panel, 0);
    if (!console) { printf("err=no_console\n"); return 2; }
    if (!sprite_for_uid(pid, panel, uid, &sprite, &role) || !sprite) {
        printf("err=no_uid uid=%u panel=%llx\n", uid, (unsigned long long)panel);
        return 2;
    }
    const uint64_t flyvt = base + FLY_VT_OFF + 0x10;
    printf("uid=%u sprite=%llx role=%llx base=%llx flyvt=%llx hz=%.1f secs=%.1f\n",
           uid, (unsigned long long)sprite, (unsigned long long)role,
           (unsigned long long)base, (unsigned long long)flyvt, hz, secs);

    enum { NW = 160, NF = 48 };
    static uint64_t prev[NW], cur[NW];
    static uint64_t fprev[NF], fcur[NF];
    int have_prev = 0, have_fprev = 0;
    uint64_t flyobj = 0;
    double t = 0.0;
    double step = (hz > 0.0 && hz < 200.0) ? 1.0 / hz : 1.0 / 30.0;
    while (t < secs) {
        if (read_region(pid, sprite, (unsigned char *)cur, NW * 8) != 0) {
            printf("t=%.2f err=read_sprite\n", t);
            return 2;
        }
        uint64_t d_ptr = 0;
        double dp[2] = {0.0, 0.0};
        if (pv_read(pid, sprite + 0x18, &d_ptr, 8) == 8 && d_ptr)
            pv_read(pid, d_ptr + 0x58, dp, sizeof dp);
        int32_t sp[2], rp[2] = {0, 0};
        sp[0] = (int32_t)(dp[0] >= 0 ? dp[0] + 0.5 : dp[0] - 0.5);
        sp[1] = (int32_t)(dp[1] >= 0 ? dp[1] + 0.5 : dp[1] - 0.5);
        unsigned char flags[8] = {0};
        if (role) {
            pv_read(pid, role + OFF_ROLE_X, rp, 8);
            pv_read(pid, role + 0x48, flags, sizeof flags);
        }
        printf("t=%.2f sprite=%d,%d role=%d,%d hide=%d titan=%d",
               t, sp[0], sp[1], rp[0], rp[1], flags[0] ? 1 : 0,
               flags[6] ? 1 : 0);

        flyobj = 0;
        for (int i = 0; i < NW; i++)
            if (cur[i] == flyvt) {
                /* 注意: 这里取到的是 vtable 常量本身, 不是 Flying 对象的地址。
                 * 后续 read_region(flyobj) 读的是 .so 里的 vtable 表, 不是对象数据。
                 * 正确做法是在 sprite 窗口里找"指向堆对象、且该对象首 8 字节 == flyvt"
                 * 的字段, 或扫 [0x1584fc8] 处的 shared_ptr。 */
                flyobj = sprite + (uint64_t)i * 8;
                printf("|flyoff=%03x", i * 8);
                break;
            }
        if (!have_prev) {
            printf("|base0:");
            for (int i = 0; i < 40; i++) printf(" %llx", (unsigned long long)cur[i]);
        } else {
            int shown = 0;
            for (int i = 0; i < NW; i++) {
                if (cur[i] == prev[i]) continue;
                int32_t a0, a1;
                unsigned char tmp[8];
                memcpy(tmp, &prev[i], 8);
                memcpy(&a0, tmp, 4);
                memcpy(&a1, tmp + 4, 4);
                printf("|chg+%03x %d,%d->", i * 8, a0, a1);
                fly_decode(i, cur[i], base, flyvt);
                if (++shown > 20) { printf("|..."); break; }
            }
        }
        if (flyobj) {
            printf("|fly=%llx", (unsigned long long)flyobj);
            if (read_region(pid, flyobj, (unsigned char *)fcur,
                            sizeof fcur) != 0) {
                printf("|err=read_fly");
            } else if (have_fprev) {
                int shown = 0;
                for (int i = 0; i < NF; i++) {
                    if (fcur[i] == fprev[i]) continue;
                    printf("|flychg");
                    fly_decode(i, fcur[i], base, flyvt);
                    if (++shown > 14) { printf("|..."); break; }
                }
            } else {
                printf("|fly0:");
                for (int i = 0; i < NF; i++) printf(" %llx", (unsigned long long)fcur[i]);
            }
            memcpy(fprev, fcur, sizeof fcur);
            have_fprev = 1;
        }
        printf("\n");
        fflush(stdout);
        memcpy(prev, cur, sizeof cur);
        have_prev = 1;
        sleep_frac(step);
        t += step;
    }
    return 0;
}

static int mode_watch(pid_t pid, double hz_idle, double hz_battle)
{
    uint64_t base = 0, console = 0, panel = 0;
    long tick = 0, idle_tick = 0;
    uint32_t last_round = 0xFFFFFFFFu;
    hints_load(pid);
    for (;;) {
        if (!base) {
            base = find_module_base(pid);
            if (!base) {
                printf("state=no_base\n");
                fflush(stdout);
                sleep_frac(1.0);
                continue;
            }
        }
        if (!console) {
            /* 未进战斗: 每 8 拍做一次全量扫描, 其余拍给一个字节预算 */
            size_t budget = (++idle_tick % 8 == 0) ? 0 : (192UL << 20);
            scan_instances(pid, base, &console, &panel, budget);
            if (!console) {
                printf("state=idle base=%llx\n", (unsigned long long)base);
                fflush(stdout);
                sleep_frac(1.0 / hz_idle);
                continue;
            }
        }
        uint64_t vptr = 0;
        if (pv_read(pid, console, &vptr, 8) != 8 ||
            vptr != base + VT_CONSOLE + 0x10) {
            console = panel = 0;
            continue;
        }
        /* 回合变化时 PanelBattle 会重建, 手里的 panel 指针立刻作废 */
        uint32_t rd = 0;
        if (pv_read(pid, console + OFF_ROUND, &rd, 4) == 4 && rd != last_round) {
            if (last_round != 0xFFFFFFFFu) panel = 0;
            last_round = rd;
        }
        if (panel) {
            uint64_t pp = 0;
            if (pv_read(pid, panel, &pp, 8) != 8 ||
                pp != base + VT_PANEL_VPTR_OFF + 0x10)
                panel = 0;
        }
        if (!panel) {
            /* panel 失效: 重新扫 (给预算, 避免每帧全堆扫) */
            size_t budget = (++tick % 15 == 0) ? 0 : (96UL << 20);
            scan_instances(pid, base, &console, &panel, budget);
        }
        printf("state=battle ");
        battle_fields(pid, base, console, panel);
        fflush(stdout);
        sleep_frac(1.0 / hz_battle);
    }
}

/* anchor: 给定一个 int32 值, 反查它所在的堆对象。
 * 在全堆里找这个值, 再向前回看 back 字节,
 * 找第一个落在模块地址范围内的 qword 当作 vtable; 命中点就是对象起始地址。
 * 输出 HIT field=... obj=... off=... vt_off=... 和对象头 5 个 int。 */
static void mode_anchor(pid_t pid, int32_t needle, unsigned long back)
{
    uint64_t base = find_module_base(pid);
    uint64_t mhi = base + 0x4000000ULL;
    printf("module_base=%llx needle=%d back=0x%lx\n",
           (unsigned long long)base, needle, back);
    if (!base) return;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static unsigned char *buf;
    if (!buf && !(buf = malloc(1048576))) { fclose(f); return; }
    char line[512];
    long printed = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, rhi; unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &rhi, perms, &off, dev, mp);
        if (n < 5) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (skip_path(mp)) continue;
        for (unsigned long a = lo; a < rhi; ) {
            size_t want = (size_t)(rhi - a);
            if (want > 1048576) want = 1048576;
            ssize_t got = pv_read(pid, a, buf, want);
            if (got <= 0) { a += 0x1000; continue; }
            size_t start = back;
            for (size_t i = start; i + 4 <= (size_t)got; i += 4) {
                int32_t v; memcpy(&v, buf + i, 4);
                if (v != needle) continue;
                unsigned long fa = (unsigned long)(a + i);
                unsigned long obj = 0; uint64_t vt = 0;
                for (size_t b = 0; b <= back; b += 4) {
                    if (i < b) break;
                    if (((fa - b) & 7) != 0) continue;
                    uint64_t p; memcpy(&p, buf + i - b, 8);
                    if (p >= base && p < mhi) { obj = fa - b; vt = p; break; }
                }
                if (!obj || printed >= 400) continue;
                printed++;
                printf("HIT field=%lx obj=%lx off=0x%lx vt=%llx vt_off=0x%llx",
                       fa, obj, fa - obj, (unsigned long long)vt,
                       (unsigned long long)(vt - base));
                int32_t ctx[5];
                if (pv_read(pid, obj, ctx, sizeof ctx) == (ssize_t)sizeof ctx)
                    printf(" head=%d,%d,%d,%d,%d", ctx[0], ctx[1], ctx[2], ctx[3], ctx[4]);
                printf("\n");
            }
            a += (unsigned long)got;
        }
    }
    fclose(f);
    printf("done printed=%ld\n", printed);
}

/* xref: 给定一个地址, 找出所有指向它的 8 字节引用。
 * 同样用"向前回看模块内 qword"的方式把引用点归属到某个对象。 */
static void mode_xref(pid_t pid, uint64_t target)
{
    uint64_t base = find_module_base(pid);
    uint64_t mhi = base + 0x4000000ULL;
    printf("module_base=%llx target=%llx\n",
           (unsigned long long)base, (unsigned long long)target);
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static unsigned char *buf;
    if (!buf && !(buf = malloc(1048576))) { fclose(f); return; }
    char line[512];
    long printed = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, rhi; unsigned long long off;
        char perms[8], dev[16], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                       &lo, &rhi, perms, &off, dev, mp);
        if (n < 5) continue;
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (skip_path(mp)) continue;
        for (unsigned long a = lo; a < rhi; ) {
            size_t want = (size_t)(rhi - a);
            if (want > 1048576) want = 1048576;
            ssize_t got = pv_read(pid, a, buf, want);
            if (got <= 0) { a += 0x1000; continue; }
            for (size_t i = 0; i + 8 <= (size_t)got; i += 8) {
                uint64_t v; memcpy(&v, buf + i, 8);
                if (v != target) continue;
                unsigned long fa = (unsigned long)(a + i);
                unsigned long obj = 0; uint64_t vt = 0;
                for (size_t b = 0; b <= 0x200; b += 8) {
                    if (i < b) break;
                    uint64_t p; memcpy(&p, buf + i - b, 8);
                    if (p >= base && p < mhi) { obj = fa - b; vt = p; break; }
                }
                if (!obj || printed >= 200) continue;
                printed++;
                printf("REF at=%lx obj=%lx off=0x%lx vt_off=0x%llx\n",
                       fa, obj, fa - obj, (unsigned long long)(vt - base));
            }
            a += (unsigned long)got;
        }
    }
    fclose(f);
    printf("done refs=%ld\n", printed);
}

/* findpair: 在堆里搜一个 (x, y) 坐标对, 三种编码都试:
 *   int32 对 / float32 对 / double 对(8 字节对齐)。
 * anon_only: 0=只扫 rw, 1=只扫匿名, 2/3=额外限制 region 大小。
 * rep_ms/rep_n: 重复采样次数与间隔, 用来观察同一地址是否稳定命中。
 * tol: 浮点容差 ±tol。 */
static int mode_findpair(pid_t pid, double x, double y, long maxhits, int anon_only,
                         int rep_ms, int rep_n, double tol)
{
    int32_t i32x = (int32_t)llround(x), i32y = (int32_t)llround(y);
    float f32x = (float)x, f32y = (float)y;
    if (tol < 0.01) tol = 0.01;
    float ftol = (float)tol;
    unsigned char *buf = malloc(4u << 20);
    if (!buf) return 2;
    if (rep_n < 1) rep_n = 1;
    for (int it = 0; it < rep_n; it++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        char path[64];
        snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
        FILE *f = fopen(path, "r");
        if (!f) { free(buf); printf("err=no_maps\n"); return 2; }
        const size_t CH = 4u << 20;
        long hits = 0, regions = 0;
        unsigned long scanned = 0;
        char line[512];
        while (fgets(line, sizeof line, f) && hits < maxhits) {
            unsigned long lo, hi;
            unsigned long long off;
            char perms[8], dev[16], mp[256] = {0};
            int n = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                           &lo, &hi, perms, &off, dev, mp);
            if (n < 5 || perms[0] != 'r') continue;
            if (!anon_only && perms[1] != 'w') continue;
            if (anon_only && mp[0] == '/') continue;
            if (strstr(mp, "[vvar]") || strstr(mp, "[vsyscall]") ||
                strstr(mp, "[vectors]")) continue;
            unsigned long sz = hi - lo;
            if (sz < 0x1000 || sz > (1UL << 31)) continue;
            if (anon_only >= 2 && sz > (32UL << 20)) continue;
            if (anon_only >= 3 && sz > (2UL << 20)) continue;
            regions++;
            for (unsigned long p2 = 0; p2 + 8 <= sz && hits < maxhits; p2 += CH) {
                size_t want = sz - p2;
                if (want > CH) want = CH;
                if (read_region(pid, lo + p2, buf, want) != 0) break;
                scanned += (unsigned long)want;
                for (size_t i = 0; i + 8 <= want && hits < maxhits; i += 4) {
                    int32_t a, b;
                    memcpy(&a, buf + i, 4);
                    memcpy(&b, buf + i + 4, 4);
                    if (a == i32x && b == i32y) {
                        printf("#%d i32 %lx\n", it, lo + p2 + i);
                        hits++;
                        continue;
                    }
                    float fa, fb;
                    memcpy(&fa, buf + i, 4);
                    memcpy(&fb, buf + i + 4, 4);
                    if (fabsf(fa - f32x) < ftol && fabsf(fb - f32y) < ftol) {
                        printf("#%d f32 %lx\n", it, lo + p2 + i);
                        hits++;
                        continue;
                    }
                    if (((lo + p2 + i) & 7) == 0 && i + 16 <= want) {
                        double da, db;
                        memcpy(&da, buf + i, 8);
                        memcpy(&db, buf + i + 8, 8);
                        if (fabs(da - x) < tol && fabs(db - y) < tol) {
                            printf("#%d f64 %lx\n", it, lo + p2 + i);
                            hits++;
                        }
                    }
                }
            }
        }
        fclose(f);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                         (t1.tv_nsec - t0.tv_nsec) / 1000000);
        printf("#%d done hits=%ld scanned=%luMB regions=%ld ms=%ld\n",
               it, hits, scanned >> 20, regions, ms);
        fflush(stdout);
        if (it + 1 < rep_n && rep_ms > 0) usleep((useconds_t)rep_ms * 1000);
    }
    free(buf);
    return 0;
}


static int mode_findin(pid_t pid, unsigned long base, unsigned long len,
                       double x, double y, double tol)
{
    if (tol < 0.01) tol = 0.01;
    int32_t i32x = (int32_t)llround(x), i32y = (int32_t)llround(y);
    float f32x = (float)x, f32y = (float)y, ftol = (float)tol;
    const size_t CH = 1u << 20;
    unsigned char *buf = malloc(CH);
    if (!buf) return 2;
    long hits = 0;
    for (unsigned long p2 = 0; p2 < len; p2 += CH) {
        size_t want = len - p2;
        if (want > CH) want = CH;
        if (read_region(pid, base + p2, buf, want) != 0) { printf("err=read@%lx\n", base + p2); break; }
        for (size_t i = 0; i + 8 <= want; i += 4) {
            unsigned long at = base + p2 + i;
            int32_t a, b;
            memcpy(&a, buf + i, 4);
            memcpy(&b, buf + i + 4, 4);
            if (a == i32x && b == i32y) { printf("i32 %lx\n", at); hits++; continue; }
            float fa, fb;
            memcpy(&fa, buf + i, 4);
            memcpy(&fb, buf + i + 4, 4);
            if (fabsf(fa - f32x) < ftol && fabsf(fb - f32y) < ftol) {
                printf("f32 %lx (%g,%g)\n", at, fa, fb);
                hits++;
                continue;
            }
            if ((at & 7) == 0 && i + 16 <= want) {
                double da, db;
                memcpy(&da, buf + i, 8);
                memcpy(&db, buf + i + 8, 8);
                if (fabs(da - x) < tol && fabs(db - y) < tol) {
                    printf("f64 %lx (%g,%g)\n", at, da, db);
                    hits++;
                }
            }
        }
    }
    free(buf);
    printf("done hits=%ld len=%lu\n", hits, len);
    return 0;
}

/* movers: 在给定区域内反复采样, 找出会变化的 float32/float64 坐标对。
 * rounds/ms 控制采样轮数与间隔; minchg 是"变化次数"阈值。
 * 只打印数值落在屏幕坐标系范围内的候选, 减少噪声。 */
static int mode_movers(pid_t pid, unsigned long addr, size_t len, int rounds, int ms, int minchg)
{
    if (rounds < 2) rounds = 2;
    if (rounds > 24) rounds = 24;
    if (minchg < 1) minchg = 1;
    const size_t CH = 1u << 20;
    unsigned char *a = malloc(CH), *b = malloc(CH);
    unsigned char *cnt = calloc(CH / 4 + 16, 1);
    if (!a || !b || !cnt) { free(a); free(b); free(cnt); return 2; }
    long shown = 0;
    for (size_t c = 0; c < len; c += CH) {
        size_t want = len - c;
        if (want > CH) want = CH;
        if (read_region(pid, addr + c, a, want) != 0) { printf("err=read\n"); break; }
        memset(cnt, 0, want / 4 + 8);
        int ok = 1;
        for (int r = 1; r < rounds; r++) {
            if (ms > 0) usleep((useconds_t)ms * 1000);
            if (read_region(pid, addr + c, b, want) != 0) { ok = 0; break; }
            for (size_t i = 0; i + 8 <= want; i += 4)
                if (memcmp(a + i, b + i, 8) != 0) cnt[i / 4]++;
        }
        for (size_t i = 0; ok && i + 16 <= want; i += 4) {
            unsigned c0 = cnt[i / 4], c1 = cnt[i / 4 + 1];
            if (c0 >= minchg && c1 >= minchg) {
                float f0, g0, f1, g1;
                memcpy(&f0, a + i, 4); memcpy(&g0, a + i + 4, 4);
                memcpy(&f1, b + i, 4); memcpy(&g1, b + i + 4, 4);
                if (f0 > 10 && f0 < 3000 && g0 > 10 && g0 < 1600 &&
                    f1 > 10 && f1 < 3000 && g1 > 10 && g1 < 1600) {
                    printf("f32 %lx n=%d (%g,%g)->(%g,%g) d=%.1f\n", addr + c + i, c0,
                           f0, g0, f1, g1,
                           (double)sqrt((f1 - f0) * (f1 - f0) + (g1 - g0) * (g1 - g0)));
                    shown++;
                }
            }
            if ((i & 7) == 0 && i + 24 <= want) {
                unsigned m = cnt[i / 4];
                for (int k = 1; k < 6; k++) if (cnt[i / 4 + k] < m) m = cnt[i / 4 + k];
                if (m >= minchg) {
                    double d0, e0, d1, e1;
                    memcpy(&d0, a + i, 8); memcpy(&e0, a + i + 8, 8);
                    memcpy(&d1, b + i, 8); memcpy(&e1, b + i + 8, 8);
                    if (d0 > 10 && d0 < 3000 && e0 > 10 && e0 < 1600 &&
                        d1 > 10 && d1 < 3000 && e1 > 10 && e1 < 1600) {
                        printf("f64 %lx n=%u (%.2f,%.2f)->(%.2f,%.2f) d=%.1f\n", addr + c + i, m,
                               d0, e0, d1, e1,
                               sqrt((d1 - d0) * (d1 - d0) + (e1 - e0) * (e1 - e0)));
                        shown++;
                    }
                }
            }
        }
    }
    free(a); free(b); free(cnt);
    printf("done moved=%ld rounds=%d minchg=%d\n", shown, rounds, minchg);
    return 0;
}

static int mode_moversall(pid_t pid, int rounds, int ms, size_t maxreg, int minchg)
{
    if (rounds < 2) rounds = 2;
    if (rounds > 12) rounds = 12;
    if (minchg < 2) minchg = rounds - 1;
    const size_t CH = 4u << 20;
    unsigned char *a = malloc(CH), *b = malloc(CH);
    unsigned char *cnt = calloc(CH / 4 + 16, 1);
    if (!a || !b || !cnt) { free(a); free(b); free(cnt); return 2; }
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { printf("err=no_maps\n"); return 2; }
    long shown = 0;
    unsigned long scanned = 0;
    char line[512];
    /* 只扫匿名可读写 region (排除文件映射与 vvar/vsyscall) */
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perms, mp);
        if (n < 3 || perms[0] != 'r' || perms[1] != 'w') continue;
        if (mp[0] == '/') continue;
        if (strstr(mp, "[vvar]") || strstr(mp, "[vsyscall]") || strstr(mp, "[vectors]")) continue;
        unsigned long sz = hi - lo;
        if (sz < 0x1000 || sz > maxreg) continue;
        for (unsigned long c = 0; c < sz; c += CH) {
            size_t want = sz - c;
            if (want > CH) want = CH;
            if (read_region(pid, lo + c, a, want) != 0) break;
            memset(cnt, 0, want / 4 + 8);
            int ok = 1;
            for (int r = 1; r < rounds && ok; r++) {
                usleep((useconds_t)ms * 1000);
                if (read_region(pid, lo + c, b, want) != 0) { ok = 0; break; }
                for (size_t i = 0; i + 8 <= want; i += 4) {
                    if (memcmp(a + i, b + i, 8) != 0) cnt[i / 4]++;
                }
            }
            scanned += (unsigned long)want;
            for (size_t i = 0; ok && i + 8 <= want; i += 4) {
                if (cnt[i / 4] < minchg) continue;
                float f0, g0, f1, g1;
                memcpy(&f0, a + i, 4); memcpy(&g0, a + i + 4, 4);
                memcpy(&f1, b + i, 4); memcpy(&g1, b + i + 4, 4);
                if (!(f0 > 10 && f0 < 3000 && g0 > 10 && g0 < 1600)) continue;
                if (!(f1 > 10 && f1 < 3000 && g1 > 10 && g1 < 1600)) continue;
                if (f0 == 0 && g0 == 0) continue;
                printf("mv %lx n=%d (%g,%g)->(%g,%g) d=%.1f\n", lo + c + i, cnt[i / 4],
                       f0, g0, f1, g1, (double)sqrt((f1 - f0) * (f1 - f0) + (g1 - g0) * (g1 - g0)));
                shown++;
            }
        }
    }
    fclose(f);
    free(a); free(b); free(cnt);
    printf("done moved=%ld scanned=%luMB rounds=%d minchg=%d\n", shown, scanned >> 20, rounds, minchg);
    return 0;
}

/* findq: 整个匿名内存里找指定 8 字节值 (通常是 vtable 指针),
 * 打印命中地址。maxhits > 0 时达到上限就提前停止。 */
static int mode_findq(pid_t pid, uint64_t val, long maxhits)
{
    const size_t CH = 1u << 20;
    unsigned char *buf = malloc(CH);
    if (!buf) return 2;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { printf("err=no_maps\n"); free(buf); return 2; }
    long hits = 0;
    unsigned long scanned = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8], mp[256] = {0};
        int n = sscanf(line, "%lx-%lx %7s %*s %*s %*s %255[^\n]", &lo, &hi, perms, mp);
        if (n < 3 || perms[0] != 'r') continue;
        if (mp[0] == '/') continue;
        for (unsigned long c = lo; c < hi; c += CH) {
            size_t want = hi - c;
            if (want > CH) want = CH;
            if (read_region(pid, c, buf, want) != 0) break;
            scanned += want;
            for (size_t i = 0; i + 8 <= want; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v == val) {
                    if (maxhits > 0 && hits >= maxhits) {
                        fclose(f); free(buf);
                        printf("\nstop hits=%ld\n", hits);
                        return 0;
                    }
                    printf("q %lx\n", c + i);
                    hits++;
                }
            }
        }
    }
    fclose(f);
    free(buf);
    printf("\ndone hits=%ld scanned=%luMB\n", hits, scanned >> 20);
    return 0;
}

/* newpair: 监控一段内存, 逐轮打印"新出现的 int32 坐标对"。
 * 用于抓服务器刚写入的坐标 (例如解除隐身的那一刻)。 */
static int mode_newpair(pid_t pid, unsigned long addr, size_t len, int rounds, int wait_ms)
{
    if (len > (16u << 20)) len = 16u << 20;
    unsigned char *a = malloc(len), *b = malloc(len);
    if (!a || !b) { free(a); free(b); printf("err=oom\n"); return 2; }
    if (read_region(pid, addr, a, len) != 0) { printf("err=read\n"); free(a); free(b); return 2; }
    long shown = 0;
    for (int r = 0; r < rounds; r++) {
        if (wait_ms > 0) usleep((useconds_t)wait_ms * 1000);
        if (read_region(pid, addr, b, len) != 0) { printf("err=read%d\n", r); break; }
        for (size_t i = 0; i + 8 <= len; i += 4) {
            if (memcmp(a + i, b + i, 8) == 0) continue;
            int32_t x, y;
            memcpy(&x, b + i, 4);
            memcpy(&y, b + i + 4, 4);
            if (x <= 0 || x >= 3000 || y <= 0 || y >= 1600) continue;
            int32_t ox, oy; memcpy(&ox, a + i, 4); memcpy(&oy, a + i + 4, 4);
            printf("np r=%d %lx (%d,%d) old=(%d,%d)\n", r, addr + i, x, y, ox, oy);
            shown++;
        }
        memcpy(a, b, len);
    }
    free(a); free(b);
    printf("done np=%ld rounds=%d\n", shown, rounds);
    return 0;
}

static int mode_heapdiff(pid_t pid, unsigned long addr, size_t len, int wait_ms)
{
    unsigned char *a = malloc(len), *b = malloc(len);
    if (!a || !b) { free(a); free(b); return 2; }
    if (read_region(pid, addr, a, len) != 0) { printf("err=read1\n"); free(a); free(b); return 2; }
    usleep((useconds_t)wait_ms * 1000);
    if (read_region(pid, addr, b, len) != 0) { printf("err=read2\n"); free(a); free(b); return 2; }
    long nd = 0, shown = 0;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        uint64_t x, y;
        memcpy(&x, a + i, 8);
        memcpy(&y, b + i, 8);
        if (x == y) continue;
        nd++;
        float f1, g1, f2, g2;
        memcpy(&f1, a + i, 4); memcpy(&g1, a + i + 4, 4);
        memcpy(&f2, b + i, 4); memcpy(&g2, b + i + 4, 4);
        int ok1 = (f1 > -3000 && f1 < 6000 && g1 > -3000 && g1 < 6000);
        int ok2 = (f2 > -3000 && f2 < 6000 && g2 > -3000 && g2 < 6000);
        if (!ok1 || !ok2) continue;
        if (fabsf(f2 - f1) < 2.0f && fabsf(g2 - g1) < 2.0f) continue;
        if (shown >= 300) continue;
        printf("off=%06zx xy=(%g,%g)->(%g,%g)\n", i, f1, g1, f2, g2);
        shown++;
    }
    printf("changed=%ld shown=%ld len=%zu\n", nd, shown, len);
    fflush(stdout);
    free(a); free(b);
    return 0;
}

static uint64_t page_hash(const unsigned char *p, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h ^= v;
        h *= 1099511628211ULL;
    }
    return h;
}

static int mode_hotpages(pid_t pid, int wait_ms, size_t maxreg, int minchg)
{
    size_t cap = 0, n = 0;
    unsigned long *addrs = NULL;
    uint64_t *h1 = NULL;
    unsigned char *buf = malloc(4u << 20);
    if (!buf) return 2;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)pid);

    for (int pass = 0; pass < 2; pass++) {
        FILE *f = fopen(path, "r");
        if (!f) { printf("err=no_maps\n"); return 2; }
        char line[512];
        size_t idx = 0;
        if (pass == 1) usleep((useconds_t)wait_ms * 1000);
        while (fgets(line, sizeof line, f)) {
            unsigned long lo, hi;
            unsigned long long off;
            char perms[8], dev[16], mp[256] = {0};
            int nf = sscanf(line, "%lx-%lx %7s %llx %15s %*s %255[^\n]",
                            &lo, &hi, perms, &off, dev, mp);
            if (nf < 5 || perms[0] != 'r' || perms[1] != 'w') continue;
            if (mp[0] == '/') continue;
            if (strstr(mp, "[vvar]") || strstr(mp, "[vsyscall]") ||
                strstr(mp, "[vectors]")) continue;
            unsigned long sz = hi - lo;
            if (sz < 0x1000 || sz > maxreg) continue;
            for (unsigned long a = lo; a + 0x1000 <= hi; a += 0x1000) {
                if (pass == 0) {
                    if (n == cap) {
                        cap = cap ? cap * 2 : 65536;
                        addrs = realloc(addrs, cap * sizeof(*addrs));
                        h1 = realloc(h1, cap * sizeof(*h1));
                        if (!addrs || !h1) { printf("err=oom\n"); return 2; }
                    }
                    if (read_region(pid, a, buf, 0x1000) != 0) { h1[n] = 0; addrs[n] = a; n++; continue; }
                    addrs[n] = a;
                    h1[n] = page_hash(buf, 0x1000);
                    n++;
                } else {
                    if (idx >= n || addrs[idx] != a) { idx++; continue; }
                    uint64_t hh = 0;
                    if (read_region(pid, a, buf, 0x1000) == 0) hh = page_hash(buf, 0x1000);
                    if (h1[idx] != hh) printf("hot %lx\n", a);
                    idx++;
                }
            }
        }
        fclose(f);
        if (pass == 0) printf("pages=%zu maxreg=%zu\n", n, maxreg);
    }
    free(buf); free(addrs); free(h1);
    return 0;
}

static int mode_fpairs(pid_t pid, unsigned long addr, size_t len)
{
    unsigned char *b = malloc(len);
    if (!b) return 2;
    if (read_region(pid, addr, b, len) != 0) { printf("err=read\n"); free(b); return 2; }
    for (size_t i = 0; i + 8 <= len; i += 4) {
        float f, g;
        memcpy(&f, b + i, 4);
        memcpy(&g, b + i + 4, 4);
        if (!(f > -50 && f < 3200 && g > -50 && g < 1200)) continue;
        if (f == 0 && g == 0) continue;
        printf("%06zx %g %g\n", i, f, g);
    }
    printf("done len=%zu\n", len);
    free(b);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: dump|batch|scan|battle|state|anchor|selftest ...\n");
        return 1;
    }

    if (strcmp(argv[1], "xref") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: tnt_memeat xref PID ADDR_HEX\n");
            return 1;
        }
        mode_xref((pid_t)strtol(argv[2], NULL, 10),
                  strtoull(argv[3], NULL, 16));
        return 0;
    }

    if (strcmp(argv[1], "anchor") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: tnt_memeat anchor PID VALUE [backbytes]\n");
            return 1;
        }
        unsigned long back = (argc >= 5) ? strtoul(argv[4], NULL, 0) : 0x300;
        if (back > 0x2000) back = 0x2000;
        mode_anchor((pid_t)strtol(argv[2], NULL, 10),
                    (int32_t)strtol(argv[3], NULL, 10), back);
        return 0;
    }
    const char *cmd = argv[1];

    if (strcmp(cmd, "selftest") == 0) {
        static unsigned char marker[64] = "TNT_MEMEAT_SELFTEST_MARKER_0123456789abcdef";
        unsigned char out[64];
        if (read_region(getpid(), (unsigned long)marker, out, sizeof out) != 0) return 2;
        if (memcmp(marker, out, sizeof out) != 0) {
            fprintf(stderr, "selftest MISMATCH\n");
            return 2;
        }
        fprintf(stderr, "selftest OK\n");
        return 0;
    }

    if (strcmp(cmd, "state") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: tnt_memeat state PID\n");
            return 1;
        }
        return mode_state((pid_t)strtol(argv[2], NULL, 10));
    }

    if (strcmp(cmd, "findall") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: tnt_memeat findall PID console|panel\n");
            return 1;
        }
        pid_t fp = (pid_t)strtol(argv[2], NULL, 10);
        uint64_t fb = find_module_base(fp);
        if (!fb) { fprintf(stderr, "no_base\n"); return 2; }
        int kind = 0;
        uint64_t vt;
        if (strcmp(argv[3], "console") == 0) {
            kind = 0;
            vt = fb + VT_CONSOLE + 0x10;
        } else if (strcmp(argv[3], "panel") == 0) {
            kind = 1;
            vt = fb + VT_PANEL_VPTR_OFF + 0x10;
        } else if (strcmp(argv[3], "wind") == 0) {
            kind = 2;
            vt = fb + 0x1558a38ULL;
        } else {
            fprintf(stderr, "kind must be console|panel|wind\n");
            return 1;
        }
        return mode_findall(fp, vt, kind);
    }

    if (strcmp(cmd, "watch") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: tnt_memeat watch PID [hz_idle] [hz_battle]\n");
            return 1;
        }
        double hi = (argc >= 4) ? atof(argv[3]) : 2.0;
        double hb = (argc >= 5) ? atof(argv[4]) : 20.0;
        return mode_watch((pid_t)strtol(argv[2], NULL, 10), hi, hb);
    }

    if (strcmp(cmd, "fly") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: tnt_memeat fly PID UID [hz] [secs]\n");
            return 1;
        }
        double hz = (argc >= 5) ? atof(argv[4]) : 30.0;
        double secs = (argc >= 6) ? atof(argv[5]) : 6.0;
        return mode_fly((pid_t)strtol(argv[2], NULL, 10),
                        (uint32_t)strtoul(argv[3], NULL, 10), hz, secs);
    }

    if (strcmp(cmd, "probe") == 0) {
        if (argc < 3) {
            fprintf(stderr, "usage: tnt_memeat probe PID\n");
            return 1;
        }
        return mode_probe((pid_t)strtol(argv[2], NULL, 10));
    }

    if (strcmp(cmd, "findtext") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: tnt_memeat findtext PID TEXT\n");
            return 1;
        }
        return mode_findtext((pid_t)strtol(argv[2], NULL, 10), argv[3]);
    }

    if (argc < 4) return 1;
    pid_t pid = (pid_t)strtol(argv[2], NULL, 10);

    if (strcmp(cmd, "setforce") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat setforce PID HEX_CONSOLE VALUE\n");
            return 1;
        }
        unsigned long console = strtoul(argv[3], NULL, 16);
        double v = strtod(argv[4], NULL);
        unsigned long obj = 0;
        if (read_region(pid, console + OFF_FORCE_OBJ, (unsigned char *)&obj, 8) != 0)
            return 2;
        printf("force_obj=%lx\n", obj);
        if (!obj) return 2;
        if (write_region(pid, obj + OFF_FORCE_VAL, (const unsigned char *)&v, 8) != 0)
            return 2;
        double back = 0;
        read_region(pid, obj + OFF_FORCE_VAL, (unsigned char *)&back, 8);
        printf("force %.4f -> %.4f\n", v, back);
        return 0;
    }

    if (strcmp(cmd, "setangle") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat setangle PID HEX_CONSOLE ACTUAL_ANGLE\n");
            return 1;
        }
        unsigned long console = strtoul(argv[3], NULL, 16);
        int want = (int)strtod(argv[4], NULL);
        unsigned long obj = 0, tobj = 0;
        if (read_region(pid, console + OFF_ANGLE_OBJ, (unsigned char *)&obj, 8) != 0)
            return 2;
        if (!obj) return 2;
        read_region(pid, console + OFF_TERRAIN_OBJ, (unsigned char *)&tobj, 8);
        double terrain_rot = 0.0, m11 = 1.0, dummy = 1.0;
        read_qg_transform(pid, obj + 0x10, &dummy, &m11);
        if (tobj) read_qg_transform(pid, tobj + 0x10, &terrain_rot, &dummy);
        double signed_terrain = (m11 < 0.0) ? -terrain_rot : terrain_rot;
        int32_t weapon = (int32_t)(want - signed_terrain);
        if (write_region(pid, obj + OFF_ANGLE_VAL,
                         (const unsigned char *)&weapon, 4) != 0) return 2;
        int32_t back = 0;
        read_region(pid, obj + OFF_ANGLE_VAL, (unsigned char *)&back, 4);
        printf("angle_obj=%lx terrain=%.3f m11=%.2f weapon %d -> %d (actual %d)\n",
               obj, terrain_rot, m11, weapon, back, (int)(back + signed_terrain));
        return 0;
    }

    if (strcmp(cmd, "poke") == 0) {
        if (argc < 6) {
            fprintf(stderr, "usage: tnt_memeat poke PID HEX_ADDR d|f|i VALUE\n");
            return 1;
        }
        unsigned long addr = strtoul(argv[3], NULL, 16);
        const char *type = argv[4];
        if (strcmp(type, "d") == 0) {
            double v = strtod(argv[5], NULL);
            if (write_region(pid, addr, (const unsigned char *)&v, 8) != 0) return 2;
            printf("wrote double %g @ %lx\n", v, addr);
        } else if (strcmp(type, "f") == 0) {
            float v = (float)strtod(argv[5], NULL);
            if (write_region(pid, addr, (const unsigned char *)&v, 4) != 0) return 2;
            printf("wrote float %g @ %lx\n", v, addr);
        } else if (strcmp(type, "i") == 0) {
            int32_t v = (int32_t)strtol(argv[5], NULL, 10);
            if (write_region(pid, addr, (const unsigned char *)&v, 4) != 0) return 2;
            printf("wrote int %d @ %lx\n", v, addr);
        } else if (strcmp(type, "b") == 0) {
            unsigned char v = (unsigned char)strtol(argv[5], NULL, 10);
            if (write_region(pid, addr, &v, 1) != 0) return 2;
            printf("wrote byte %u @ %lx\n", v, addr);
        } else {
            fprintf(stderr, "bad type: %s\n", type);
            return 1;
        }
        return 0;
    }

    if (strcmp(cmd, "dump") == 0) {
        if (argc < 5) return 1;
        unsigned long addr = strtoul(argv[3], NULL, 16);
        size_t size = strtoul(argv[4], NULL, 10);
        unsigned char *buf = malloc(size);
        if (!buf) return 1;
        if (read_region(pid, addr, buf, size) != 0) { free(buf); return 2; }
        int rc = write_all(buf, size) == 0 ? 0 : 2;
        free(buf);
        return rc;
    }

    if (strcmp(cmd, "batch") == 0) {
        for (int i = 3; i < argc; i++) {
            unsigned long addr = strtoul(argv[i], NULL, 16);
            const char *colon = strchr(argv[i], ':');
            if (!colon) { fprintf(stderr, "bad spec: %s\n", argv[i]); return 1; }
            size_t size = strtoul(colon + 1, NULL, 10);
            unsigned char *buf = malloc(size);
            if (!buf) return 1;
            if (read_region(pid, addr, buf, size) != 0) { free(buf); return 2; }
            if (write_all(buf, size) != 0) { free(buf); return 2; }
            free(buf);
        }
        return 0;
    }

    if (strcmp(cmd, "scan4") == 0) {
        if (argc < 6) return 1;
        unsigned long addr = strtoul(argv[3], NULL, 16);
        size_t size = strtoul(argv[4], NULL, 10);
        int32_t needle = (int32_t)strtol(argv[5], NULL, 10);
        long max_hits = (argc >= 7) ? strtol(argv[6], NULL, 10) : 4096;
        unsigned char *buf = malloc(size);
        if (!buf) return 1;
        if (read_region(pid, addr, buf, size) != 0) { free(buf); return 2; }
        long hits = 0;
        for (size_t i = 0; i + 4 <= size && hits < max_hits; i += 4) {
            int32_t v;
            memcpy(&v, buf + i, 4);
            if (v == needle) {
                printf("%lx\n", addr + i);
                hits++;
            }
        }
        free(buf);
        return 0;
    }

    if (strcmp(cmd, "fpairs") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat fpairs PID HEX_ADDR LEN\n");
            return 1;
        }
        return mode_fpairs(pid, strtoul(argv[3], NULL, 16),
                           (size_t)strtoul(argv[4], NULL, 0));
    }

    if (strcmp(cmd, "hotpages") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat hotpages PID WAIT_MS [MAXREG]\n");
            return 1;
        }
        size_t mr = (argc >= 6) ? (size_t)strtoul(argv[5], NULL, 0) : (2u << 20);
        return mode_hotpages(pid, atoi(argv[3]), mr, 0);
    }

    if (strcmp(cmd, "heapdiff") == 0) {
        if (argc < 6) {
            fprintf(stderr, "usage: tnt_memeat heapdiff PID HEX_ADDR LEN WAIT_MS\n");
            return 1;
        }
        return mode_heapdiff(pid, strtoul(argv[3], NULL, 16),
                             (size_t)strtoul(argv[4], NULL, 0), atoi(argv[5]));
    }

    if (strcmp(cmd, "findpair") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat findpair PID X Y [maxhits] [anon_only]\n");
            return 1;
        }
        long mh = (argc >= 6) ? strtol(argv[5], NULL, 10) : 40;
        int ao = (argc >= 7) ? atoi(argv[6]) : 1;
        int rm = (argc >= 8) ? atoi(argv[7]) : 0;
        int rn = (argc >= 9) ? atoi(argv[8]) : 1;
        double tl = (argc >= 10) ? atof(argv[9]) : 0.01;
        return mode_findpair(pid, atof(argv[3]), atof(argv[4]), mh, ao, rm, rn, tl);
    }

    if (strcmp(cmd, "movers") == 0) {
        if (argc < 7) {
            fprintf(stderr, "usage: tnt_memeat movers PID HEX_ADDR LEN ROUNDS MS [MINCHG]\n");
            return 1;
        }
        return mode_movers(pid, strtoul(argv[3], NULL, 16), strtoul(argv[4], NULL, 0),
                           atoi(argv[5]), atoi(argv[6]), argc >= 8 ? atoi(argv[7]) : 1);
    }

    if (strcmp(cmd, "moversall") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat moversall PID ROUNDS MS [MAXREG] [MINCHG]\n");
            return 1;
        }
        size_t mr = (argc >= 6) ? (size_t)strtoul(argv[5], NULL, 0) : (2u << 20);
        int mc = (argc >= 7) ? atoi(argv[6]) : 0;
        return mode_moversall(pid, atoi(argv[3]), atoi(argv[4]), mr, mc);
    }

    if (strcmp(cmd, "newpair") == 0) {
        if (argc < 7) { fprintf(stderr, "usage: tnt_memeat newpair PID HEX_ADDR LEN ROUNDS WAIT_MS\n"); return 1; }
        return mode_newpair(pid, strtoul(argv[3], NULL, 16), strtoul(argv[4], NULL, 0),
                            atoi(argv[5]), atoi(argv[6]));
    }

    if (strcmp(cmd, "findq") == 0) {
        if (argc < 4) { fprintf(stderr, "usage: tnt_memeat findq PID HEX_VALUE [maxhits]\n"); return 1; }
        return mode_findq(pid, strtoull(argv[3], NULL, 16), argc >= 5 ? atol(argv[4]) : 0);
    }

    if (strcmp(cmd, "findin") == 0) {
        if (argc < 7) {
            fprintf(stderr, "usage: tnt_memeat findin PID HEX_ADDR LEN X Y [tol]\n");
            return 1;
        }
        return mode_findin(pid, strtoul(argv[3], NULL, 16), strtoul(argv[4], NULL, 0),
                           atof(argv[5]), atof(argv[6]),
                           argc >= 8 ? atof(argv[7]) : 0.01);
    }

    if (strcmp(cmd, "dumpq") == 0) {
        if (argc < 5) {
            fprintf(stderr, "usage: tnt_memeat dumpq PID HEX_ADDR COUNT\n");
            return 1;
        }
        unsigned long a = strtoul(argv[3], NULL, 16);
        long n = strtol(argv[4], NULL, 10);
        if (n <= 0 || n > 4096) return 1;
        uint64_t *q = malloc((size_t)n * 8);
        if (!q) return 1;
        if (read_region(pid, a, (unsigned char *)q, (size_t)n * 8) != 0) {
            fprintf(stderr, "err=read %lx\n", a);
            free(q);
            return 2;
        }
        for (long i = 0; i < n; i++) {
            printf("%lx ", a + (unsigned long)i * 8);
            fly_decode((int)i, q[i], 0, 0);
            printf("\n");
        }
        free(q);
        return 0;
    }

    if (strcmp(cmd, "scan") == 0) {
        if (argc < 6) return 1;
        unsigned long addr = strtoul(argv[3], NULL, 16);
        size_t size = strtoul(argv[4], NULL, 10);
        unsigned long long needle = strtoull(argv[5], NULL, 16);
        long max_hits = (argc >= 7) ? strtol(argv[6], NULL, 10) : 64;
        unsigned char pat[8];
        for (int i = 0; i < 8; i++) pat[i] = (unsigned char)((needle >> (8 * i)) & 0xff);
        unsigned char *buf = malloc(size);
        if (!buf) return 1;
        if (read_region(pid, addr, buf, size) != 0) { free(buf); return 2; }
        long hits = 0;
        for (size_t i = 0; i + 8 <= size && hits < max_hits; i += 8) {
            if (memcmp(buf + i, pat, 8) == 0) {
                printf("%lx\n", addr + i);
                hits++;
            }
        }
        free(buf);
        return 0;
    }

    if (strcmp(cmd, "battle") == 0) {
        if (argc != 6) {
            fprintf(stderr, "usage: tnt_memeat battle PID HEX_BASE HEX_CONSOLE HEX_PANEL\n");
            return 1;
        }
        return mode_battle(pid,
                           strtoul(argv[3], NULL, 16),
                           strtoul(argv[4], NULL, 16),
                           strtoul(argv[5], NULL, 16));
    }

    fprintf(stderr, "unknown cmd: %s\n", cmd);
    return 1;
}


