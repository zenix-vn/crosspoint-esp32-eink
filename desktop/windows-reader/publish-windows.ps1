# Publishes a Windows release to the update feed the app checks:
#   https://crosspoint.zenix.vn/windows/CrossPointReader-Setup-<version>.msi
#   https://crosspoint.zenix.vn/windows/latest.json   (written last, so the feed
#                                                     never points at a missing file)
# Like scripts/publish_ota.py for the firmware: same host and SSH port.
#
#   pwsh -File publish-windows.ps1 -Bump -Notes "Tra từ khi đọc; gửi sách sang X4"
#   pwsh -File publish-windows.ps1 -Bump -NotesFile notes.txt   # notes from a UTF-8 file (safe from any shell)
#   pwsh -File publish-windows.ps1 -DryRun          # build and write latest.json, no upload
param(
    [switch]$Bump,
    [string]$Notes = '',
    [string]$NotesFile = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if ($NotesFile) { $Notes = [IO.File]::ReadAllText((Resolve-Path $NotesFile), [Text.Encoding]::UTF8).Trim() }

$SshHost = 'zenix-server'   # ~/.ssh/config alias, as in publish_ota.py
$SshPort = '8430'
$Remote = '/usr/local/lsws/crosspoint.zenix.vn/html/windows'
$Keep = 5

$proj = Join-Path $PSScriptRoot 'CrossPointReader.csproj'
$text = [IO.File]::ReadAllText($proj)
if ($text -notmatch '<Version>(\d+)\.(\d+)\.(\d+)</Version>') { throw 'No <Version>X.Y.Z</Version> in the csproj' }
$version = "$($Matches[1]).$($Matches[2]).$($Matches[3])"
if ($Bump) {
    $new = "$($Matches[1]).$($Matches[2]).$([int]$Matches[3] + 1)"
    [IO.File]::WriteAllText($proj, $text.Replace("<Version>$version</Version>", "<Version>$new</Version>"))
    Write-Output "Version $version -> $new"
    $version = $new
}

& (Join-Path $PSScriptRoot 'build-installer.ps1') | Out-Host
$msi = Join-Path $PSScriptRoot "dist\CrossPointReader-Setup-$version.msi"
if (-not (Test-Path $msi)) { throw "Missing $msi" }

$manifest = [ordered]@{
    version = $version
    url     = (Split-Path $msi -Leaf)
    sha256  = (Get-FileHash $msi -Algorithm SHA256).Hash.ToLowerInvariant()
    size    = (Get-Item $msi).Length
    notes   = $Notes
    date    = (Get-Date).ToString('yyyy-MM-dd')
}
$json = Join-Path $PSScriptRoot 'dist\latest.json'
[IO.File]::WriteAllText($json, ($manifest | ConvertTo-Json), (New-Object Text.UTF8Encoding $false))
Get-Content $json

if ($DryRun) { Write-Output 'Dry run: nothing uploaded.'; return }

ssh -p $SshPort $SshHost "mkdir -p $Remote"
scp -q -P $SshPort $msi "${SshHost}:$Remote/"
if ($LASTEXITCODE) { throw 'Upload of the installer failed' }
scp -q -P $SshPort $json "${SshHost}:$Remote/latest.json.part"
if ($LASTEXITCODE) { throw 'Upload of latest.json failed' }
# The manifest goes live last; older installers beyond the last $Keep are removed.
ssh -p $SshPort $SshHost "set -e; cd $Remote && mv latest.json.part latest.json && ls -1t CrossPointReader-Setup-*.msi | tail -n +$($Keep + 1) | xargs -r rm -f; chown -R nobody:nobody $Remote; chmod 644 $Remote/*"
Write-Output "Published ${version}: https://crosspoint.zenix.vn/windows/latest.json"
