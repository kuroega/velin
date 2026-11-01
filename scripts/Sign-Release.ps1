# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string[]]$Files,
    [string]$SignTool,
    [string]$AzureDlib,
    [string]$AzureMetadata,
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [switch]$RequireSigning,
    [string]$ReportPath
)
$ErrorActionPreference = 'Stop'
$report = [ordered]@{ signed = $false; provider = 'none'; reason = ''; files = @() }
if (-not $env:VELIN_SIGN_PFX_BASE64 -and -not ($AzureDlib -and $AzureMetadata)) {
    $report.reason = 'No signing credentials were supplied. Artifacts are unsigned.'
    Write-Warning $report.reason
    if ($ReportPath) { $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
    if ($RequireSigning) { throw $report.reason }
    return
}
if (-not $SignTool) {
    $command = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($command) { $SignTool = $command.Source }
}
if (-not $SignTool) {
    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $SignTool = Get-ChildItem -LiteralPath $sdkRoot -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq 'x64' } | Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $SignTool) { throw 'SignTool is required when signing credentials are supplied.' }
$certificate = $null
$pfx = $null
try {
    $arguments = @('sign', '/fd', 'SHA256', '/tr', $TimestampUrl, '/td', 'SHA256')
    if ($env:VELIN_SIGN_PFX_BASE64) {
        $pfx = Join-Path ([IO.Path]::GetTempPath()) ('Velin-signing-' + [Guid]::NewGuid().ToString('N') + '.pfx')
        [IO.File]::WriteAllBytes($pfx, [Convert]::FromBase64String($env:VELIN_SIGN_PFX_BASE64))
        $password = ConvertTo-SecureString -String ([string]$env:VELIN_SIGN_PFX_PASSWORD) -AsPlainText -Force
        $certificate = Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\My -Password $password
        if (-not $certificate.HasPrivateKey) { throw 'The supplied certificate does not contain a private key.' }
        $arguments += @('/sha1', $certificate.Thumbprint)
        $report.provider = 'PFX'
    } else {
        if (-not (Test-Path -LiteralPath $AzureDlib) -or -not (Test-Path -LiteralPath $AzureMetadata)) { throw 'Azure signing Dlib and metadata paths must exist.' }
        $arguments += @('/dlib', [IO.Path]::GetFullPath($AzureDlib), '/dmdf', [IO.Path]::GetFullPath($AzureMetadata))
        $report.provider = 'AzureTrustedSigning'
    }
    foreach ($file in $Files) {
        $resolved = (Resolve-Path -LiteralPath $file).Path
        & $SignTool @arguments $resolved
        if ($LASTEXITCODE -ne 0) { throw "SignTool failed for $resolved." }
        & $SignTool verify /pa /all $resolved
        if ($LASTEXITCODE -ne 0) { throw "Authenticode validation failed for $resolved." }
        $signature = Get-AuthenticodeSignature -LiteralPath $resolved
        if ($signature.Status -ne 'Valid') { throw "The signed artifact did not validate: $resolved" }
        $report.files += [ordered]@{ name = [IO.Path]::GetFileName($resolved); signer = $signature.SignerCertificate.Thumbprint }
    }
    $report.signed = $true
} finally {
    if ($pfx -and (Test-Path -LiteralPath $pfx)) { Remove-Item -LiteralPath $pfx -Force }
    if ($certificate) { Remove-Item -LiteralPath ('Cert:\CurrentUser\My\' + $certificate.Thumbprint) -DeleteKey -Force }
    if ($ReportPath) { $report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $ReportPath -Encoding UTF8 }
}
