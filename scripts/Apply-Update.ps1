# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Archive,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedSha256,
    [Parameter(Mandatory)][string]$TargetDirectory,
    [int]$WaitProcessId = 0,
    [string]$RequiredSignerThumbprint,
    [switch]$Restart,
    [string]$RestartArgumentsJson = '[]'
)
$ErrorActionPreference = 'Stop'
$Archive = (Resolve-Path -LiteralPath $Archive).Path
$TargetDirectory = (Resolve-Path -LiteralPath $TargetDirectory).Path.TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $TargetDirectory 'velin.exe') -PathType Leaf)) { throw 'The target directory is not an existing Velin installation.' }
if ((Get-Item -LiteralPath $TargetDirectory).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Update targets cannot be reparse points.' }
if ((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash -ne $ExpectedSha256) { throw 'Update archive SHA256 verification failed.' }
$restartPayload = ConvertFrom-Json ('{"arguments":' + $RestartArgumentsJson + '}')
$restartArguments = $restartPayload.arguments
if (@($restartPayload.PSObject.Properties).Count -ne 1 -or $restartArguments -isnot [array] -or $restartArguments.Count -gt 256 -or @($restartArguments | Where-Object { $_ -isnot [string] }).Count) { throw 'RestartArgumentsJson must be an array of strings.' }
$parent = Split-Path -Parent $TargetDirectory
$operation = [Guid]::NewGuid().ToString('N')
$staging = Join-Path $parent ('.velin-update-stage-' + $operation)
$backup = Join-Path $parent ('.velin-update-backup-' + $operation)
$lockPath = Join-Path $TargetDirectory '.velin-update.lock'
$lock = $null
$installed = [Collections.Generic.List[string]]::new()
$originals = [Collections.Generic.List[string]]::new()
$committed = $false
$rolledBack = $false
$pendingCopy = $null

function Remove-UpdateScratch([string]$Path) {
    $resolved = [IO.Path]::GetFullPath($Path)
    if ((Split-Path -Parent $resolved) -ne $parent -or
        (Split-Path -Leaf $resolved) -notmatch '^\.velin-update-(stage|backup)-[a-f0-9]{32}$') {
        throw 'Unexpected update scratch directory.'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
function Assert-NoReparseParent([string]$Path) {
    $directory = Split-Path -Parent $Path
    while ($directory.Length -ge $TargetDirectory.Length) {
        if ((Test-Path -LiteralPath $directory) -and
            ((Get-Item -LiteralPath $directory).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'An update destination contains a reparse point.'
        }
        if ($directory -eq $TargetDirectory) { break }
        $directory = Split-Path -Parent $directory
    }
}
try {
    $lock = [IO.File]::Open($lockPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    New-Item -ItemType Directory -Path $staging, $backup | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $package = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($package.Entries.Count -gt 10000) { throw 'Update archive contains too many files.' }
        $total = [long]0
        foreach ($entry in $package.Entries) {
            $total += $entry.Length
            if ($total -gt 2GB -or $entry.Length -gt 512MB) { throw 'Update archive exceeds the extraction size limit.' }
            $name = $entry.FullName.Replace('/', '\')
            if ($name.Contains(':') -or $name.StartsWith('\')) { throw 'Update archive contains an absolute path.' }
            $destination = [IO.Path]::GetFullPath((Join-Path $staging $name))
            if (-not $destination.StartsWith($staging + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Update archive contains a path traversal.' }
            if (-not $entry.Name) { New-Item -ItemType Directory -Force -Path $destination | Out-Null; continue }
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $false)
        }
    } finally { $package.Dispose() }
    $payload = $staging
    if (-not (Test-Path -LiteralPath (Join-Path $payload 'velin.exe'))) {
        $directories = @(Get-ChildItem -LiteralPath $staging -Directory)
        if ($directories.Count -ne 1) { throw 'Update archive does not contain a single Velin payload.' }
        $payload = $directories[0].FullName
    }
    $manifestPath = Join-Path $payload 'release-file-manifest.json'
    if (-not (Test-Path -LiteralPath $manifestPath)) { throw 'Update archive is missing its file manifest.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $verified = @{}
    foreach ($file in $manifest.files) {
        if ($file.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $file.name -notmatch '^[a-zA-Z0-9_ ./-]+$' -or $file.name.Contains('..')) { throw 'Invalid update file manifest.' }
        $path = [IO.Path]::GetFullPath((Join-Path $payload $file.name))
        if (-not $path.StartsWith($payload + '\', [StringComparison]::OrdinalIgnoreCase) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { throw "Update file verification failed: $($file.name)" }
        $verified[$file.name.Replace('/', '\').ToLowerInvariant()] = $true
    }
    foreach ($required in @('velin.exe', 'velin-cli.exe', 'libmpv-2.dll')) { if (-not $verified.ContainsKey($required)) { throw "Missing update component: $required" } }
    foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse) {
        $relative = $file.FullName.Substring($payload.Length + 1)
        if ($relative -ne 'release-file-manifest.json' -and -not $verified.ContainsKey($relative.ToLowerInvariant())) { throw "Unlisted update file: $relative" }
        if ([IO.Path]::GetExtension($relative) -in @('.exe', '.dll') -and $relative.Contains('\')) { throw 'Executable update files must be in the application root.' }
    }
    if ($RequiredSignerThumbprint) {
        foreach ($name in @('velin.exe', 'velin-cli.exe')) {
            $signature = Get-AuthenticodeSignature -LiteralPath (Join-Path $payload $name)
            if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $RequiredSignerThumbprint) { throw 'The application update signer does not match the required signer.' }
        }
    }
    if ($WaitProcessId -gt 0) {
        if ($WaitProcessId -eq $PID) { throw 'The updater cannot wait for its own process.' }
        $process = Get-Process -Id $WaitProcessId -ErrorAction SilentlyContinue
        if ($process -and -not $process.WaitForExit(120000)) { throw 'The player did not close within two minutes.' }
    }
    foreach ($file in Get-ChildItem -LiteralPath $payload -File -Recurse) {
        $relative = $file.FullName.Substring($payload.Length + 1)
        $destination = Join-Path $TargetDirectory $relative
        Assert-NoReparseParent $destination
        if (Test-Path -LiteralPath $destination) {
            if ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'An update destination is a reparse point.' }
            $saved = Join-Path $backup $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved) | Out-Null
            Copy-Item -LiteralPath $destination -Destination $saved
            $originals.Add($relative)
        }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        if (Test-Path -LiteralPath ($destination + '.velin-new')) { throw 'An update staging destination already exists.' }
        $pendingCopy = $destination + '.velin-new'
        Copy-Item -LiteralPath $file.FullName -Destination $pendingCopy -Force
        Move-Item -LiteralPath ($destination + '.velin-new') -Destination $destination -Force
        $pendingCopy = $null
        $installed.Add($relative)
    }
    $committed = $true
    Write-Output 'The verified portable update was installed successfully.'
} catch {
    foreach ($relative in $installed) {
        $destination = Join-Path $TargetDirectory $relative
        if ($originals.Contains($relative)) { Copy-Item -LiteralPath (Join-Path $backup $relative) -Destination $destination -Force }
        elseif (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
    }
    $rolledBack = $true
    throw
} finally {
    if ($pendingCopy -and (Test-Path -LiteralPath $pendingCopy -PathType Leaf)) { Remove-Item -LiteralPath $pendingCopy -Force }
    if ($lock) { $lock.Dispose(); Remove-Item -LiteralPath $lockPath -Force }
    Remove-UpdateScratch $staging
    if ($committed -or $rolledBack) { Remove-UpdateScratch $backup }
    elseif ((Test-Path -LiteralPath $backup) -and -not (Get-ChildItem -LiteralPath $backup -Force)) { Remove-UpdateScratch $backup }
}
if ($Restart -and $committed) {
    function Quote-Argument([string]$Value) { return '"' + [regex]::Replace([regex]::Replace($Value, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"' }
    $command = ($restartArguments | ForEach-Object { Quote-Argument $_ }) -join ' '
    $options = @{ FilePath = (Join-Path $TargetDirectory 'velin.exe'); WorkingDirectory = $TargetDirectory; WindowStyle = 'Hidden' }
    if ($command) { $options.ArgumentList = $command }
    Start-Process @options | Out-Null
}
