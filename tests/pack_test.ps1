# ============================================================================
# tests\pack_test.ps1
# 用命令行复现打包器的拼接逻辑，产出测试用单文件 exe。
# 仅用于自动化验证 —— 正式使用请直接双击 WuSi_Attacher.exe 走 GUI。
#
# 布局必须与 src\shared_config.h 一致：
#   [躯干镜像][B][A][StubTrailer 104 字节]
# ============================================================================

$ErrorActionPreference = 'Stop'

$Root   = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Stub   = Join-Path $Root 'build\stub.exe'
$B      = Join-Path $Root 'tests\out\B.exe'
$A      = Join-Path $Root 'tests\out\A.exe'
$Output = Join-Path $Root 'tests\out\Packed_Test.exe'

# 必须与 shared_config.h 保持一致
$TRAILER_MAGIC = 'WuSiStubTrailer.v1.9f3a1c7d'
$VERSION       = 1
# CLEANUP_AFTER(0x04) | ENABLE_LOG(0x08) | CREATE_NO_WINDOW(0x02)，不开 ADMIN
$FLAGS         = 0x04 -bor 0x08 -bor 0x02
$TIMEOUT       = 60000

$CFG_SIZE     = 8 + (8 * 4) + 4 + (4 * 7)      # = 72，必须等于 sizeof(StubConfig)
$TRAILER_SIZE = $CFG_SIZE + 32                 # = 104，必须等于 sizeof(StubTrailer)
$MAGIC_LEN    = $TRAILER_MAGIC.Length

foreach ($f in @($Stub, $A, $B)) {
    if (-not (Test-Path $f)) { throw "缺少文件: $f" }
}
if ($MAGIC_LEN -gt 32) { throw "trailerMagic 超过 32 字节" }

$stubBytes = [System.IO.File]::ReadAllBytes($Stub)
$aBytes    = [System.IO.File]::ReadAllBytes($A)
$bBytes    = [System.IO.File]::ReadAllBytes($B)

# 配置块放在最末尾
$bOffset = $stubBytes.Length
$aOffset = $bOffset + $bBytes.Length

$cfg = New-Object System.Collections.Generic.List[byte]
$cfg.AddRange([BitConverter]::GetBytes([uint32]$VERSION))
$cfg.AddRange([BitConverter]::GetBytes([uint32]$FLAGS))
$cfg.AddRange([BitConverter]::GetBytes([uint64]$bOffset))
$cfg.AddRange([BitConverter]::GetBytes([uint64]$bBytes.Length))
$cfg.AddRange([BitConverter]::GetBytes([uint64]$aOffset))
$cfg.AddRange([BitConverter]::GetBytes([uint64]$aBytes.Length))
$cfg.AddRange([BitConverter]::GetBytes([uint32]$TIMEOUT))
for ($i = 0; $i -lt 7; $i++) { $cfg.AddRange([BitConverter]::GetBytes([uint32]0)) }
if ($cfg.Count -ne $CFG_SIZE) { throw "StubConfig 大小错误: $($cfg.Count)，应为 $CFG_SIZE" }

# trailerMagic 定长 32 字节，不足补 0
$magicBytes = New-Object byte[] 32
$tmp = [System.Text.Encoding]::ASCII.GetBytes($TRAILER_MAGIC)
[Array]::Copy($tmp, $magicBytes, $tmp.Length)

$ms = New-Object System.IO.MemoryStream
$ms.Write($stubBytes, 0, $stubBytes.Length)
$ms.Write($bBytes, 0, $bBytes.Length)
$ms.Write($aBytes, 0, $aBytes.Length)
$ms.Write($cfg.ToArray(), 0, $cfg.Count)
$ms.Write($magicBytes, 0, 32)
[System.IO.File]::WriteAllBytes($Output, $ms.ToArray())
$ms.Dispose()

$expect = $stubBytes.Length + $bBytes.Length + $aBytes.Length + $TRAILER_SIZE
$actual = (Get-Item $Output).Length
if ($actual -ne $expect) { throw "大小不符: $actual != $expect" }

Write-Host ("已生成: {0} ({1:N0} 字节)" -f $Output, $actual)
Write-Host ("  stub={0} B={1} A={2} trailer={3}" -f `
    $stubBytes.Length, $bBytes.Length, $aBytes.Length, $TRAILER_SIZE)
Write-Host ("  bOffset={0} aOffset={1} flags=0x{2:X}" -f $bOffset, $aOffset, $FLAGS)
