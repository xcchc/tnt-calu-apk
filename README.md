# TNT 弹道辅助

一个 **This Is Not TNT**（`com.grapefruit.game.tnt`）的战斗状态读取 + 弹道解算 + Android 悬浮层。

它做三件事：从游戏进程内存读出角度/力度/风/双方坐标，用标定好的物理公式算出命中所需力度，
把结果画在悬浮层上。**不自动瞄准、不自动开火**，只显示数字。

> **免责声明**：仅供本地环境的技术研究与学习。在任何服务器上使用第三方工具都可能
> 违反服务条款，风险自负。作者不对封号、数据损失或法律后果负责。详见 [LICENSE](LICENSE)。

---

## 这套东西做了什么

```
┌─────────────────┐   adb shell    ┌──────────────────┐
│  Android 模拟器  │  ──────────►   │ tnt_memeat (C)   │
│  游戏进程        │  process_vm_   │ 设备端常驻读取器  │
│                 │   readv        └──────────────────┘
└─────────────────┘                        │  k=v 流
                                           ▼
┌──────────────────────────────────────────────────────┐
│ core/device_reader.py   解析状态 → core/aim_solver.py │
│                         弹道解算                      │
└──────────────────────────────────────────────────────┘
       │ TCP 28080 (adb reverse)          │ 或直接在设备内
       ▼                                   ▼
┌──────────────────────────────────────────────────────┐
│ overlay/  Android 悬浮层 APK (com.tnt.assist)         │
│ 显示敌人距离/高差/所需力度/按住秒数/飞行力度           │
└──────────────────────────────────────────────────────┘
```

三块可以独立使用：

| 模块 | 作用 | 需要什么 |
|---|---|---|
| `core/aim_solver.py` | 纯数学：给定角度/距离/高差/风，算需要的力度 | 只要 Python |
| `overlay/` | Android 悬浮层，屏幕上直接显示解算结果 | Android SDK + root 模拟器 |
| `tools/tnt_memeat.c` | 设备端内存读取器 | root 模拟器 + zig/NDK |

**只要你想要弹道公式**，看 [`core/aim_solver.py`](core/aim_solver.py) 和
[`docs/FINDINGS.md`](docs/FINDINGS.md) 就够了，其余都用不上。

---

## 快速开始（只用公式）

```bash
# 已知距离(距)、高差、风，求每个角度的力度
python core/aim_solver.py --dist 12 --height 0 --wind 4.3

# 已知自己和敌人的世界坐标
python core/aim_solver.py --self 148,175 --enemy 320,140 --wind -7.4 --facing right

# 鼠标点选：屏幕上「我脚下」和「目标点」的像素坐标
python core/aim_solver.py --click 700,600 1300,520 --angle 45 --wind 4.3

# 校验 Python 侧与 APK 侧的常量是否一致
python tools/check_consts.py
```

输出示例：

```json
{
 "angle": 45.0,
 "force": 50.7,
 "hold_s": 2.77,
 "fly_force": 44.0,
 "fly_hold_s": 2.40,
 "facing": "right",
 "dist_ju": 11.2,
 "height_ju": 1.5
}
```

---

## 完整流程（内存读取 + 悬浮层）

### 0. 前置

- MuMu 模拟器（或任何可 `adb root` 的 Android 模拟器），装好游戏
- Android SDK：build-tools 34.0.0 + platforms/android-34
- JDK 8+（`javac` / `keytool` 在 PATH 里）
- Python 3.10+
- 编译 C 读取器：`zig` 或 Android NDK

### 1. 编译并部署内存读取器

```bash
cd tools
./build_memeat.sh                    # 或手动: zig cc -target aarch64-linux-musl -O2 -static -o tnt_memeat tnt_memeat.c
adb connect 127.0.0.1:16384
adb -s 127.0.0.1:16384 root
adb -s 127.0.0.1:16384 push tnt_memeat /data/local/tmp/
adb -s 127.0.0.1:16384 shell chmod 755 /data/local/tmp/tnt_memeat
```

验证读取通道：

```bash
adb -s 127.0.0.1:16384 shell /data/local/tmp/tnt_memeat selftest   # -> selftest OK
adb -s 127.0.0.1:16384 shell /data/local/tmp/tnt_memeat state $(adb shell pidof com.grapefruit.game.tnt)
```

`state` 在战斗外返回 `state=idle`，战斗中返回一行 `state=battle ... roles=...`。

### 2. 跑 PC 端主循环

```bash
# 单帧调试
python core/device_reader.py --once

# 持续采集（JSONL 到 stdout）
python core/device_reader.py --loop

# 采集 → 解算 → 推给悬浮层
adb -s 127.0.0.1:16384 reverse tcp:8765 tcp:28080
python core/assist_server.py --push
```

### 3. 构建并安装悬浮层

```bash
# 需要 ANDROID_SDK_ROOT 指向 SDK
powershell -File overlay/build_apk.ps1

adb -s 127.0.0.1:16384 install -r overlay/dist/TNT-Assist-v1.0.apk
```

悬浮层有两种取数方式，会自动切换：

1. **本地模式**：App 内通过 `127.0.0.1:5555` 连本机 adbd，自己跑 `tnt_memeat`
   —— 不需要 PC，但要求设备内 adbd 可提权；
2. **PC 模式**：连 `127.0.0.1:8765`（经 `adb reverse` 转到 PC 的 28080）
   —— 由 `core/assist_server.py` 推 JSON。

本地模式正常时不碰 PC 通道；两种都连不上且持续 3 次失败，面板会显示
「本地模式未启动, PC 也没连上」。

---

## 悬浮层怎么用

打开 App → 授权悬浮窗 → 「开启辅助悬浮层」。进入对战后面板自动出现。

### 面板显示什么

```
回合12  风 顺4.3 120ms
45°→  力 50.7%  正常
敌1 名字A   (1234,567)  距11.2 我低1.5  45° 51.2力
敌2 名字B   (1567,890)  距13.4 同高     45° 62.0力
```

- **第一行**：回合数、风（顺/逆 + 数值）、数据延迟。延迟大说明读不到新帧。
  处于队友视角时，风值前面会多一个「·队友视角」。
- **第二行**：当前盘面角度 + 朝向箭头（→/←）、面板实时力度、体型状态
  （「正常」或「变大」——放大镜变身时距离和高度按 ÷2 计入）。
- **之后每个敌人一行**：名字、世界坐标、水平距离（距）、高差、按当前角度所需力度。

敌人**隐身或飞行**期间坐标会冻结，行尾会标出这是「最后一次看到的坐标」，
不是当前位置（原因见 [FINDINGS 第四章](docs/FINDINGS.md)）。

### 底部控制条

| 按钮 | 作用 |
|---|---|
| 小 / 大 | 缩放面板（0.6× ~ 2.6×），设置会记住 |
| 重置 | 缩放回 1.0× |
| 简 | 简洁模式：敌人列表改成两栏并排，一屏能放下更多人 |
| 我 | 视角切换：默认以自己为原点；队友回合时可切到队友 |
| 点 | 打开全屏点选层，用来打地图上的任意位置 |

面板左上角的圆角手柄可以拖动面板位置，也能拖拽缩放。

### 点选层（「点」按钮）

对局里想打某个具体位置（比如预判落点、打箱子）时用：

1. 点「点」→ 弹出全屏点选层
2. 绿圈是「我脚下」的锚点，先把它拖到自己脚下
3. 点目标位置 → 立刻画出准星和信息框
4. 右上角 × 退出

信息框内容：

```
距离 11.2 距  高差 +1.5 距
角度 45°  力度 50.7%  按住 2.77s
飞过去: 力度 44% (按住 2.40s)
方向 右   屏幕比例 1.98
```

角度和风自动取面板当前值，不用手填。目标在左/右时风向会自动翻转。
点选层是独立的覆盖窗口，在自己的窗口内接管触摸，不会把点击传给游戏。

> 点选层靠 `SCREEN_SCALE`（屏幕 px / 世界单位）换算距离。这个值跟**分辨率**和
> **缩放**有关，当前默认 1.98 是 1920×1080 竞技模式实测值。换分辨率要重新标定，
> 否则距离会算错。

---

## 目录

```
core/
  aim_solver.py        弹道解算（子弹 + 飞行），纯数学，无依赖
  device_reader.py     adb/root 管理 + tnt_memeat 状态流解析
  assist_server.py     采集 → 解算 → TCP 推送给悬浮层
overlay/
  app/java/...         Java 源码（悬浮层、组包、解算、点选层、adb 客户端）
  app/res, AndroidManifest.xml
  app/assets/tnt_memeat 随 APK 分发的读取器
  build_apk.ps1        无 Gradle 的一键构建脚本
tools/
  tnt_memeat.c         设备端内存读取器（探测/取证子命令）
  tnt_memeat           预编译 arm64 静态二进制
  build_memeat.sh      编译脚本
  check_consts.py      校验 Python 与 Java 两侧常量一致
  core_offsets.py      结构体/vtable 偏移常量（单一来源）
  mem.py               以库方式访问设备内存
  calib_*.py           战斗采样器
  elf_symbols.py       ELF 动态符号表提取/对比
  mem_symtab.py        从运行时镜像提取真实符号表
  probe_symtab.py      探测 dynsym 在镜像中的位置
  disasm_at.py         按地址反汇编（镜像偏移即 VA）
  disasm_new.py        按符号名反汇编
  dsm.py               按 ELF 程序头反汇编（磁盘 .so）
  find_callers.py      找 bl 调用点
  vtable_map.py        vtable 槽位 → 符号名
  field_drift.py       新旧版本字段偏移漂移对比
  imglib.py            镜像/符号表路径解析
docs/
  FINDINGS.md          技术结论 + 每条结论的验证等级
  layout-v261016.json  符号表与结构体偏移（附验证状态）
```

---

## 关键概念

**两个坐标来源**

| 来源 | 含义 | 何时可信 |
|---|---|---|
| `sprite` | 画面上的 QGraphicsItem 位置，由服务器轨迹动画驱动 | 可见时精确（实测 0 px） |
| `role` | `BattlePlayer+0x88` 服务器同步点 | 可见时与 sprite 一致；飞行/隐身时冻结 |

对端**隐身或飞行**期间，服务端不下发位置包，两个来源都会冻结。悬浮层用
`pos_stale` / `stale_ms` 明确区分「精确」和「最后一次看到的位置」。

**角度**：盘面显示的是 `actual` = `weapon` + `terrain`，解算直接用 `actual`，
不要再加任何偏置。反抛（>90°）时水平射向翻转。

**风**：`wind_action` 正 = 顺射向。风值存在 `BattleWind+0xA8` 的 QByteArray 里，
内容是 `MD5(int32 风力×10)` —— 通过预建整数空间的哈希表反查。

细节和证据等级见 [`docs/FINDINGS.md`](docs/FINDINGS.md)。

---

## 支持的游戏版本

当前偏移对应 **v261016.1**（`lib/arm64-v8a/libgame_arm64-v8a.so`）。

版本不对时不会显示错数据：`tnt_memeat` 每帧校验 console 对象的 vtable，
对不上就报 `state=idle`，悬浮层显示「读取失败」或一直「等待进入对战」。
**所以换版本后需要重新标定，不能直接沿用。**

### 偏移都在哪

| 位置 | 内容 |
|---|---|
| `tools/core_offsets.py` | vtable 偏移、单例地址、包名、设备路径（代码里的唯一来源） |
| `docs/layout-v261016.json` | 符号表 + 结构体字段偏移，每项带 `status` 和验证证据 |

### 换版本的操作顺序

1. **先看符号还认不认识**

   ```bash
   python tools/elf_symbols.py dump <新版本>/lib/arm64-v8a/libgame_arm64-v8a.so new_syms.json
   python tools/elf_symbols.py diff <旧>.so <新>.so --filter Battle
   ```

   壳会把磁盘 `.so` 的符号名伪造成随机串，`elf_symbols.py` 走 `PT_DYNAMIC`
   拿真实符号表，不信节表。

2. **从运行时镜像提取符号表**（磁盘符号不可用时）

   ```bash
   # 需要一个"从 module_base 起的连续 VA dump"
   TNT_IMG=dumps/tnt_img.bin python tools/mem_symtab.py parse dumps/tnt_img.bin --out dumps/syms.json
   ```

3. **判断结构体字段有没有漂移**

   ```bash
   python tools/field_drift.py <旧_img> <新_img> --old-syms <旧>.json --new-syms <新>.json
   ```

   它抽取同名函数里的 `[Xn, #imm]` 访存偏移序列做对比。序列一致说明字段偏移没变，
   只需要更新 vtable/符号地址。

4. **在真机上逐个验证字段**

   ```bash
   # 找 console/panel 实例
   adb shell /data/local/tmp/tnt_memeat findall <pid> console
   # 反查某个值属于哪个对象
   adb shell /data/local/tmp/tnt_memeat anchor <pid> <int32 值>
   # 找谁引用了某个地址
   adb shell /data/local/tmp/tnt_memeat xref <pid> <hex 地址>
   # 打一帧完整状态
   adb shell /data/local/tmp/tnt_memeat state <pid>
   ```

   改完 `core_offsets.py` 和 `tnt_memeat.c` 里的 `OFF_*` 宏后重编译，用 `state`
   对着游戏 UI 逐项核对（角度、回合、力度、风、坐标）。

5. **同步 Java 侧常量**

   物理常量（K/G/V0K 等）如果有变，`core/aim_solver.py` 和
   `overlay/.../Solver.java` 必须一起改：

   ```bash
   python tools/check_consts.py    # 不一致时退出码 1
   ```

6. **重编译 APK**

   ```bash
   powershell -File overlay/build_apk.ps1
   ```

   构建脚本会把 `overlay/app/assets/tnt_memeat` 打进 APK，所以换版本前先
   `cp tools/tnt_memeat overlay/app/assets/tnt_memeat` 更新它。

---

## 已知限制

- **隐身期间拿不到真实位置**：这是服务端数据边界，不是实现问题（见 FINDINGS 第四章）。
- **放大镜（变大）状态缺真实对局验证**：三条证据都指向"只改受击体积、不改公式"，
  但测试账号拿不到该道具。
- **朝向不可靠**：`state.facing` 由旋转矩阵 m11 符号推出，实测有回合会与实际出膛方向相反。
- **按住充电公式有机械误差**：±0.3~0.5 力度点 ≈ ±6~10 px，是当前主要误差来源。
