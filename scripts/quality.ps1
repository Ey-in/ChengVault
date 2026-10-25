param([string]$Toolchain = '', [string]$Python = '', [switch]$AllowFindings)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
if (!$Toolchain) { $Toolchain = Join-Path $projectRoot '.toolchain\root\ucrt64' }
if (!$Python) { $Python = (Get-Command python -ErrorAction Stop).Source }
$reportDir = Join-Path $projectRoot 'evidence\quality'
New-Item -ItemType Directory -Force -Path $reportDir | Out-Null
$env:PATH = "$Toolchain\bin;$env:PATH"
$formatter = Join-Path $projectRoot '.toolchain\python-tools\clang_format\data\bin\clang-format.exe'
if (!(Test-Path -LiteralPath $formatter)) { $formatter = (Get-Command clang-format -ErrorAction Stop).Source }
$files = Get-ChildItem -LiteralPath (Join-Path $projectRoot 'src'),(Join-Path $projectRoot 'include'),(Join-Path $projectRoot 'tests') -Recurse -File | Where-Object Extension -In @('.cpp','.hpp','.h')
$formatLog = Join-Path $reportDir 'format.log'
[IO.File]::WriteAllText($formatLog, '', [Text.UTF8Encoding]::new($false))
$formatFailures = @()
# PS5 wraps native stderr as ErrorRecords. A format diagnostic is evidence to
# collect, not a reason to abort before recording the remaining audited files.
function Invoke-AuditedNative([string]$Executable, [string[]]$Arguments, [string]$LogPath, [bool]$Append = $false) {
    if (!(Test-Path -LiteralPath $Executable -PathType Leaf)) { throw "Audit executable missing: $Executable" }
    $ErrorActionPreference = 'Continue'
    $nativeOutput = & $Executable @Arguments 2>&1
    $nativeExit = $LASTEXITCODE
    $logText = ($nativeOutput | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    if ($logText) { $logText += [Environment]::NewLine }
    if ($Append) { [IO.File]::AppendAllText($LogPath, $logText, [Text.UTF8Encoding]::new($false)) }
    else { [IO.File]::WriteAllText($LogPath, $logText, [Text.UTF8Encoding]::new($false)) }
    return $nativeExit
}
Push-Location $projectRoot
try {
    # Audit only: this script never rewrites source files or silently suppresses
    # a style difference. Developers opt into formatting separately.
    foreach ($file in $files) {
        $formatExit = Invoke-AuditedNative $formatter @('--dry-run', '--Werror', $file.FullName) $formatLog $true
        if ($formatExit) { $formatFailures += $file.FullName.Substring($projectRoot.Length + 1) }
    }
    & $Python (Join-Path $PSScriptRoot 'comment_audit.py') --root $projectRoot --output (Join-Path $reportDir 'comments.json')
    if ($LASTEXITCODE) { throw 'Comment audit failed' }
    $xmlPath = Join-Path $reportDir 'cppcheck.xml'
    $cppcheckLog = Join-Path $reportDir 'cppcheck.log'
    $cppcheckArguments = @('--enable=warning,style,performance,portability', '--std=c++20', '--platform=win64', '--library=qt', '-D_WIN32', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', "-I$projectRoot\include", '--xml', '--xml-version=2', "--output-file=$xmlPath", 'src', 'include')
    $cppcheckExit = Invoke-AuditedNative "$Toolchain\bin\cppcheck.exe" $cppcheckArguments $cppcheckLog
    $findings = @()
    if (Test-Path -LiteralPath $xmlPath) {
        [xml]$document = Get-Content -LiteralPath $xmlPath -Raw -Encoding UTF8
        $findings = @($document.results.errors.error | Where-Object { $_.severity -in @('error','warning','style','performance','portability') })
    }
    $commentReport = Get-Content -LiteralPath (Join-Path $reportDir 'comments.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    # Compare exact integer counts. Rounded display percentages must not turn
    # a value at or just below the required strict 20% boundary into a pass.
    $commentPass = (100 * [long]$commentReport.commentLines) -gt (20 * [long]$commentReport.totalLines)
    $summary = [ordered]@{
        formatCheckedFiles = $files.Count; formatNoncompliantFiles = $formatFailures
        cppcheckExit = $cppcheckExit; cppcheckFindingCount = $findings.Count
        cppcheckErrorCount = @($findings | Where-Object severity -EQ 'error').Count
        cppcheckWarningCount = @($findings | Where-Object severity -EQ 'warning').Count
        cppcheckStyleCount = @($findings | Where-Object severity -EQ 'style').Count
        cppcheckPerformanceCount = @($findings | Where-Object severity -EQ 'performance').Count
        reviewNotes = 'docs/quality-and-coverage.md; original findings remain in cppcheck.xml'
        commentPercentPhysical = $commentReport.commentPercentPhysical
        commentPercentNonblank = $commentReport.commentPercentNonblank
        commentThresholdExclusive = 20; commentPass = $commentPass
        generatedAtUtc = [DateTime]::UtcNow.ToString('o')
    }
    $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $reportDir 'summary.json') -Encoding UTF8
    $summary | ConvertTo-Json -Depth 5
    if (($formatFailures.Count -or $cppcheckExit -or $findings.Count -or !$commentPass) -and !$AllowFindings) {
        throw 'Quality audit has findings; inspect evidence/quality. No source was rewritten.'
    }
}
finally { Pop-Location }
