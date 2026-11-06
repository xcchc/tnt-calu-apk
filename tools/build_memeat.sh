# 构建 tools/tnt_memeat (arm64 静态二进制)
#
# 推荐用 zig (单文件工具链, 自带 musl 交叉编译):
#     zig cc -target aarch64-linux-musl -O2 -static -o tnt_memeat tnt_memeat.c
#
# 也可以用 Android NDK 的 clang:
#     $NDK/toolchains/llvm/prebuilt/<host>/bin/aarch64-linux-android21-clang \
#         -O2 -static -o tnt_memeat tnt_memeat.c
#
# 产物放到本目录, device_reader.py 会自动推送并在尺寸变化时更新。
set -e
cd "$(dirname "$0")"

if command -v zig >/dev/null 2>&1; then
  echo "==> 用 zig 构建"
  zig cc -target aarch64-linux-musl -O2 -static -o tnt_memeat tnt_memeat.c
elif [ -n "${NDK:-}" ] && [ -d "$NDK/toolchains/llvm/prebuilt" ]; then
  echo "==> 用 NDK 构建"
  CC=$(find "$NDK/toolchains/llvm/prebuilt" -name 'aarch64-linux-android21-clang' | head -1)
  [ -n "$CC" ] || { echo "找不到 aarch64-linux-android21-clang"; exit 1; }
  "$CC" -O2 -static -o tnt_memeat tnt_memeat.c
else
  echo "需要 zig 或 \$NDK 指向 Android NDK" >&2
  exit 1
fi

file tnt_memeat 2>/dev/null || true
echo "==> 完成: $(ls -lh tnt_memeat | awk '{print $5}')"
echo "   部署: adb push tnt_memeat /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/tnt_memeat"
