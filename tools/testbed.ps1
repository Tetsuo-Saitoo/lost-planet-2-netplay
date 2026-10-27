# Shared test bed for Lost Planet 2. Several agents build LP2 Netplay at once (each in its own git worktree), but
# this PC has one game install, one screen and one keyboard. Take the lock before installing a DLL into the game,
# starting the game or driving its window, and release it when done. Release puts back the known-good xlive.dll
# (the build the user plays with) and lp2net.ini / config.ini as they were at Acquire. Quit your game copies first.
#
#   testbed.ps1 Status
#   testbed.ps1 Acquire -Owner <session> -Purpose "voice test"   exit 1 if taken, or if the game runs unlocked
#   testbed.ps1 Wait    -Owner <session> -Purpose "..."          retries Acquire every 30 s (run it in background)
#   testbed.ps1 Release -Owner <session>
#   testbed.ps1 SetKnownGood -Dll <path>                         coordinator only
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('Status', 'Acquire', 'Wait', 'Release', 'SetKnownGood')] [string]$Action,
    [string]$Owner,
    [string]$Purpose = '',
    [string]$Dll,
    [int]$WaitMinutes = 240,
    [int]$StaleHours = 4,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$capcomSha = '867BFE461A0562AEE0A43CD832A52B228AC6DA4E4216BDFBB94EF14BECE85366'
$state = Join-Path $env:LOCALAPPDATA 'LP2Netplay\testbed'
$lock = Join-Path $state 'game.lock'
$snap = Join-Path $state 'snapshot'
$knownGood = Join-Path $state 'known-good\xlive.dll'
$game = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Capcom\LostPlanet 2').installdir
$docs = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'CAPCOM\LOST PLANET 2'
$settings = @(
    @{ Live = Join-Path $game 'lp2net.ini'; Name = 'lp2net.ini' },
    @{ Live = Join-Path $docs 'config.ini'; Name = 'config.ini' }
)
New-Item -ItemType Directory -Force $state, $snap, (Split-Path $knownGood) | Out-Null

function Get-Holder { if (Test-Path $lock) { Get-Content $lock -Raw | ConvertFrom-Json } }
function Get-Games { @(Get-Process lp2dx9, lp2dx11 -ErrorAction SilentlyContinue) }
function Need-Owner { if (-not $Owner) { throw "-Owner <your session name> is required" } }

# Returns $null on success, else why the lock was not taken.
function Try-Acquire {
    $h = Get-Holder
    if ($h) {
        $age = (Get-Date) - [datetime]$h.since
        if ($h.owner -eq $Owner) { return $null }
        if (-not ($Force -and $age.TotalHours -ge $StaleHours)) {
            return "taken by $($h.owner) since $($h.since): $($h.purpose)"
        }
        Remove-Item $lock
    }
    $g = Get-Games
    if ($g.Count) { return "the game is running (pid $($g.Id -join ', ')) and nobody holds the lock: the user may be playing" }
    try { $fs = [IO.File]::Open($lock, 'CreateNew', 'Write', 'None') } catch { return 'taken by another session just now' }
    $json = [ordered]@{ owner = $Owner; purpose = $Purpose; since = (Get-Date).ToString('s') } | ConvertTo-Json -Compress
    $bytes = [Text.Encoding]::UTF8.GetBytes($json)
    $fs.Write($bytes, 0, $bytes.Length)
    $fs.Close()
    foreach ($s in $settings) {
        $b = Join-Path $snap $s.Name
        if (Test-Path $s.Live) { Copy-Item $s.Live $b -Force } else { Remove-Item $b -ErrorAction SilentlyContinue }
    }
    return $null
}

switch ($Action) {
    'Status' {
        $h = Get-Holder
        if ($h) { "locked by $($h.owner) since $($h.since): $($h.purpose)" } else { 'free' }
        $g = Get-Games
        if ($g.Count) { "game running: pid $($g.Id -join ', ')" }
        $live = (Get-FileHash (Join-Path $game 'xlive.dll')).Hash
        $kg = if (Test-Path $knownGood) { (Get-FileHash $knownGood).Hash } else { '' }
        if ($live -eq $kg) { 'installed xlive.dll: known-good build' }
        elseif ($live -eq $capcomSha) { "installed xlive.dll: Capcom's stub (mod not installed)" }
        else { "installed xlive.dll: a test build ($($live.Substring(0, 12)))" }
    }
    'Acquire' {
        Need-Owner
        $why = Try-Acquire
        if ($why) { Write-Output "not acquired: $why"; exit 1 }
        "acquired by $Owner"
    }
    'Wait' {
        Need-Owner
        $deadline = (Get-Date).AddMinutes($WaitMinutes)
        while ($true) {
            $why = Try-Acquire
            if (-not $why) { "acquired by $Owner"; break }
            if ((Get-Date) -gt $deadline) { Write-Output "gave up after $WaitMinutes min: $why"; exit 1 }
            Start-Sleep -Seconds 30
        }
    }
    'Release' {
        Need-Owner
        $h = Get-Holder
        if (-not $h) { 'not locked'; break }
        if ($h.owner -ne $Owner -and -not $Force) { throw "held by $($h.owner), not $Owner" }
        $g = Get-Games
        if ($g.Count) { throw "quit the game copies first (pid $($g.Id -join ', '))" }
        if (Test-Path $knownGood) {
            $xlive = Join-Path $game 'xlive.dll'
            $capcom = Join-Path $game 'xlive_capcom.dll'
            if (-not (Test-Path $capcom)) {
                if ((Get-FileHash $xlive).Hash -ne $capcomSha) { throw "xlive_capcom.dll is missing and xlive.dll is not Capcom's stub; fix the install by hand" }
                Move-Item $xlive $capcom
            }
            if ((Get-FileHash $xlive).Hash -ne (Get-FileHash $knownGood).Hash) {
                Copy-Item $knownGood $xlive -Force
                'restored the known-good xlive.dll'
            }
        }
        foreach ($s in $settings) {
            $b = Join-Path $snap $s.Name
            if (Test-Path $b) { Copy-Item $b $s.Live -Force }
        }
        Remove-Item $lock
        "released by $Owner"
    }
    'SetKnownGood' {
        if (-not $Dll -or -not (Test-Path $Dll)) { throw '-Dll <path to xlive.dll> is required' }
        Copy-Item $Dll $knownGood -Force
        "known-good build: $((Get-FileHash $knownGood).Hash)"
    }
}
