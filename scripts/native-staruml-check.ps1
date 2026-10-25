#requires -Version 7.2
param(
    [string]$Application=(Join-Path $PSScriptRoot '..\.toolchain\staruml\StarUML.exe'),
    [string]$Model=(Join-Path $PSScriptRoot '..\docs\uml\ChengVault.mdj'),
    [string]$EvidenceDirectory=(Join-Path $PSScriptRoot '..\evidence\uml-native'),
    [switch]$ProbeOnly
)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$Application=[IO.Path]::GetFullPath($Application); $Model=[IO.Path]::GetFullPath($Model)
$EvidenceDirectory=[IO.Path]::GetFullPath($EvidenceDirectory)
[IO.Directory]::CreateDirectory($EvidenceDirectory)|Out-Null
$run=Join-Path $project ('tmp\staruml-isolated-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($run)|Out-Null
$helper=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot 'isolated-desktop.cs'))
if(-not ('IsolatedDesktop' -as [type])) {Add-Type -LiteralPath $helper}
$before=[IsolatedDesktop]::InputName()
$name='ChengVaultVerification-'+[Guid]::NewGuid().ToString('N')
$desktop=[IntPtr]::Zero; $job=[IntPtr]::Zero
$settingsPath=Join-Path $env:APPDATA 'StarUML\settings.json'
$settingsChanged=$false; $settingsBytes=$null; $hadSettings=Test-Path -LiteralPath $settingsPath
if($hadSettings) {$settingsBytes=[IO.File]::ReadAllBytes($settingsPath)}
$result=[ordered]@{checkedAtUtc=[DateTime]::UtcNow.ToString('o');nativeStarUmlOpenVerified=$false;nativeRenderingVerified=$false;outcome='NOT_VERIFIED';activeDesktopBefore=$before;isolatedDesktop=$name;switchDesktopCalled=$false;testProcesses=@();officialApi='https://docs.staruml.io/user-guide/ai-integration-via-mcp';modelSha256=(Get-FileHash -LiteralPath $Model).Hash.ToLowerInvariant()}
try {
    $desktop=[IsolatedDesktop]::Create($name); $job=[IsolatedDesktop]::Job()
    $probePath=Join-Path $run 'probe.ps1'; $probeOutput=Join-Path $run 'probe.json'
    $probe=@'
param([string]$Helper,[string]$Output)
$ErrorActionPreference='Stop'
Add-Type -LiteralPath $Helper
[ordered]@{threadDesktop=[IsolatedDesktop]::Name([IsolatedDesktop]::GetThreadDesktop([IsolatedDesktop]::GetCurrentThreadId()));activeInputDesktop=[IsolatedDesktop]::InputName();pid=$PID}|ConvertTo-Json|Set-Content -LiteralPath $Output -Encoding utf8
'@
    [IO.File]::WriteAllText($probePath,$probe,[Text.UTF8Encoding]::new($false))
    $shell=(Get-Process -Id $PID).Path
    $probePid=[IsolatedDesktop]::Launch($job,$name,$shell,('-NoProfile -NonInteractive -File "'+$probePath+'" -Helper "'+$helper+'" -Output "'+$probeOutput+'"'),$run)
    $result.testProcesses+=@{pid=$probePid;purpose='desktop identity probe'}
    $process=Get-Process -Id $probePid
    if(-not $process.WaitForExit(20000) -or -not (Test-Path -LiteralPath $probeOutput)) { throw 'The independent desktop probe did not complete.' }
    $probeResult=Get-Content -LiteralPath $probeOutput -Raw|ConvertFrom-Json
    if($probeResult.threadDesktop -ne $name -or $probeResult.activeInputDesktop -ne $before -or [IsolatedDesktop]::InputName() -ne $before) { throw 'Desktop isolation could not be established. StarUML will not be started.' }
    $result['desktopProbe']=$probeResult; $result['desktopIsolationVerified']=$true
    Write-Host "Independent desktop probe passed: child=$name, active=$before."
    if($ProbeOnly) { $result.outcome='PROBE_ONLY_PASS'; return }
    if(Get-Process StarUML -ErrorAction SilentlyContinue) { throw 'An existing StarUML process is running; refusing to interact with it.' }
    if(Get-NetTCPConnection -LocalPort 58321 -State Listen -ErrorAction SilentlyContinue) { throw 'The official API port is already occupied; refusing to contact another process.' }
    $signature=Get-AuthenticodeSignature -LiteralPath $Application
    if($signature.Status -ne 'Valid') { throw 'StarUML application signature is not valid.' }
    $result['applicationSha256']=(Get-FileHash -LiteralPath $Application).Hash.ToLowerInvariant()
    $result['signature']=$signature.SignerCertificate.Subject
    $result['version']=(Get-Item -LiteralPath $Application).VersionInfo.ProductVersion
    if($hadSettings) {$settingsBytes=[IO.File]::ReadAllBytes($settingsPath);$settings=Get-Content -LiteralPath $settingsPath -Raw|ConvertFrom-Json -AsHashtable} else {$settings=@{}}
    $settings.apiServer=$true; $settings.apiServerPort=58321
    $settings|ConvertTo-Json -Depth 30|Set-Content -LiteralPath $settingsPath -Encoding utf8
    $settingsChanged=$true
    # Only the official model pathname argument is supplied; there are no
    # Electron switches, debugging flags, license edits or app source changes.
    $appPid=[IsolatedDesktop]::Launch($job,$name,$Application,('"'+$Model+'"'),$run)
    $result.testProcesses+=@{pid=$appPid;purpose='native model and diagram rendering'}
    Write-Host "StarUML started exclusively on the verified inactive desktop (PID $appPid)."
    $diagrams=$null
    for($attempt=0;$attempt -lt 30;++$attempt) {
        try {$response=Invoke-RestMethod -Uri 'http://127.0.0.1:58321/get_all_diagrams_info' -Method Post -ContentType 'application/json' -Body '{}' -TimeoutSec 2;if($response.success){$diagrams=$response.data;if(@($diagrams).Count -eq 7){break}}} catch {}
        Start-Sleep -Milliseconds 500
    }
    if($null -eq $diagrams) { throw 'StarUML did not return native diagram metadata through its official API; no rendering result is claimed.' }
    $diagrams|ConvertTo-Json -Depth 30|Set-Content -LiteralPath (Join-Path $EvidenceDirectory 'native-diagrams.json') -Encoding utf8
    $result['diagramResponse']=$diagrams
    # The API returns a list of native diagrams. Save its actual response first
    # so a changed vendor schema cannot be mistaken for successful validation.
    $items=@($diagrams)
    if($items.Count -ne 7) { throw "Expected seven native diagrams; API returned $($items.Count)." }
    $images=[Collections.Generic.List[object]]::new()
    foreach($diagram in $items) {
        $diagramId=if($diagram.id){$diagram.id}else{$diagram._id}
        if(-not $diagramId) { throw 'Native diagram metadata did not contain a diagram identifier.' }
        $response=Invoke-RestMethod -Uri 'http://127.0.0.1:58321/get_diagram_image_by_id' -Method Post -ContentType 'application/json' -Body (@{diagramId=$diagramId}|ConvertTo-Json -Compress) -TimeoutSec 15
        if(-not $response.success -or -not $response.data) { throw "Native rendering failed for $diagramId" }
        $base64=[string]$response.data
        if($base64.StartsWith('data:')) {$base64=$base64.Substring($base64.IndexOf(',')+1)}
        $png=[Convert]::FromBase64String($base64)
        if($png.Length -lt 8 -or [Convert]::ToHexString($png[0..7]) -ne '89504E470D0A1A0A') { throw 'Native image response is not a PNG.' }
        $output=Join-Path $EvidenceDirectory ('native-'+($images.Count+1).ToString('00')+'.png')
        [IO.File]::WriteAllBytes($output,$png)
        $images.Add([ordered]@{id=$diagramId;name=$diagram.name;path=$output;bytes=$png.Length;sha256=(Get-FileHash -LiteralPath $output).Hash.ToLowerInvariant()})
    }
    $result['images']=$images; $result.nativeStarUmlOpenVerified=$true; $result.nativeRenderingVerified=$true; $result.outcome='NATIVE_RENDER_PASS_VISUAL_REVIEW_PENDING'
} catch {
    $result['error']=$_.Exception.Message
    Write-Warning $_.Exception.Message
} finally {
    # Closing only our job kills exactly its assigned process tree, even if
    # Electron spawned children. The desktop handle is released afterwards.
    if($job -ne [IntPtr]::Zero) {$result['jobClosed']=[IsolatedDesktop]::CloseHandle($job);Start-Sleep -Milliseconds 300}
    if($desktop -ne [IntPtr]::Zero) {$result['desktopClosed']=[IsolatedDesktop]::CloseDesktop($desktop)}
    if($settingsChanged) {if($hadSettings){[IO.File]::WriteAllBytes($settingsPath,$settingsBytes)}else{Remove-Item -LiteralPath $settingsPath}}
    $result['settingsRestored']=if($hadSettings){(Get-FileHash -LiteralPath $settingsPath).Hash -eq [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($settingsBytes))}else{-not (Test-Path -LiteralPath $settingsPath)}
    $result['activeDesktopAfter']=[IsolatedDesktop]::InputName()
    $result['activeDesktopUnchanged']=$result.activeDesktopAfter -eq $before
    $result|ConvertTo-Json -Depth 40|Set-Content -LiteralPath (Join-Path $EvidenceDirectory $(if($ProbeOnly){'isolation-probe.json'}else{'isolated-native-validation.json'})) -Encoding utf8
    Write-Host "Outcome: $($result.outcome); active desktop unchanged: $($result.activeDesktopUnchanged)."
}
