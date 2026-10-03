# Windows equivalent of link-module.sh: creates a directory junction (no admin rights needed).
# Usage: powershell -File scripts\link-module.ps1 [-Core path\to\azerothcore]
param([string]$Core = "")
$root = Split-Path -Parent $PSScriptRoot
if ($Core -eq "") { $Core = Join-Path $root "vendor\azerothcore" }
if (-not (Test-Path (Join-Path $Core "modules\CMakeLists.txt"))) { throw "not an AzerothCore checkout: $Core" }
$target = Join-Path $Core "modules\mod-gamebridge"
if (Test-Path $target) {
    $item = Get-Item $target -Force
    if (-not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "$target exists and is not a link; refusing to overwrite" }
    $item.Delete()
}
New-Item -ItemType Junction -Path $target -Target (Join-Path $root "modules\mod-gamebridge") | Out-Null
Write-Host "linked $target"
