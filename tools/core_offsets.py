"""分析目标版本的结构体偏移常量(单一来源)。

这些偏移都是"模块内相对偏移", 运行时地址 = module_base + 偏移。
完整说明见 docs/layout-v261016.json; 改版本时只需要改这一个文件。
"""

# ---- vtable (指向"对象起始处存的 vptr" = vtable + 0x10) ----
VT_CONSOLE = 0x156E188      # _ZTV13BattleConsole
VT_PANEL = 0x156AE70        # _ZTV11PanelBattle
VT_BATTLE_WIND = 0x1558A38  # _ZTV10BattleWind
VT_WIND_ARROW = 0x1558878   # _ZTV16BattleWindArrow
VT_FLYING = 0x154D7F8       # Flying 动画对象 vtable (Q1 探针用)

# ---- 单例与全局 ----
SINST_CONFIG = 0x1598F68    # Singleton<Config>::s_inst

# ---- 设备侧路径 ----
MEMEAT = "/data/local/tmp/tnt_memeat"
PKG = "com.grapefruit.game.tnt"
LIB_NAME = "libgame_arm64-v8a.so"
