# SPDX-License-Identifier: GPL-3.0-or-later
# Regenerates the Velin "Scan" mark as a multiresolution Windows icon:
# scanlines whose lengths trace a play shape, each fading from deep indigo
# into a lavender-to-sky tint. Geometry and colors match velin.svg; sizes of
# 32 px and below use a bolder five-line cut that stays legible on the
# taskbar. No third-party artwork is used.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$indigo = [System.Drawing.Color]::FromArgb(0x4b, 0x47, 0xe8)
$lavender = [System.Drawing.Color]::FromArgb(0x9d, 0x8c, 0xff)
$sky = [System.Drawing.Color]::FromArgb(0x2f, 0xa7, 0xff)

function Mix-Color([System.Drawing.Color]$From, [System.Drawing.Color]$To, [double]$Amount) {
    [System.Drawing.Color]::FromArgb(
        [int][Math]::Round($From.R + ($To.R - $From.R) * $Amount),
        [int][Math]::Round($From.G + ($To.G - $From.G) * $Amount),
        [int][Math]::Round($From.B + ($To.B - $From.B) * $Amount))
}

# Draws the mark on a 1024 px canvas using the 256-unit geometry of velin.svg.
function New-Mark([int]$Count, [double]$Step, [double]$Thickness, [double]$Left, [double]$Span, [double]$Reach) {
    $canvas = [System.Drawing.Bitmap]::new(1024, 1024, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $graphics = [System.Drawing.Graphics]::FromImage($canvas)
    $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $graphics.Clear([System.Drawing.Color]::Transparent)
    $graphics.ScaleTransform(4, 4)
    $middle = ($Count - 1) / 2
    for ($i = 0; $i -lt $Count; ++$i) {
        $y = 128 + ($i - $middle) * $Step
        $right = $Left + $Span * (1 - [Math]::Abs($y - 128) / $Reach)
        $x = [single]($Left - $Thickness / 2)
        $top = [single]($y - $Thickness / 2)
        $width = [single]($right - $Left + $Thickness)
        $size = [single]$Thickness
        $line = [System.Drawing.Drawing2D.GraphicsPath]::new()
        $line.AddArc($x, $top, $size, $size, [single]90, [single]180)
        $line.AddArc([single]($x + $width - $size), $top, $size, $size, [single]270, [single]180)
        $line.CloseFigure()
        $bounds = [System.Drawing.RectangleF]::new($x, $top, $width, $size)
        $tint = Mix-Color $lavender $sky ($i / ($Count - 1))
        $brush = [System.Drawing.Drawing2D.LinearGradientBrush]::new($bounds, $indigo, $tint,
            [System.Drawing.Drawing2D.LinearGradientMode]::Horizontal)
        # Flipping the tile keeps GDI+ from wrapping a sliver of indigo onto the far cap.
        $brush.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipX
        $graphics.FillPath($brush, $line)
        $brush.Dispose()
        $line.Dispose()
    }
    $graphics.Dispose()
    return $canvas
}

$detailed = New-Mark -Count 9 -Step 19 -Thickness 12 -Left 42 -Span 170 -Reach 90
$bold = New-Mark -Count 5 -Step 36 -Thickness 24 -Left 44 -Span 166 -Reach 92
$sizes = @(16, 24, 32, 48, 64, 128, 256)
$images = @()
foreach ($size in $sizes) {
    $source = if ($size -le 32) { $bold } else { $detailed }
    $image = [System.Drawing.Bitmap]::new($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $resizer = [System.Drawing.Graphics]::FromImage($image)
    $resizer.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $resizer.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $resizer.DrawImage($source, 0, 0, $size, $size)
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
$detailed.Dispose()
$bold.Dispose()
Write-Host "Generated $path"
