# Draws the installer artwork (installer/banner.bmp 493x58, installer/dialog.bmp
# 493x312) and writes installer/license.rtf from the Vietnamese text below.
param([string]$Dir = (Join-Path $PSScriptRoot '..\installer'))
Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$indigo = [System.Drawing.Color]::FromArgb(99, 102, 241)
$violet = [System.Drawing.Color]::FromArgb(139, 92, 246)

function Draw-Book($g, [single]$x, [single]$y, [single]$s) {
    # Rounded tile with two pages, as in app.ico.
    $r = $s * 0.22
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $path.AddArc($x, $y, $r * 2, $r * 2, 180, 90)
    $path.AddArc($x + $s - $r * 2, $y, $r * 2, $r * 2, 270, 90)
    $path.AddArc($x + $s - $r * 2, $y + $s - $r * 2, $r * 2, $r * 2, 0, 90)
    $path.AddArc($x, $y + $s - $r * 2, $r * 2, $r * 2, 90, 90)
    $path.CloseFigure()
    $rect = New-Object System.Drawing.RectangleF $x, $y, $s, $s
    $g.FillPath((New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, $indigo, $violet, 45), $path)
    $cx = $x + $s / 2; $top = $y + $s * 0.27; $bot = $y + $s * 0.75; $e = $s * 0.18
    $pt = { param($a, $b) New-Object System.Drawing.PointF $a, $b }
    $g.FillPolygon([System.Drawing.Brushes]::White, [System.Drawing.PointF[]]@(
        (& $pt ($cx - $s*0.02) ($top + $s*0.05)), (& $pt ($x + $e) $top), (& $pt ($x + $e) ($bot - $s*0.05)), (& $pt ($cx - $s*0.02) $bot)))
    $g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(225, 228, 255))), [System.Drawing.PointF[]]@(
        (& $pt ($cx + $s*0.02) ($top + $s*0.05)), (& $pt ($x + $s - $e) $top), (& $pt ($x + $s - $e) ($bot - $s*0.05)), (& $pt ($cx + $s*0.02) $bot)))
}

function New-Canvas([int]$w, [int]$h) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::White)
    return $bmp, $g
}

# Welcome / finish page: the left 164 px carry the art, text sits on the right.
$bmp, $g = New-Canvas 493 312
$panel = New-Object System.Drawing.Rectangle 0, 0, 164, 312
$g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush $panel, ([System.Drawing.Color]::FromArgb(79, 70, 229)), ([System.Drawing.Color]::FromArgb(124, 58, 237)), 70), $panel)
$soft = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(28, 255, 255, 255))
$g.FillEllipse($soft, 70, -40, 150, 150)
$g.FillEllipse($soft, -60, 210, 160, 160)
Draw-Book $g 46 70 72
$fmt = New-Object System.Drawing.StringFormat
$fmt.Alignment = 'Center'
$g.DrawString('ZENIX', (New-Object System.Drawing.Font 'Segoe UI Semibold', 8.5), (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(200, 255, 255, 255))), (New-Object System.Drawing.RectangleF 0, 152, 164, 18), $fmt)
$g.DrawString('CrossPoint', (New-Object System.Drawing.Font 'Segoe UI Semibold', 15), [System.Drawing.Brushes]::White, (New-Object System.Drawing.RectangleF 0, 166, 164, 30), $fmt)
$g.DrawString('Reader', (New-Object System.Drawing.Font 'Segoe UI', 10.5), (New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(220, 255, 255, 255))), (New-Object System.Drawing.RectangleF 0, 194, 164, 24), $fmt)
$g.Dispose(); $bmp.Save((Join-Path $Dir 'dialog.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp); $bmp.Dispose()

# Banner across the top of the inner pages: title text on the left, tile on the right.
$bmp, $g = New-Canvas 493 58
$g.FillRectangle((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(246, 247, 251))), 0, 0, 493, 58)
Draw-Book $g 440 9 40
$g.Dispose(); $bmp.Save((Join-Path $Dir 'banner.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp); $bmp.Dispose()

# Terms shown on the welcome page. RTF carries Vietnamese as \uNNNN? escapes.
$paras = @(
    @('h', 'Zenix CrossPoint Reader cho Windows'),
    @('p', 'Ứng dụng đọc sách EPUB trên máy tính, kết nối với thư viện CrossPoint (crosspoint.zenix.vn) qua OPDS và nhận sách gửi từ web qua Wi-Fi, giống máy đọc X4.'),
    @('h', 'Dữ liệu và quyền riêng tư'),
    @('p', 'Sách, tiến độ đọc và dấu trang được lưu trên máy tính này, trong thư mục %LOCALAPPDATA%\CrossPointReader. Mật khẩu thư viện và khóa thiết bị được mã hóa bằng Windows (DPAPI). Ứng dụng chỉ kết nối tới máy chủ bạn chọn. Gỡ cài đặt không xóa thư viện sách của bạn.'),
    @('h', 'Giấy phép'),
    @('p', 'Phần mềm mã nguồn mở theo giấy phép MIT, © 2025 CrossPoint Reader organization. Phần mềm được cung cấp "nguyên trạng", không kèm bất kỳ bảo đảm nào. Có dùng epub.js (BSD-2-Clause), JSZip (MIT), Microsoft WebView2 và .NET (MIT).')
)
function Rtf([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $s.ToCharArray()) {
        $c = [int]$ch
        if ($ch -eq '\' -or $ch -eq '{' -or $ch -eq '}') { [void]$sb.Append('\' + $ch) }
        elseif ($c -gt 127) { [void]$sb.Append("\u$($c)?") }
        else { [void]$sb.Append($ch) }
    }
    $sb.ToString()
}
$body = foreach ($p in $paras) {
    if ($p[0] -eq 'h') { "\pard\sb120\sa60\b\fs20 $(Rtf $p[1])\b0\par" }
    else { "\pard\sa100\fs17 $(Rtf $p[1])\par" }
}
$rtf = "{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0\fswiss Segoe UI;}}\f0\fs17`r`n" + ($body -join "`r`n") + "`r`n}"
[System.IO.File]::WriteAllText((Join-Path $Dir 'license.rtf'), $rtf, [System.Text.Encoding]::ASCII)
Write-Output "Wrote banner.bmp, dialog.bmp, license.rtf in $Dir"
