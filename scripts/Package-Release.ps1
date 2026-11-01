# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/msvc-release',
    [string]$Configuration = 'Release',
    [string]$Version,
    [string]$ReleaseBaseUrl,
    [switch]$SkipInstaller,
    [switch]$RequireSigning
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDirectory)) { $BuildDirectory = Join-Path $root $BuildDirectory }
if (-not $Version) {
    $project = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    $Version = [regex]::Match($project, 'project\(Velin VERSION ([0-9]+\.[0-9]+\.[0-9]+)').Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw 'A semantic application version is required.' }
foreach ($file in @('libmpv-2.dll', 'ffmpeg.exe', 'yt-dlp.exe', 'WebView2Loader.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $root "runtime\$file"))) { throw "The complete release requires runtime/$file. Run the runtime acquisition scripts first." }
}
$release = Join-Path $BuildDirectory 'artifacts'
$run = Join-Path $root ('.cache\release\' + [Guid]::NewGuid().ToString('N'))
$stage = Join-Path $run 'payload'
New-Item -ItemType Directory -Force -Path $release, $stage | Out-Null
& cmake --install $BuildDirectory --config $Configuration --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'CMake release staging failed.' }
foreach ($file in @('velin.exe', 'velin-cli.exe', 'libmpv-2.dll', 'ffmpeg.exe', 'yt-dlp.exe', 'WebView2Loader.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $file))) { throw "Missing release payload: $file" }
}
& (Join-Path $PSScriptRoot 'Sign-Release.ps1') -Files @((Join-Path $stage 'velin.exe'), (Join-Path $stage 'velin-cli.exe')) `
    -RequireSigning:$RequireSigning -ReportPath (Join-Path $release 'application-signing.json')

$payloadFiles = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
$fileManifest = [ordered]@{ version = $Version; architecture = 'x86_64'; files = @() }
foreach ($file in $payloadFiles) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
    $fileManifest.files += [ordered]@{ name = $relative; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$fileManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'release-file-manifest.json') -Encoding UTF8
$payloadFiles = @(Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName)
$portable = Join-Path $release "Velin-$Version-windows-x64.zip"
if (Test-Path -LiteralPath $portable) { Remove-Item -LiteralPath $portable -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $portable, [IO.Compression.CompressionLevel]::Optimal, $false)

$installer = $null
if (-not $SkipInstaller) {
    $toolManifest = (Get-Content -LiteralPath (Join-Path $root 'config\installer-tools.json') -Raw | ConvertFrom-Json).nsis
    $makensis = Join-Path $root ('.cache\tools\nsis-' + $toolManifest.version + '\makensis.exe')
    if (-not (Test-Path -LiteralPath $makensis)) { throw 'Run scripts/Get-InstallerTools.ps1 before building the installer.' }
    $installInclude = Join-Path $run 'payload-install.nsh'
    $uninstallInclude = Join-Path $run 'payload-uninstall.nsh'
    function Escape-Nsis([string]$Value) { return $Value.Replace('$', '$$').Replace('"', '$\"') }
    $installLines = [Collections.Generic.List[string]]::new()
    $uninstallLines = [Collections.Generic.List[string]]::new()
    foreach ($file in $payloadFiles) {
        $relative = $file.FullName.Substring($stage.Length + 1)
        $directory = Split-Path -Parent $relative
        $out = if ($directory) { '$INSTDIR\' + (Escape-Nsis $directory) } else { '$INSTDIR' }
        $installLines.Add('SetOutPath "' + $out + '"')
        $installLines.Add('File "' + (Escape-Nsis $file.FullName) + '"')
        $uninstallLines.Add('Delete "$INSTDIR\' + (Escape-Nsis $relative) + '"')
    }
    $directories = Get-ChildItem -LiteralPath $stage -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending
    foreach ($directory in $directories) {
        $relative = $directory.FullName.Substring($stage.Length + 1)
        $uninstallLines.Add('RMDir "$INSTDIR\' + (Escape-Nsis $relative) + '"')
    }
    $installLines | Set-Content -LiteralPath $installInclude -Encoding UTF8
    $uninstallLines | Set-Content -LiteralPath $uninstallInclude -Encoding UTF8
    $associations = @('.mkv','.mp4','.webm','.avi','.mov','.m4v','.mp3','.flac','.wav','.ogg','.m4a','.opus','.m3u','.m3u8','.pls')
    $associations | ForEach-Object {
        'WriteRegBin HKCU "Software\Classes\' + $_ + '\OpenWithProgids" "Velin.Media" ""'
        'WriteRegStr HKCU "Software\Velin\Capabilities\FileAssociations" "' + $_ + '" "Velin.Media"'
    } | Set-Content -LiteralPath "$installInclude.associations" -Encoding UTF8
    $associations | ForEach-Object {
        'DeleteRegValue HKCU "Software\Classes\' + $_ + '\OpenWithProgids" "Velin.Media"'
    } | Set-Content -LiteralPath "$uninstallInclude.associations" -Encoding UTF8
    $installer = Join-Path $release "Velin-$Version-windows-x64-setup.exe"
    Push-Location (Join-Path $root 'installer')
    try {
        $nsisArguments = @('/V3', "/DVERSION=$Version", "/DSTAGE=$stage", "/DOUTPUT=$installer",
            "/DPAYLOAD_INSTALL=$installInclude", "/DPAYLOAD_UNINSTALL=$uninstallInclude", 'Velin.nsi') |
            ForEach-Object { '"' + $_ + '"' }
        $nsisLog = Join-Path $run 'nsis.stdout.log'
        $nsisErrors = Join-Path $run 'nsis.stderr.log'
        $compiler = Start-Process -FilePath $makensis -ArgumentList $nsisArguments -WindowStyle Hidden `
            -Wait -PassThru -RedirectStandardOutput $nsisLog -RedirectStandardError $nsisErrors
        Get-Content -LiteralPath $nsisLog
        if ($compiler.ExitCode -ne 0) {
            Get-Content -LiteralPath $nsisErrors | Write-Output
            throw 'NSIS installer compilation failed.'
        }
    } finally { Pop-Location }
    & (Join-Path $PSScriptRoot 'Sign-Release.ps1') -Files @($installer) -RequireSigning:$RequireSigning `
        -ReportPath (Join-Path $release 'installer-signing.json')
}

& cpack --config (Join-Path $BuildDirectory 'CPackSourceConfig.cmake') -B $release
if ($LASTEXITCODE -ne 0) { throw 'Source packaging failed.' }
$assets = @($portable, (Join-Path $release "Velin-$Version-source.zip"))
if ($installer) { $assets += $installer }
$releaseManifest = [ordered]@{ version = $Version; assets = @() }
foreach ($asset in $assets) {
    $name = [IO.Path]::GetFileName($asset)
    $url = if ($ReleaseBaseUrl) { $ReleaseBaseUrl.TrimEnd('/') + '/' + $name } else { '' }
    $kind = if ($name.EndsWith('-setup.exe')) { 'installer' } elseif ($name.EndsWith('-source.zip')) { 'source' } else { 'portable' }
    $releaseManifest.assets += [ordered]@{ name = $name; kind = $kind; url = $url; sha256 = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$releaseManifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $release 'releases.json') -Encoding UTF8
$releaseManifest.assets | ForEach-Object { $_.sha256 + '  ' + $_.name } | Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS') -Encoding ascii
Write-Output "Release artifacts: $release"
