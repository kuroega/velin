# SPDX-License-Identifier: GPL-3.0-or-later
# Dot-source this file to use the common hash-pinned acquisition helpers.
function Get-VerifiedDownload {
    param([Parameter(Mandatory)][string]$Url,
          [Parameter(Mandatory)][string]$Destination,
          [Parameter(Mandatory)][string]$Sha256)
    if ((Test-Path -LiteralPath $Destination -PathType Leaf) -and
        (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash -eq $Sha256) { return }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    $partial = "$Destination.partial"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $partial -UseBasicParsing
    $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
    # SourceForge can return a publisher HTML download page before the archive.
    if ($actual -ne $Sha256 -and ([Uri]$Url).Host -eq 'sourceforge.net') {
        $html = Get-Content -LiteralPath $partial -Raw
        $redirect = [regex]::Match($html, '<meta http-equiv="refresh" content="[0-9]+; url=([^"]+)"')
        if ($redirect.Success) {
            $next = [Net.WebUtility]::HtmlDecode($redirect.Groups[1].Value)
            $uri = [Uri]$next
            if ($uri.Scheme -ne 'https' -or $uri.Host -ne 'downloads.sourceforge.net') {
                throw 'Unexpected SourceForge download redirect.'
            }
            Invoke-WebRequest -Uri $next -OutFile $partial -UseBasicParsing
            $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash
        }
    }
    if ($actual -ne $Sha256) { throw "SHA256 mismatch for $Destination. Expected $Sha256, got $actual." }
    Move-Item -LiteralPath $partial -Destination $Destination -Force
}

function Find-VelinSevenZip {
    param([string]$Path)
    if ($Path -and (Test-Path -LiteralPath $Path)) { return $Path }
    $command = Get-Command 7z.exe -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
        (Join-Path $env:ProgramFiles 'EibolSoft\FFmpeg Batch AV Converter\7z.exe'))) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    throw '7-Zip is required. Install it or pass -SevenZipPath.'
}
