# TNT 弹道辅助

一个针对 **This Is Not TNT / `com.grapefruit.game.tnt`** 私服的弹道解算与悬浮层辅助。
含：从进程内存读战斗状态、弹道物理公式、Android 悬浮层显示。

> **免责声明**：仅用于本地私服的技术研究与逆向学习。在官方服务器或他人服务器上
> 使用第三方辅助通常违反服务条款，风险自负。作者不对任何封号、数据损失或法律
> 后果负责。详见 [LICENSE](LICENSE)。

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
| `private_server/ballistics.py` | 同一套公式的服务端副本（用于对拍校验） | 只要 Python |
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

# 校验三处常量是否一致
python private_server/ballistics.py
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

---

## 目录

```
core/
  aim_solver.py        弹道解算（子弹 + 飞行），纯数学，无依赖
  device_reader.py     adb/root 管理 + tnt_memeat 状态流解析
  assist_server.py     采集 → 解算 → TCP 推送给悬浮层
private_server/
  ballistics.py        同一套公式的服务端副本，用于常量对拍
overlay/
  app/java/...         Java 源码（悬浮层、组包、解算、点选层、adb 客户端）
  app/res, AndroidManifest.xml
  app/assets/tnt_memeat 随 APK 分发的读取器
  build_apk.ps1        无 Gradle 的一键构建脚本
tools/
  tnt_memeat.c         设备端内存读取器（探测/取证子命令）
  tnt_memeat           预编译 arm64 静态二进制
  build_memeat.sh      编译脚本
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

游戏更新后需要重新标定偏移：见 [`docs/layout-v261016.json`](docs/layout-v261016.json)
的 `fields` 段，每个字段都标了 `status`（`verified` / `verified_read_works` /
`REJECTED_not_wind` 等）和验证方式。流程大致是：

1. `tools/elf_symbols.py dump <新的 libgame.so>` 看符号是否还认识；
2. `tools/mem_symtab.py` 从运行时镜像提取真实符号表（壳会伪造磁盘符号名）；
3. `tools/field_drift.py` 对比新旧同名函数的访存偏移，判断结构体是否漂移；
4. 用 `tnt_memeat probe / anchor / xref` 在真机上验证。

---

## 已知限制

- **隐身期间拿不到真实位置**：这是服务端数据边界，不是实现问题（见 FINDINGS 第四章）。
- **放大镜（变大）状态缺真实对局验证**：三条证据都指向"只改受击体积、不改公式"，
  但测试账号拿不到该道具。
- **朝向不可靠**：`state.facing` 由旋转矩阵 m11 符号推出，实测有回合会与实际出膛方向相反。
- **按住充电公式有机械误差**：±0.3~0.5 力度点 ≈ ±6~10 px，是当前主要误差来源。
