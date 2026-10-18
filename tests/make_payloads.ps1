# ============================================================================
# tests\make_payloads.ps1
# 编译两个测试用程序，用来端到端验证
# 「释放 B -> 跑 B -> 合并注册表环境 -> 跑 A」。
# 仅用于开发验证，不属于交付物。
# ============================================================================

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$T    = Join-Path $Root 'tests'
$Out  = Join-Path $T 'out'
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Path $Out | Out-Null }

$SrcB = Join-Path $T 'test_b.cpp'
$SrcA = Join-Path $T 'test_a.cpp'
$ExeB = Join-Path $Out 'B.exe'
$ExeA = Join-Path $Out 'A.exe'

Write-Host '编译测试 B.exe ...'
& g++ -std=c++17 -O1 -o $ExeB $SrcB -static
if ($LASTEXITCODE -ne 0) { throw 'test_b 编译失败' }

Write-Host '编译测试 A.exe ...'
& g++ -std=c++17 -O1 -o $ExeA $SrcA -static
if ($LASTEXITCODE -ne 0) { throw 'test_a 编译失败' }

Write-Host ("B.exe: {0:N0} 字节" -f (Get-Item $ExeB).Length)
Write-Host ("A.exe: {0:N0} 字节" -f (Get-Item $ExeA).Length)
