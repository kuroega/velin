# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the original Velin mark as a multiresolution Windows icon.
# Geometry and colors match velin.svg. No third-party artwork is used.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$canvas = [System.Drawing.Bitmap]::new(1024, 1024, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($canvas)
$graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$graphics.Clear([System.Drawing.Color]::Transparent)
$graphics.ScaleTransform(4, 4)
$background = [System.Drawing.Drawing2D.GraphicsPath]::new()
$background.AddArc(8, 8, 120, 120, 180, 90)
$background.AddArc(128, 8, 120, 120, 270, 90)
$background.AddArc(128, 128, 120, 120, 0, 90)
$background.AddArc(8, 128, 120, 120, 90, 90)
$background.CloseFigure()
$brush = [System.Drawing.SolidBrush]::new([System.Drawing.ColorTranslator]::FromHtml('#c2afff'))
$graphics.FillPath($brush, $background)
$pen = [System.Drawing.Pen]::new([System.Drawing.ColorTranslator]::FromHtml('#211f2d'), 17)
$pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$pen.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round
$points = [System.Drawing.PointF[]]@(
    [System.Drawing.PointF]::new(70, 80),
    [System.Drawing.PointF]::new(126, 178),
    [System.Drawing.PointF]::new(187, 78)
)
$graphics.DrawLines($pen, $points)
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $image = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $resizer = [System.Drawing.Graphics]::FromImage($image)
    $resizer.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $resizer.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $resizer.DrawImage($canvas, 0, 0, $size, $size)
    $buffer = [IO.MemoryStream]::new()
    $image.Save($buffer, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += ,$buffer.ToArray()
    $buffer.Dispose()
    $resizer.Dispose()
    $image.Dispose()
}
$path = Join-Path $PSScriptRoot 'velin.ico'
$output = [IO.File]::Create($path)
$writer = [IO.BinaryWriter]::new($output)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; ++$i) {
    $dimension = if ($sizes[$i] -eq 256) { 0 } else { $sizes[$i] }
    $writer.Write([byte]$dimension)
    $writer.Write([byte]$dimension)
    $writer.Write([byte]0)
    $writer.Write([byte]0)
    $writer.Write([uint16]1)
    $writer.Write([uint16]32)
    $writer.Write([uint32]$images[$i].Length)
    $writer.Write([uint32]$offset)
    $offset += $images[$i].Length
}
foreach ($image in $images) { $writer.Write([byte[]]$image) }
$writer.Dispose()
$pen.Dispose()
$brush.Dispose()
$background.Dispose()
$graphics.Dispose()
$canvas.Dispose()
Write-Host "Generated $path"
