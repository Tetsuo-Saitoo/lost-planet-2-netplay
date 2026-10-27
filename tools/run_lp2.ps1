# Start Lost Planet 2 directly (no Launcher window), the way Steam would, and print the PID.
#   run_lp2.ps1 [-Dx11]
# Steam must be running and own the game; SteamAppId is what Steam itself sets for games it starts.
param([switch]$Dx11, [string]$GameDir)

if (-not $GameDir) {
    $GameDir = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Capcom\LostPlanet 2').installdir
}
$env:SteamAppId = '45750'
$env:SteamGameId = '45750'
$exe = if ($Dx11) { 'LP2DX11.exe' } else { 'LP2DX9.exe' }
$p = Start-Process -FilePath (Join-Path $GameDir $exe) -WorkingDirectory $GameDir -PassThru
$p.Id
