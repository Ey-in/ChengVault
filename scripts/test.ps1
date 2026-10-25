param([string]$Configuration='Release',[string]$Toolchain='')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (-not $Toolchain) { $Toolchain = Join-Path $projectRoot '.toolchain\root\ucrt64' }
if (-not (Test-Path -LiteralPath "$Toolchain\bin\qmake6.exe")) { $Toolchain = 'D:\msys2\ucrt64' }
$env:PATH = "$Toolchain\bin;$env:PATH"
$buildDir = Join-Path $projectRoot ('build-' + $Configuration.ToLowerInvariant())
& (Join-Path $buildDir 'ChengVaultTests.exe') --output (Join-Path $projectRoot 'evidence\test-results.json')
if ($LASTEXITCODE -ne 0) { throw 'Tests failed; inspect evidence/test-results.json' }
& (Join-Path $buildDir 'ChengVaultGuiTests.exe') --output (Join-Path $projectRoot 'evidence\gui-test-results.json')
if ($LASTEXITCODE -ne 0) { throw 'GUI tests failed; inspect evidence/gui-test-results.json' }
& (Join-Path $buildDir 'ChengVaultBackgroundTests.exe') --output (Join-Path $projectRoot 'evidence\background-test-results.json')
if ($LASTEXITCODE -ne 0) { throw 'Background tests failed; inspect evidence/background-test-results.json' }
