# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^\d+\.\d+\.\d+$')][string]$Version,
    [Parameter(Mandatory)][string]$BaseUrl,
    [Parameter(Mandatory)][string[]]$Artifacts,
    [Parameter(Mandatory)][string]$Output
)
$ErrorActionPreference = 'Stop'
$uri = [Uri]$BaseUrl
if ($uri.Scheme -ne 'https' -or $uri.UserInfo -or $uri.Query -or $uri.Fragment) { throw 'BaseUrl must be a public HTTPS release directory URL.' }
$manifest = [ordered]@{ version = $Version; assets = @() }
foreach ($artifact in $Artifacts) {
    $file = Get-Item -LiteralPath $artifact
    $kind = if ($file.Name.EndsWith('-setup.exe')) { 'installer' } elseif ($file.Name.EndsWith('-source.zip')) { 'source' } else { 'portable' }
    $manifest.assets += [ordered]@{ name = $file.Name; kind = $kind; url = $BaseUrl.TrimEnd('/') + '/' + [Uri]::EscapeDataString($file.Name);
        sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$portable = $manifest.assets | Where-Object { $_.kind -eq 'portable' } | Select-Object -First 1
if ($portable) { $manifest.url = $portable.url; $manifest.sha256 = $portable.sha256 }
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Output -Encoding UTF8
