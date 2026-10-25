param([string]$BinaryDirectory=(Join-Path $PSScriptRoot '..\build-release'))
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot -Parent
$fixture=Join-Path $projectRoot ('evidence\metadata-times\run-'+[Guid]::NewGuid().ToString('N'))
$source=Join-Path $fixture 'source'
$empty=Join-Path $source '空目录'
$output=Join-Path $fixture 'restored'
[IO.Directory]::CreateDirectory($empty)|Out-Null
$file=Join-Path $source '三类时间.txt'
[IO.File]::WriteAllText($file,'ChengVault timestamp verification',[Text.UTF8Encoding]::new($false))
$expected=@{CreationTimeUtc=[datetime]'2021-02-03T04:05:06Z';LastAccessTimeUtc=[datetime]'2022-03-04T05:06:07Z';LastWriteTimeUtc=[datetime]'2023-04-05T06:07:08Z'}
foreach($path in @($file,$empty)) {
    $item=Get-Item -LiteralPath $path
    foreach($name in $expected.Keys) { $item.$name=$expected[$name] }
}
$cli=[IO.Path]::GetFullPath((Join-Path $BinaryDirectory 'ChengVaultCli.exe'))
function Invoke-TestCli([string[]]$Arguments) {
    $info=[Diagnostics.ProcessStartInfo]::new($cli)
    $info.UseShellExecute=$false; $info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true; $info.RedirectStandardError=$true
    $info.Environment['PATH']=(Join-Path $projectRoot '.toolchain\root\ucrt64\bin')+';'+$env:PATH
    $info.Environment['CHENGVAULT_PASSWORD']='metadata-time-fixture-only'
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::Start($info)
    $stdout=$process.StandardOutput.ReadToEndAsync(); $stderr=$process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    if($process.ExitCode -ne 0){throw $stderr.Result}
    return $stdout.Result | ConvertFrom-Json
}
$archive=Join-Path $fixture 'times.cva'
$backup=Invoke-TestCli @('backup','--source',$source,'--output',$archive,'--cipher','aes256','--compression','lz77')
$restore=Invoke-TestCli @('restore','--input',$archive,'--output',$output)
if(-not $backup.success -or -not $restore.success -or $restore.warnings.Count -ne 0){throw 'Strict backup/restore reported a failure or warning'}
# Query metadata before opening restored file contents. Reading a file for a
# content hash can itself affect access-time bookkeeping on some volumes.
$records=foreach($relative in @('三类时间.txt','空目录')) {
    $restored=Get-Item -LiteralPath (Join-Path $output ('source\'+$relative))
    foreach($name in @('CreationTimeUtc','LastAccessTimeUtc','LastWriteTimeUtc')) {
        $wanted=$expected[$name].ToUniversalTime().ToFileTimeUtc().ToString()
        $actual=$restored.$name.ToFileTimeUtc().ToString()
        [pscustomobject][ordered]@{name="META-TIME/$relative/$name";category='系统功能';expectedFileTime=$wanted;actualFileTime=$actual;passed=($wanted -eq $actual)}
    }
}
$result=[ordered]@{timestampUtc=[datetime]::UtcNow.ToString('o');fixture=$fixture;executableSha256=(Get-FileHash -LiteralPath $cli -Algorithm SHA256).Hash.ToLowerInvariant();strictMetadata=$true;warnings=$restore.warnings;records=@($records)}
$result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $projectRoot 'evidence\metadata-times-results.json') -Encoding utf8
$records|Format-Table name,passed
if(@($records|Where-Object {-not $_.passed}).Count){throw 'Timestamp comparison failed'}
