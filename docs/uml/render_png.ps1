param([string]$Directory = $PSScriptRoot)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# This is a local rendering helper for our own diagram-layouts.json. It does
# not invoke a UML generator or change the semantic StarUML model.
$layouts = Get-Content -LiteralPath (Join-Path $Directory 'diagram-layouts.json') -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($diagram in $layouts) {
    $bitmap = [System.Drawing.Bitmap]::new([int]$diagram.width, [int]$diagram.height)
    $bitmap.SetResolution(144, 144)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([System.Drawing.Color]::White)
    $ink = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#172c42'))
    $fill = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#edf7fb'))
    $white = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $border = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#2b6d8a'), 2)
    $edgePen = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#53657c'), 2)
    $accent = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#2aa89b'), 5)
    $smallFont = [System.Drawing.Font]::new('Microsoft YaHei', 16, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $labelFont = [System.Drawing.Font]::new('Microsoft YaHei', 17, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $nodeFont = [System.Drawing.Font]::new('Microsoft YaHei', 19, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $titleFont = [System.Drawing.Font]::new('Microsoft YaHei', 29, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $center = [System.Drawing.StringFormat]::new()
    $center.Alignment = [System.Drawing.StringAlignment]::Center
    $center.LineAlignment = [System.Drawing.StringAlignment]::Center
    $graphics.DrawString([string]$diagram.title, $titleFont, $ink, [float]35, [float]16)
    $graphics.DrawLine($accent, 35, 70, 1465, 70)
    if ($diagram.type -eq 'UMLUseCaseDiagram') {
        $graphics.DrawRectangle($border, 210, 95, 1220, 965)
        $graphics.DrawString('System: ChengVault', $smallFont, $ink, [float]235, [float]104)
    }
    foreach ($edge in $diagram.edges) {
        $edgePen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Solid
        if ($edge.kind -in @('dependency', 'include') -or $edge.reply) { $edgePen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash }
        for ($i = 1; $i -lt $edge.points.Count; $i++) {
            $a = $edge.points[$i - 1]; $b = $edge.points[$i]
            $graphics.DrawLine($edgePen, [float]$a[0], [float]$a[1], [float]$b[0], [float]$b[1])
        }
        if ($edge.kind -ne 'association') {
            $a = $edge.points[$edge.points.Count - 2]; $b = $edge.points[$edge.points.Count - 1]
            $angle = [Math]::Atan2($b[1] - $a[1], $b[0] - $a[0])
            $edgePen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Solid
            foreach ($delta in @(-0.5, 0.5)) {
                $graphics.DrawLine($edgePen, [float]$b[0], [float]$b[1], [float]($b[0] - 13 * [Math]::Cos($angle + $delta)), [float]($b[1] - 13 * [Math]::Sin($angle + $delta)))
            }
        }
        $label = [string]$edge.label
        if (!$label -and $edge.kind -eq 'include') { $label = [char]0x00ab + 'include' + [char]0x00bb }
        if ($label) {
            $cx = ($edge.points[0][0] + $edge.points[1][0]) / 2
            $cy = ($edge.points[0][1] + $edge.points[1][1]) / 2 - 25
            if ($edge.labelPosition) { $cx = $edge.labelPosition[0]; $cy = $edge.labelPosition[1] - 18 }
            $size = $graphics.MeasureString($label, $labelFont)
            $rect = [System.Drawing.RectangleF]::new([float]($cx - $size.Width / 2 - 4), [float]$cy, [float]($size.Width + 8), [float]$size.Height)
            $graphics.FillRectangle($white, $rect)
            $graphics.DrawString($label, $labelFont, $ink, $rect, $center)
        }
    }
    foreach ($node in $diagram.nodes) {
        $x = [float]$node.x; $y = [float]$node.y; $w = [float]$node.w; $h = [float]$node.h
        if ($node.kind -eq 'initial') { $graphics.FillEllipse($ink, $x, $y, $w, $h); continue }
        if ($node.kind -eq 'actor') {
            $cx = $x + $w / 2
            $graphics.DrawEllipse($border, [float]($cx - 12), [float]($y + 2), [float]24, [float]24)
            $graphics.DrawLine($border, $cx, [float]($y + 26), $cx, [float]($y + 60))
            $graphics.DrawLine($border, [float]($cx - 24), [float]($y + 40), [float]($cx + 24), [float]($y + 40))
            $graphics.DrawLine($border, $cx, [float]($y + 60), [float]($cx - 23), [float]($y + 87))
            $graphics.DrawLine($border, $cx, [float]($y + 60), [float]($cx + 23), [float]($y + 87))
            $graphics.DrawString([string]$node.title, $nodeFont, $ink, [System.Drawing.RectangleF]::new($x, [float]($y + 92), $w, 28), $center)
            continue
        }
        $height = $h
        if ($node.kind -eq 'lifeline') { $height = 60 }
        $rect = [System.Drawing.RectangleF]::new($x, $y, $w, $height)
        if ($node.kind -eq 'usecase') { $graphics.FillEllipse($fill, $rect); $graphics.DrawEllipse($border, $rect) }
        else { $graphics.FillRectangle($fill, $rect); $graphics.DrawRectangle($border, $x, $y, $w, $height) }
        $titleHeight = 60
        if ($node.kind -in @('state', 'usecase')) { $titleHeight = $h }
        $graphics.DrawString([string]$node.title, $nodeFont, $ink, [System.Drawing.RectangleF]::new($x, $y, $w, $titleHeight), $center)
        if ($node.kind -eq 'class') {
            $graphics.DrawLine($border, $x, [float]($y + 48), [float]($x + $w), [float]($y + 48))
            for ($i = 0; $i -lt $node.lines.Count; $i++) {
                $graphics.DrawString([string]$node.lines[$i], $smallFont, $ink, [float]($x + 12), [float]($y + 55 + $i * 27))
            }
        }
        elseif ($node.kind -eq 'lifeline') {
            $edgePen.DashStyle = [System.Drawing.Drawing2D.DashStyle]::Dash
            $graphics.DrawLine($edgePen, [float]($x + $w / 2), [float]($y + 60), [float]($x + $w / 2), [float]($y + $h))
        }
    }
    $graphics.DrawString([string]$diagram.note, $smallFont, $ink, [float]35, [float]($diagram.height - 48))
    $bitmap.Save((Join-Path $Directory ($diagram.key + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
    foreach ($resource in @($center, $titleFont, $nodeFont, $labelFont, $smallFont, $accent, $edgePen, $border, $white, $fill, $ink, $graphics, $bitmap)) { $resource.Dispose() }
}
Write-Output ('Rendered ' + $layouts.Count + ' PNG diagrams.')
