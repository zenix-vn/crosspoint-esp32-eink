# Builds dist/CrossPointReader-Setup-<version>.msi: publishes the single-file
# exe into build/app (never the publish/ folder you may be running from), draws
# the installer art, then builds the MSI with WiX (restored from NuGet).
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

dotnet publish -c Release -o build\app
if ($LASTEXITCODE) { throw 'publish failed' }

& (Join-Path $PSScriptRoot 'tools\make-installer-art.ps1')
dotnet build installer\CrossPointReader.Installer.wixproj -c Release
if ($LASTEXITCODE) { throw 'installer build failed' }

$version = (Get-Item build\app\CrossPointReader.exe).VersionInfo.ProductVersion.Split('+')[0]
$msi = "dist\CrossPointReader-Setup-$version.msi"
Copy-Item dist\vi-VN\CrossPointReader-Setup.msi $msi -Force
Get-Item $msi | Select-Object FullName, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }