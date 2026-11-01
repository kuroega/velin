# SPDX-License-Identifier: GPL-3.0-or-later
# Invoked only when the user selects the optional WebView2 installer component.
$ErrorActionPreference = 'Stop'
$bootstrapper = Join-Path ([IO.Path]::GetTempPath()) ('Velin-WebView2-' + [Guid]::NewGuid().ToString('N') + '.exe')
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' -OutFile $bootstrapper -UseBasicParsing
    $signature = Get-AuthenticodeSignature -LiteralPath $bootstrapper
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw 'The WebView2 bootstrapper does not have a valid Microsoft Authenticode signature.'
    }
    $process = Start-Process -FilePath $bootstrapper -ArgumentList '/silent','/install' -Wait -PassThru -WindowStyle Hidden
    if ($process.ExitCode -ne 0) { throw "WebView2 runtime installer returned $($process.ExitCode)." }
} finally {
    if (Test-Path -LiteralPath $bootstrapper) { Remove-Item -LiteralPath $bootstrapper -Force }
}
