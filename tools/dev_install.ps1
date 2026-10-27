# Developer install of build\xlive.dll into the Lost Planet 2 folder (players use LP2NetSetup.exe).
#   dev_install.ps1            install / update
#   dev_install.ps1 -Uninstall restore Capcom's stub
param([switch]$Uninstall, [string]$GameDir)

$ErrorActionPreference = 'Stop'
$capcomSha = '867BFE461A0562AEE0A43CD832A52B228AC6DA4E4216BDFBB94EF14BECE85366'
if (-not $GameDir) {
    $GameDir = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Capcom\LostPlanet 2').installdir
}
$xlive = Join-Path $GameDir 'xlive.dll'
$capcom = Join-Path $GameDir 'xlive_capcom.dll'

if ($Uninstall) {
    if (Test-Path $capcom) {
        Remove-Item $xlive -Force -ErrorAction SilentlyContinue
        Move-Item $capcom $xlive
        "Restored Capcom's xlive.dll"
    } else {
        'Nothing to uninstall (no xlive_capcom.dll)'
    }
    return
}

if (-not (Test-Path $capcom)) {
    $sha = (Get-FileHash $xlive -Algorithm SHA256).Hash
    if ($sha -ne $capcomSha) {
        throw "xlive.dll in $GameDir is not Capcom's 2025 stub (sha $sha); refusing to touch it"
    }
    Move-Item $xlive $capcom
    "Kept Capcom's stub as xlive_capcom.dll"
}
$built = Join-Path $PSScriptRoot '..\build\xlive.dll'
Copy-Item $built $xlive -Force
"Installed LP2Net xlive.dll ($((Get-Item $xlive).Length) bytes) into $GameDir"
