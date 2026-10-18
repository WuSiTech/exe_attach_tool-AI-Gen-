# ============================================================================
# build.ps1 —— 一键构建 WuSi Attacher
#
# 产出：
#   build\stub.exe      躯干模板（不需要你手动运行）
#   WuSi_Attacher.exe   打包器成品，双击即用，改配置不需要重新编译
#
# 编译顺序很重要：stub.exe 必须先编译出来，packer.rc 才能把它嵌进打包器。
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File build.ps1
# ============================================================================

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src  = Join-Path $Root 'src'
$Build = Join-Path $Root 'build'

Write-Host ''
Write-Host '=== WuSi Attacher 构建 ===' -ForegroundColor Cyan

# ---------------------------------------------------------------------------
# 1. 找工具链
# ---------------------------------------------------------------------------
function Find-Tool([string]$name) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}

$GXX = Find-Tool 'g++.exe'
$RC  = Find-Tool 'windres.exe'
$CL  = Find-Tool 'cl.exe'

$useMsvc = $false
if (-not $GXX -or -not $RC) {
    if ($CL) {
        $useMsvc = $true
    } else {
        Write-Host '错误：找不到 C++ 编译器。' -ForegroundColor Red
        Write-Host '  需要以下之一：'
        Write-Host '    - MinGW-w64（g++.exe + windres.exe 在 PATH 里）'
        Write-Host '    - Visual Studio Build Tools（在“x64 Native Tools 命令提示符”里运行本脚本）'
        Write-Host ''
        Write-Host '  推荐 MinGW-w64 的免安装发行版 w64devkit：解压后把 bin 目录加进 PATH 即可。'
        exit 1
    }
}

if ($useMsvc) {
    Write-Host '工具链: MSVC (cl.exe)' -ForegroundColor Green
    $CXX = $CL
} else {
    Write-Host "工具链: MinGW-w64" -ForegroundColor Green
    Write-Host "  g++     : $GXX"
    Write-Host "  windres : $RC"
    & $GXX --version | Select-Object -First 1 | ForEach-Object { Write-Host "  version : $_" }
}

if (-not (Test-Path $Build)) { New-Item -ItemType Directory -Path $Build | Out-Null }

# ---------------------------------------------------------------------------
# 2. 编译躯干模板 stub.exe
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[1/4] 编译躯干模板 stub.exe ...' -ForegroundColor Yellow

$stubExe  = Join-Path $Build 'stub.exe'
$stubRcO  = Join-Path $Build 'stub_rc.o'
$stubRcR  = Join-Path $Build 'stub_rc.res'

Push-Location $Src
try {
    if ($useMsvc) {
        & $CL /nologo /utf-8 /W4 /O2 /EHsc /DUNICODE /D_UNICODE `
              /Fo"$Build\\" /Fe"$stubExe" stub.cpp `
              /link /SUBSYSTEM:WINDOWS
        if ($LASTEXITCODE -ne 0) { throw "编译 stub.cpp 失败 (MSVC)" }
        # MSVC 下用 rc.exe 编清单，再让链接器合并
        & rc /nologo /fo "$stubRcR" stub.rc
        if ($LASTEXITCODE -ne 0) { throw "编译 stub.rc 失败 (rc.exe)" }
        & $CL /nologo "$stubRcR" /link /SUBSYSTEM:WINDOWS /OUT:"$stubExe" /ENTRY:wmainCRTStartup
        if ($LASTEXITCODE -ne 0) { throw "链接 stub 资源失败" }
    } else {
        & $RC -I. -i stub.rc -o "$stubRcO" --input-format=rc --output-format=coff
        if ($LASTEXITCODE -ne 0) { throw "windres 编译 stub.rc 失败" }
        # 躯干模板是生成物的"引导器"，必须能在**没有编译器的机器**上跑。
        # MinGW 默认动态链接 libstdc++-6.dll / libgcc_s_seh-1.dll / libwinpthread-1.dll，
        # 缺任何一个都会弹"缺少库"。所以这里静态链接：把运行库全部塞进 exe。
        # 代价只是产物大几十 KB，换来的是真正的单文件、免依赖。
        & $GXX -std=c++17 -O2 -s -mwindows -DUNICODE -D_UNICODE `
               -static -static-libgcc -static-libstdc++ `
               -o "$stubExe" stub.cpp "$stubRcO"
        if ($LASTEXITCODE -ne 0) { throw "编译链接 stub 失败 (MinGW)" }
    }
} finally {
    Pop-Location
}

if (-not (Test-Path $stubExe)) { throw "stub.exe 没有生成" }
$stubSize = (Get-Item $stubExe).Length
Write-Host ("      完成: stub.exe ({0:N0} 字节)" -f $stubSize) -ForegroundColor Green

# ---------------------------------------------------------------------------
# 3. 编译打包器
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[2/4] 编译打包器 WuSi_Attacher.exe ...' -ForegroundColor Yellow

$packerExe = Join-Path $Root 'WuSi_Attacher.exe'
$packerRcO = Join-Path $Build 'packer_rc.o'
$packerRcR = Join-Path $Build 'packer_rc.res'

# packer.rc 要引用仓库根目录的 icon.ico，先复制到 src 下再让 rc 工具读取
$iconInSrc = Join-Path $Src 'icon.ico'
$iconRoot  = Join-Path $Root 'icon.ico'
$haveIcon  = Test-Path $iconRoot
if ($haveIcon) {
    Copy-Item $iconRoot $iconInSrc -Force
    Write-Host '      已载入 icon.ico 作为打包器图标' -ForegroundColor Green
} else {
    Write-Host '      警告：仓库根目录没有 icon.ico，打包器将不带自定义图标' -ForegroundColor Yellow
}

Push-Location $Src
try {
    if ($useMsvc) {
        & rc /nologo /fo "$packerRcR" packer.rc
        if ($LASTEXITCODE -ne 0) { throw "编译 packer.rc 失败 (rc.exe)" }
        & $CL /nologo /utf-8 /W4 /O2 /EHsc /MT /DUNICODE /D_UNICODE `
              /Fo"$Build\\" packer.cpp progress.cpp `
              /link /SUBSYSTEM:WINDOWS /OUT:"$packerExe" `
              user32.lib comdlg32.lib shell32.lib ole32.lib gdi32.lib comctl32.lib `
              "$packerRcR"
        if ($LASTEXITCODE -ne 0) { throw "编译链接打包器失败 (MSVC)" }
    } else {
        & $RC -I. -i packer.rc -o "$packerRcO" --input-format=rc --output-format=coff
        if ($LASTEXITCODE -ne 0) { throw "windres 编译 packer.rc 失败" }
        & $GXX -std=c++17 -O2 -s -mwindows -municode -DUNICODE -D_UNICODE `
               -static -static-libgcc -static-libstdc++ `
               -o "$packerExe" packer.cpp progress.cpp "$packerRcO" `
               -lcomdlg32 -lshell32 -lole32 -lgdi32 -luser32 -lcomctl32
        if ($LASTEXITCODE -ne 0) { throw "编译链接打包器失败 (MinGW)" }
    }
} finally {
    Pop-Location
    if ($haveIcon) { Remove-Item $iconInSrc -Force -ErrorAction SilentlyContinue }
}

if (-not (Test-Path $packerExe)) { throw "打包器没有生成" }

# ---------------------------------------------------------------------------
# 4. 把躯干模板追加到打包器尾部
#
# 这里刻意不用 Windows 资源段：MinGW 的 windres 与 FindResource 在 RCDATA
# 上表现不可靠（资源枚举能看到，FindResource 返回 1813）。追加 + 尾部标记
# 与生成产物用的是同一套机制，只有一条代码路径要维护。
#
# 布局:  [ 打包器 PE 镜像 ][ 标记 ][ stub.exe 字节 ]
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[3/4] 追加躯干模板到打包器尾部 ...' -ForegroundColor Yellow

$TemplateMagic = 'WuSiPackerTemplate.v1.Slot'
$magicBytes = [System.Text.Encoding]::ASCII.GetBytes($TemplateMagic)
$stubBytes  = [System.IO.File]::ReadAllBytes($stubExe)
$packerBytes = [System.IO.File]::ReadAllBytes($packerExe)

if ($stubBytes.Length -lt 2 -or $stubBytes[0] -ne 0x4D -or $stubBytes[1] -ne 0x5A) {
    throw "stub.exe 不是有效的 PE 镜像"
}

$out = New-Object System.IO.MemoryStream
$out.Write($packerBytes, 0, $packerBytes.Length)
$out.Write($magicBytes, 0, $magicBytes.Length)
$out.Write($stubBytes, 0, $stubBytes.Length)
[System.IO.File]::WriteAllBytes($packerExe, $out.ToArray())
$out.Dispose()

Write-Host ("      已追加 {0:N0} 字节躯干模板（标记 {1} 字节）" -f $stubBytes.Length, $magicBytes.Length) -ForegroundColor Green

# 立即回读校验
$verify = [System.IO.File]::ReadAllBytes($packerExe)
$expected = $packerBytes.Length + $magicBytes.Length + $stubBytes.Length
if ($verify.Length -ne $expected) {
    throw "追加后大小不符: $($verify.Length) != $expected"
}
$magicAt = $verify.Length - $stubBytes.Length - $magicBytes.Length
$readBack = [System.Text.Encoding]::ASCII.GetString($verify, $magicAt, $magicBytes.Length)
if ($readBack -ne $TemplateMagic) {
    throw "追加后标记校验失败: '$readBack'"
}
Write-Host "      标记与模板校验通过" -ForegroundColor Green

# ---------------------------------------------------------------------------
# 5. 收尾
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '[4/4] 校验产物 ...' -ForegroundColor Yellow

Remove-Item (Join-Path $Src 'stub.exe') -Force -ErrorAction SilentlyContinue

$packerSize = (Get-Item $packerExe).Length
Write-Host ("      WuSi_Attacher.exe ({0:N0} 字节)" -f $packerSize) -ForegroundColor Green

Write-Host ''
Write-Host '=== 构建成功 ===' -ForegroundColor Cyan
Write-Host ''
Write-Host "打包器: $packerExe"
Write-Host ''
Write-Host '下一步：双击 WuSi_Attacher.exe，选 A、选 B，点「生成」。' -ForegroundColor White
Write-Host '生成的单文件里已经含 A 和 B，双击即可运行，目标机器不需要编译器。' -ForegroundColor White
Write-Host ''
