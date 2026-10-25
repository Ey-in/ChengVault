param(
    [string]$Toolchain = '',
    [string]$Python = '',
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$KeepCounters,
    [switch]$AllowBelowTarget,
    [double]$LineTarget = 85,
    [double]$BranchTarget = 75
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$Toolchain) { $Toolchain = Join-Path $projectRoot '.toolchain\root\ucrt64' }
if (!$Python) {
    $bundledPython = Join-Path $env:USERPROFILE '.cache\python-tools\python.exe'
    if (Test-Path -LiteralPath $bundledPython) { $Python = $bundledPython }
    else { $Python = (Get-Command python -ErrorAction Stop).Source }
}
$buildDir = Join-Path $projectRoot 'build-coverage'
$reportDir = Join-Path $projectRoot 'evidence\coverage'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$env:PATH = "$Toolchain\bin;$env:PATH"
$env:CHENGVAULT_TOOLCHAIN = $Toolchain.Replace('\', '/')
$env:PYTHONPATH = (Join-Path $projectRoot '.toolchain\python-tools') + [IO.Path]::PathSeparator + $env:PYTHONPATH
# Windows PowerShell 5 turns native stderr into ErrorRecords. gcov/gcovr use
# stderr for ordinary progress, so inspect process status instead of treating
# the existence of a stderr message as a failed command.
function Invoke-LoggedNative([string]$Executable, [string[]]$Arguments, [string]$LogPath) {
    $ErrorActionPreference = 'Continue'
    & $Executable @Arguments *> $LogPath
    $nativeExit = $LASTEXITCODE
    # PS5 redirection writes UTF-16; normalize evidence to portable UTF-8.
    $logText = [IO.File]::ReadAllText($LogPath)
    [IO.File]::WriteAllText($LogPath, $logText, [Text.UTF8Encoding]::new($false))
    if ($nativeExit -ne 0) { throw "Native process failed ($nativeExit): $Executable. Inspect $LogPath" }
}
Push-Location $projectRoot
try {
    if (!$SkipBuild) {
        Invoke-LoggedNative "$Toolchain\bin\cmake.exe" @('--preset','windows-coverage') (Join-Path $reportDir 'configure.log')
        Invoke-LoggedNative "$Toolchain\bin\cmake.exe" @('--build',$buildDir,'--target','ChengVaultTests','ChengVaultGuiTests','--parallel','3') (Join-Path $reportDir 'build.log')
    }
    if (!$SkipTests) {
        if (!$KeepCounters) {
            # Remove only gcov counter files inside this explicitly checked build.
            # Source, executable and unrelated test data are never deleted.
            $resolvedBuild = (Resolve-Path -LiteralPath $buildDir).Path
            $expectedBuild = [IO.Path]::GetFullPath((Join-Path $projectRoot 'build-coverage'))
            if ($resolvedBuild -ne $expectedBuild) { throw 'Unexpected coverage build path' }
            Get-ChildItem -LiteralPath $resolvedBuild -Filter '*.gcda' -Recurse -File | ForEach-Object {
                if (!$_.FullName.StartsWith($expectedBuild + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Counter outside build directory' }
                Remove-Item -LiteralPath $_.FullName
            }
        }
        Invoke-LoggedNative (Join-Path $buildDir 'ChengVaultTests.exe') @('--output',(Join-Path $reportDir 'core-tests.json')) (Join-Path $reportDir 'core-tests.log')
        Invoke-LoggedNative (Join-Path $buildDir 'ChengVaultGuiTests.exe') @('--output',(Join-Path $reportDir 'gui-tests.json')) (Join-Path $reportDir 'gui-tests.log')
    }
    # This filter includes ALL production files in core and platform. No source
    # line or exception branch is excluded to inflate the primary metrics.
    $filter = [regex]::Escape($projectRoot.Replace('\', '/')) + '/src/(core|platform)/'
    # GCC 16.2 on this Windows toolchain truncates very large JSON execution
    # counts to signed 32 bits. Its ordinary text output retains full 64-bit
    # counts. Parse that unmodified text; never ignore negative-count errors.
    $traceDir = Join-Path $reportDir 'gcov-text'
    Invoke-LoggedNative $Python @((Join-Path $PSScriptRoot 'collect_gcov.py'),'--build',$buildDir,'--gcov',"$Toolchain\bin\gcov.exe",'--output',$traceDir) (Join-Path $reportDir 'collect.log')
    # SHA rotations legitimately execute billions of times across password-KDF
    # integration cases. Retain those exact counts; only reject beyond int64.
    $gcovrBase = @('-m','gcovr','--root',$projectRoot,$traceDir,'--filter',$filter,'--gcov-use-existing-files','--keep-intermediate-files','--gcov-suspicious-hits-threshold','9223372036854775807','--no-markers','--decisions','--gcov-executable',"$Toolchain\bin\gcov.exe")
    Invoke-LoggedNative $Python ($gcovrBase + @('--json',(Join-Path $reportDir 'coverage.json'),'--json-summary',(Join-Path $reportDir 'summary.json'),'--html-details',(Join-Path $reportDir 'index.html'),'--txt',(Join-Path $reportDir 'coverage.txt'),'--print-summary')) (Join-Path $reportDir 'gcovr.log')
    # Supplementary only: distinguish compiler exception arcs from ordinary
    # source branches. This never replaces the unfiltered threshold check.
    Invoke-LoggedNative $Python ($gcovrBase + @('--exclude-throw-branches','--exclude-unreachable-branches','--json-summary',(Join-Path $reportDir 'auxiliary-summary.json'),'--print-summary')) (Join-Path $reportDir 'auxiliary.log')
    $summary = Get-Content -LiteralPath (Join-Path $reportDir 'summary.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $linePass = $summary.line_percent -ge $LineTarget
    $branchPass = $summary.branch_percent -ge $BranchTarget
    $thresholds = [ordered]@{
        lineTarget = $LineTarget; branchTarget = $BranchTarget
        lineActual = $summary.line_percent; branchActual = $summary.branch_percent
        linePass = $linePass; branchPass = $branchPass
        primaryBranchMetric = 'All GCC CFG arcs, including exception-handling arcs'
        scope = 'All src/core/*.cpp and src/platform/*.cpp'
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    }
    $thresholds | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reportDir 'thresholds.json') -Encoding UTF8
    Get-Content -LiteralPath (Join-Path $reportDir 'coverage.txt')
    if ((!$linePass -or !$branchPass) -and !$AllowBelowTarget) {
        throw 'Coverage target not reached. Report contains actual numbers; no code was excluded.'
    }
}
finally { Pop-Location }
