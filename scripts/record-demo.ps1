#requires -Version 7.2
param(
    [string]$BinaryDirectory = (Join-Path $PSScriptRoot '..\build-release'),
    [switch]$DeferPublish
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$demoDirectory = Join-Path $projectRoot ('evidence\demo\run-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
[IO.Directory]::CreateDirectory($demoDirectory) | Out-Null
$ffmpeg = Join-Path $projectRoot '.toolchain\python-tools\imageio_ffmpeg\binaries\ffmpeg-win-x86_64-v7.1.exe'
if (-not (Test-Path -LiteralPath $ffmpeg)) { throw 'Install imageio-ffmpeg into .toolchain/python-tools, or supply its bundled executable.' }
if (-not ('DemoRecordingWindow' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class DemoRecordingWindow {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindow(string className, string windowName);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    public static bool BelongsToProcess(string title, uint expectedProcessId) {
        IntPtr window = FindWindow(null, title);
        if (window == IntPtr.Zero) return false;
        uint processId;
        GetWindowThreadProcessId(window, out processId);
        return processId == expectedProcessId;
    }
}
'@
}
# The tested application performs real operations. ffmpeg records its native
# window; no reconstructed screens or simulated success messages are used.
$testInfo = [Diagnostics.ProcessStartInfo]::new((Join-Path $BinaryDirectory 'ChengVaultGuiTests.exe'))
$testInfo.UseShellExecute = $false
$testInfo.CreateNoWindow = $true
$testInfo.Environment['PATH'] = (Join-Path $projectRoot '.toolchain\root\ucrt64\bin') + ';' + $env:PATH
$testInfo.Environment['QT_SCALE_FACTOR'] = '0.7'
$testInfo.ArgumentList.Add('--demo-dir')
$testInfo.ArgumentList.Add($demoDirectory)
$testInfo.ArgumentList.Add('--output')
$testInfo.ArgumentList.Add((Join-Path $demoDirectory 'demo-test-results.json'))
$testInfo.RedirectStandardOutput = $true
$testInfo.RedirectStandardError = $true
$testProcess = [Diagnostics.Process]::Start($testInfo)
$testOutput = $testProcess.StandardOutput.ReadToEndAsync()
$testError = $testProcess.StandardError.ReadToEndAsync()
$deadline = [DateTime]::UtcNow.AddSeconds(15)
do {
    Start-Sleep -Milliseconds 150
    $testProcess.Refresh()
    # FindWindow uses this process's assigned desktop, as gdigrab does. The
    # .NET MainWindowTitle heuristic can miss an inactive-desktop window and
    # silently waste the first 15 seconds of the actual demonstration.
    $windowReady=[DemoRecordingWindow]::BelongsToProcess('ChengVault Demonstration',[uint32]$testProcess.Id)
} until ($windowReady -or $testProcess.HasExited -or [DateTime]::UtcNow -gt $deadline)
if ($testProcess.HasExited) { throw ('GUI test failed before recording: ' + $testError.Result) }
if (-not $windowReady) { $testProcess.Kill($true); throw 'The owned demonstration window did not become available for capture.' }
$videoPath = Join-Path $demoDirectory 'ChengVault-真实运行演示.mp4'
$videoInfo = [Diagnostics.ProcessStartInfo]::new($ffmpeg)
$videoInfo.UseShellExecute = $false
$videoInfo.CreateNoWindow = $true
$videoInfo.RedirectStandardInput = $true
$videoInfo.RedirectStandardError = $true
foreach ($argument in @('-y','-f','gdigrab','-framerate','12','-draw_mouse','0','-i','title=ChengVault Demonstration','-t','170','-vf','scale=1280:720:force_original_aspect_ratio=decrease,pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=white','-c:v','libx264','-crf','23','-pix_fmt','yuv420p','-movflags','+faststart',$videoPath)) { $videoInfo.ArgumentList.Add($argument) }
$videoProcess = [Diagnostics.Process]::Start($videoInfo)
$videoError = $videoProcess.StandardError.ReadToEndAsync()
$testProcess.WaitForExit()
if (-not $videoProcess.HasExited) { $videoProcess.StandardInput.WriteLine('q'); $videoProcess.StandardInput.Flush() }
$videoProcess.WaitForExit()
[IO.File]::WriteAllText((Join-Path $demoDirectory 'gui-test.log'),$testOutput.Result + $testError.Result,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $demoDirectory 'ffmpeg.log'),$videoError.Result,[Text.UTF8Encoding]::new($false))
if ($testProcess.ExitCode -ne 0 -or $videoProcess.ExitCode -ne 0) { throw "Demo failed; inspect $demoDirectory" }
$deliveryDirectory=Join-Path $projectRoot '演示视频'
# Decode the recorded file before reporting its duration. A present but partial
# container is not a successful demonstration deliverable.
$decodeInfo = [Diagnostics.ProcessStartInfo]::new($ffmpeg)
$decodeInfo.UseShellExecute = $false
$decodeInfo.CreateNoWindow = $true
$decodeInfo.RedirectStandardError = $true
foreach ($argument in @('-i',$videoPath,'-f','null','-')) { $decodeInfo.ArgumentList.Add($argument) }
$decode = [Diagnostics.Process]::Start($decodeInfo)
$decodeError = $decode.StandardError.ReadToEndAsync()
if (-not $decode.WaitForExit(60000)) { $decode.Kill($true); throw 'Recorded video validation timed out.' }
$decodedLog = $decodeError.Result
[IO.File]::WriteAllText((Join-Path $demoDirectory 'decode.log'),$decodedLog,[Text.UTF8Encoding]::new($false))
$duration = [regex]::Match($decodedLog,'Duration:\s*(\d+):(\d+):(\d+(?:\.\d+)?)')
if ($decode.ExitCode -ne 0 -or -not $duration.Success) { throw 'Recorded video did not decode completely.' }
$seconds = 3600 * [int]$duration.Groups[1].Value + 60 * [int]$duration.Groups[2].Value + [double]::Parse($duration.Groups[3].Value,[Globalization.CultureInfo]::InvariantCulture)
if ($seconds -le 0 -or $seconds -gt 180) { throw 'Video is outside the course duration requirement.' }
$result = Get-Content -LiteralPath (Join-Path $demoDirectory 'demo-test-results.json') -Raw | ConvertFrom-Json
$frames = @(Get-ChildItem -LiteralPath $demoDirectory -Filter '*.png' | Sort-Object Name | Select-Object -ExpandProperty Name)
if ($result.failed -ne 0 -or $frames.Count -ne 8) { throw 'Demonstration evidence is incomplete.' }
$recording=[ordered]@{
    timestampUtc=[DateTime]::UtcNow.ToString('o'); runDirectory=$demoDirectory
    videoPath=$videoPath
    durationSeconds=$seconds; fullyDecoded=$true; frames=$frames
    passed=$result.passed; failed=$result.failed
    executableSha256=(Get-FileHash -LiteralPath $testInfo.FileName).Hash.ToLowerInvariant()
    videoSha256=(Get-FileHash -LiteralPath $videoPath).Hash.ToLowerInvariant()
}
# Inactive-desktop recording must first be visually inspected: a decodable
# container alone does not prove gdigrab captured the application's pixels.
# Deferred runs keep both the existing published video and latest evidence
# untouched until a reviewer has checked frames from multiple timestamps.
$recording | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $demoDirectory 'recording-result.json') -Encoding utf8
if (-not $DeferPublish) {
    [IO.Directory]::CreateDirectory($deliveryDirectory) | Out-Null
    $deliveryPath=Join-Path $deliveryDirectory 'ChengVault-真实运行演示.mp4'
    Copy-Item -LiteralPath $videoPath -Destination $deliveryPath -Force
    $recording.videoPath=$deliveryPath
    $recording | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $projectRoot 'evidence\demo\latest.json') -Encoding utf8
}
Write-Output $videoPath
