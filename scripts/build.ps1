param([ValidateSet('Release','Debug','Coverage')][string]$Configuration='Release', [string]$Toolchain='')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $Toolchain) { $Toolchain = Join-Path $projectRoot '.toolchain\root\ucrt64' }
if (-not (Test-Path -LiteralPath "$Toolchain\bin\cmake.exe")) { $Toolchain = 'D:\msys2\ucrt64' }
$env:CHENGVAULT_TOOLCHAIN = $Toolchain.Replace('\','/')
$env:PATH = "$Toolchain\bin;$env:PATH"
$preset = 'windows-' + $Configuration.ToLowerInvariant()
& "$Toolchain\bin\cmake.exe" --preset $preset -S $projectRoot
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& "$Toolchain\bin\cmake.exe" --build (Join-Path $projectRoot ('build-' + $Configuration.ToLowerInvariant())) --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
