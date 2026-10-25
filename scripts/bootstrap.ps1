param([string]$MsysRoot='D:\msys2')
$ErrorActionPreference = 'Stop'
# MSYS2 itself is deliberately not installed silently. This script operates on
# the explicitly selected installation and never mixes Conda Qt with GCC.
$bash = Join-Path $MsysRoot 'usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw 'Install MSYS2 from https://www.msys2.org/ and supply -MsysRoot.' }
& $bash -lc 'pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-qt6-tools mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja'
if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
