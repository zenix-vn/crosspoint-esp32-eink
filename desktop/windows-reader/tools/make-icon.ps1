# Draws app.ico (16-256 px, PNG entries): an open book on an indigo tile.
param([string]$Out = (Join-Path $PSScriptRoot '..\app.ico'))
Add-Type -AssemblyName System.Drawing

function New-Frame([int]$s) {
    $bmp = New-Object System.Drawing.Bitmap $s, $s
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $r = [single]($s * 0.22)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $w = [single]($s - 1)
    $path.AddArc(0, 0, $r * 2, $r * 2, 180, 90)
    $path.AddArc($w - $r * 2, 0, $r * 2, $r * 2, 270, 90)
    $path.AddArc($w - $r * 2, $w - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc(0, $w - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF 0, 0, $s, $s
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(99, 102, 241)), ([System.Drawing.Color]::FromArgb(139, 92, 246)), 45
    $g.FillPath($brush, $path)

    # Two pages meeting at the spine.
    $white = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 255))
    $soft = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(225, 228, 255))
    $cx = $s / 2.0; $top = $s * 0.27; $bot = $s * 0.75; $edge = $s * 0.18
    $left = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($cx - $s*0.02), ($top + $s*0.05)),
        (New-Object System.Drawing.PointF $edge, $top),
        (New-Object System.Drawing.PointF $edge, ($bot - $s*0.05)),
        (New-Object System.Drawing.PointF ($cx - $s*0.02), $bot))
    $right = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF ($cx + $s*0.02), ($top + $s*0.05)),
        (New-Object System.Drawing.PointF ($s - $edge), $top),
        (New-Object System.Drawing.PointF ($s - $edge), ($bot - $s*0.05)),
        (New-Object System.Drawing.PointF ($cx + $s*0.02), $bot))
    $g.FillPolygon($white, $left)
    $g.FillPolygon($soft, $right)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    return ,$ms.ToArray()
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$frames = $sizes | ForEach-Object { ,(New-Frame $_) }
$fs = [System.IO.File]::Create($Out)
$bw = New-Object System.IO.BinaryWriter $fs
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $s = $sizes[$i]; $d = $frames[$i]
    $b = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$b); $bw.Write([byte]$b); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$d.Length); $bw.Write([uint32]$offset)
    $offset += $d.Length
}
foreach ($d in $frames) { $bw.Write($d) }
$bw.Close()
Write-Output "Wrote $Out"
