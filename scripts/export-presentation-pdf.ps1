param([string]$Deck=(Join-Path $PSScriptRoot '..\..\PPT\ChengVault项目答辩.pptx'))
$ErrorActionPreference='Stop'
$deckPath=[IO.Path]::GetFullPath($Deck)
$pdfPath=[IO.Path]::ChangeExtension($deckPath,'.pdf')
$imageDirectory=Join-Path (Split-Path $PSScriptRoot -Parent) '.cache\presentation\native'
# Office is an authoring/verification dependency, not a runtime dependency of
# the backup application. Preserve any already-open user presentations.
$powerpoint=New-Object -ComObject PowerPoint.Application
$existing=$powerpoint.Presentations.Count
$presentation=$null
try {
    $presentation=$powerpoint.Presentations.Open($deckPath,-1,0,0)
    $presentation.SaveAs($pdfPath,32)
    $presentation.Export($imageDirectory,'PNG',1280,720)
    [pscustomobject]@{slides=$presentation.Slides.Count;pdf=$pdfPath;images=$imageDirectory}
} finally {
    if($presentation){$presentation.Close(); [void][Runtime.InteropServices.Marshal]::ReleaseComObject($presentation)}
    if($existing -eq 0){$powerpoint.Quit()}
    [void][Runtime.InteropServices.Marshal]::ReleaseComObject($powerpoint)
}
