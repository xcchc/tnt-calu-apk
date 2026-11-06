# TNT 辅助悬浮层 一键构建(不依赖 Gradle, 只用 Android SDK build-tools)
#
# 前置:
#   - Android SDK build-tools 34.0.0 (含 aapt2 / d8 / zipalign / apksigner)
#   - platforms/android-34/android.jar
#   - JDK 8+ (javac / keytool 在 PATH 里)
#
# 定位 SDK 的顺序: $env:ANDROID_SDK_ROOT -> $env:ANDROID_HOME -> 常见安装路径
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
$app  = Join-Path $root 'app'
$build = Join-Path $root 'build'
$dist  = Join-Path $root 'dist'

$sdk = $env:ANDROID_SDK_ROOT
if (-not $sdk) { $sdk = $env:ANDROID_HOME }
if (-not $sdk) {
    $cand = @(
        (Join-Path $env:LOCALAPPDATA 'Android\Sdk'),
        'C:\Android\Sdk'
    ) | Where-Object { $_ -and (Test-Path $_) }
    if ($cand) { $sdk = $cand[0] }
}
if (-not $sdk) {
    throw "未找到 Android SDK。请设置 ANDROID_SDK_ROOT, 或把 SDK 放到默认位置。"
}

$bt  = Join-Path $sdk 'build-tools\34.0.0'
$jar = Join-Path $sdk 'platforms\android-34\android.jar'

if (-not (Test-Path "$bt\aapt2.exe")) { throw "未找到 aapt2.exe: $bt" }
if (-not (Test-Path $jar))            { throw "未找到 android.jar: $jar" }

$javaHome = (Get-Command java.exe).Source | Split-Path | Split-Path
$javac    = Join-Path $javaHome 'bin\javac.exe'
$keytool  = Join-Path $javaHome 'bin\keytool.exe'

foreach ($d in @($build, "$build\gen", "$build\classes", "$build\dex", $dist)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
Remove-Item -LiteralPath "$build\res.zip", "$build\base.apk", "$build\unsigned.apk", "$build\aligned.apk" -ErrorAction SilentlyContinue

Write-Host '[1/6] aapt2 编译资源...' -ForegroundColor Cyan
& "$bt\aapt2.exe" compile --dir "$app\res" -o "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile 失败" }

Write-Host '[2/6] aapt2 link...' -ForegroundColor Cyan
& "$bt\aapt2.exe" link -o "$build\base.apk" `
    -I $jar `
    --manifest "$app\AndroidManifest.xml" `
    --java "$build\gen" `
    -A "$app\assets" `
    --min-sdk-version 21 --target-sdk-version 28 `
    --auto-add-overlay `
    "$build\res.zip"
if ($LASTEXITCODE -ne 0) { throw "aapt2 link 失败" }

Write-Host '[3/6] javac...' -ForegroundColor Cyan
$sources = @(Get-ChildItem "$app\java" -Recurse -Filter *.java | ForEach-Object { $_.FullName })
$sources += Get-ChildItem "$build\gen" -Recurse -Filter R.java | ForEach-Object { $_.FullName }
& $javac -encoding UTF-8 -source 8 -target 8 `
    -bootclasspath $jar `
    -d "$build\classes" `
    @sources
if ($LASTEXITCODE -ne 0) { throw "javac 失败" }

Write-Host '[4/6] d8...' -ForegroundColor Cyan
$classFiles = Get-ChildItem "$build\classes" -Recurse -Filter *.class | ForEach-Object { $_.FullName }
& "$bt\d8.bat" --release --lib $jar --min-api 21 --output "$build\dex" @classFiles
if ($LASTEXITCODE -ne 0) { throw "d8 失败" }

Write-Host '[5/6] 打包 + zipalign...' -ForegroundColor Cyan
Copy-Item "$build\base.apk" "$build\unsigned.apk" -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open("$build\unsigned.apk", 'Update')
[IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
    $zip, "$build\dex\classes.dex", 'classes.dex', 'Optimal') | Out-Null
$zip.Dispose()
& "$bt\zipalign.exe" -f -p 4 "$build\unsigned.apk" "$build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "zipalign 失败" }

Write-Host '[6/6] 签名...' -ForegroundColor Cyan
$keystore = "$build\debug.keystore"
if (-not (Test-Path $keystore)) {
    & $keytool -genkeypair -keystore $keystore `
        -storepass android -keypass android `
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 `
        -dname 'CN=Android Debug,O=Android,C=US' | Out-Null
}
$outApk = Join-Path $dist 'TNT-Assist-v1.0.apk'
& "$bt\apksigner.bat" sign --ks $keystore `
    --ks-pass pass:android --key-pass pass:android `
    --out $outApk "$build\aligned.apk"
if ($LASTEXITCODE -ne 0) { throw "apksigner 失败" }

& "$bt\apksigner.bat" verify --print-certs $outApk
$size = '{0:N1} KB' -f ((Get-Item $outApk).Length / 1KB)
Write-Host ""
Write-Host "✔ 构建完成: $outApk ($size)" -ForegroundColor Green
