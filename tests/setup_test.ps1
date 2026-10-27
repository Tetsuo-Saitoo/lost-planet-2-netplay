# LP2NetSetup.exe against fake game folders in %TEMP%: install, repair, update, uninstall, the GFWL backup and
# restore, Steam putting files back, and the cases Setup must refuse. Needs Capcom's 2025 stub from a Lost Planet 2
# install (only ever copied into the temp folders). Never touches the real game folder or the firewall.
#   powershell -File tests\setup_test.ps1 [-Setup build\LP2NetSetup.exe] [-CapcomStub <path>] [-OldBuild <pre-0.2 xlive.dll>]
param([string]$Setup, [string]$CapcomStub, [string]$OldBuild, [switch]$Keep)

$ErrorActionPreference = 'Stop'
$capcomSha = '867BFE461A0562AEE0A43CD832A52B228AC6DA4E4216BDFBB94EF14BECE85366'
$leftovers = 'gfwlivesetup.exe', 'LP2DX9.exe.cat', 'LP2DX9.exe.cfg', 'LP2DX11.exe.cat', 'LP2DX11.exe.cfg'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Setup) { $Setup = Join-Path $root 'build\LP2NetSetup.exe' }
$Setup = (Resolve-Path $Setup).Path
$ours = Join-Path (Split-Path $Setup) 'xlive.dll'  # the build the exe embeds
if (-not $CapcomStub) {
    $game = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Capcom\LostPlanet 2' -ErrorAction SilentlyContinue).installdir
    foreach ($name in 'xlive_capcom.dll', 'xlive.dll') {
        $p = if ($game) { Join-Path $game $name } else { '' }
        if ($p -and (Test-Path $p) -and (Get-FileHash $p).Hash -eq $capcomSha) { $CapcomStub = $p; break }
    }
}
if (-not $CapcomStub) { "SKIP: Capcom's stub not found (pass -CapcomStub)"; exit 0 }

$tmp = Join-Path $env:TEMP "lp2setup-test-$PID"
New-Item -ItemType Directory -Force $tmp | Out-Null
$script:fails = 0
$script:n = 0
$script:out = ''

function Invoke-Setup {
    $script:out = (& $Setup @args | Out-String)
    return $LASTEXITCODE
}

function Check([string]$what, [bool]$ok) {
    if ($ok) { Write-Host "  ok    $what"; return }
    Write-Host "  FAIL  $what" -ForegroundColor Red
    Write-Host ($script:out -replace '(?m)^', '        ')
    $script:fails++
}

function Hash([string]$p) { if (Test-Path $p) { (Get-FileHash $p).Hash } else { '' } }

# Every file under the folder with its hash, to prove a refused run changed nothing.
function Snapshot([string]$dir) {
    (Get-ChildItem $dir -Recurse -File | Sort-Object FullName | ForEach-Object { "$($_.FullName.Substring($dir.Length)) $(Hash $_.FullName)" }) -join "`n"
}

function New-Game([string]$title) {
    Write-Host $title -ForegroundColor Cyan
    $g = Join-Path $tmp ('game' + (++$script:n))
    New-Item -ItemType Directory -Force $g, "$g\nativePC" | Out-Null
    foreach ($f in 'LP2DX9.exe', 'LP2DX11.exe', 'Launcher.exe', 'notes.cfg', 'LP2DX9.exe.cfgbak', 'nativePC\data.arc') {
        "dummy $f" | Set-Content "$g\$f"
    }
    foreach ($f in $leftovers) { "leftover $f $(Get-Random)" | Set-Content "$g\$f" }
    Copy-Item $CapcomStub "$g\xlive.dll"
    return $g
}

function Installed([string]$g) {
    (Hash "$g\xlive.dll") -eq (Hash $ours) -and (Hash "$g\xlive_capcom.dll") -eq $capcomSha
}

function Pristine([string]$g) {
    (Hash "$g\xlive.dll") -eq $capcomSha -and -not (Test-Path "$g\xlive_capcom.dll")
}

function Ini([string]$g, [string]$section, [string]$key) {
    $text = Get-Content "$g\lp2net.ini" -Raw
    if ($text -match "(?ms)^\[$section\][^\[]*?^$key=([^\r\n]*)") { return $Matches[1] }
    return $null
}

try {
    $g = New-Game 'Status of a fresh game folder'
    $code = Invoke-Setup --status --game-dir $g
    Check 'exit 0' ($code -eq 0)
    Check "reports Capcom's file" ($script:out -match "xlive\.dll: Capcom's 2025 file")
    Check 'lists exactly the five GFWL leftovers' (($script:out -match 'GFWL leftovers: .*gfwlivesetup\.exe') -and
        ($leftovers | Where-Object { $script:out -notmatch [regex]::Escape($_) + ' \(' }).Count -eq 0 -and
        $script:out -notmatch 'notes\.cfg|cfgbak')
    Check 'shows a firewall line (read only)' ($script:out -match 'Firewall: ')

    Write-Host 'Install with settings, no cleanup' -ForegroundColor Cyan
    $before = @{}; foreach ($f in $leftovers) { $before[$f] = Hash "$g\$f" }
    $code = Invoke-Setup --install --game-dir $g --name 'Test Player!!' --peer '203.0.113.7:3074'
    Check 'exit 0' ($code -eq 0)
    Check "xlive.dll is ours, Capcom's kept as xlive_capcom.dll" (Installed $g)
    Check 'gamertag sanitized and saved' ((Ini $g 'Player' 'Name') -eq 'Test Player')
    Check 'friend address saved' ((Ini $g 'Network' 'Peers') -eq '203.0.113.7:3074')
    Check 'AllowMultipleCopies=0' ((Ini $g 'Debug' 'AllowMultipleCopies') -eq '0')
    Check 'leftovers untouched without --cleanup' (($leftovers | Where-Object { (Hash "$g\$_") -ne $before[$_] }).Count -eq 0)
    Check 'no temp file left' (-not (Test-Path "$g\xlive.dll.lp2new"))
    $code = Invoke-Setup --status --game-dir $g
    Check 'status: LP2 Netplay 0.2.0 installed' ($script:out -match 'xlive\.dll: LP2 Netplay 0\.2\.0')

    Write-Host 'Repair (read-only xlive.dll, settings left alone)' -ForegroundColor Cyan
    Set-ItemProperty "$g\xlive.dll" -Name IsReadOnly -Value $true
    $code = Invoke-Setup --repair --game-dir $g
    Check 'exit 0' ($code -eq 0)
    Check 'says Repaired' ($script:out -match 'Repaired LP2 Netplay 0\.2\.0')
    Check 'still installed' (Installed $g)
    Check 'settings kept when not given' ((Ini $g 'Player' 'Name') -eq 'Test Player' -and (Ini $g 'Network' 'Peers') -eq '203.0.113.7:3074')

    if ($OldBuild) {
        Write-Host 'Update from a test build without a version resource' -ForegroundColor Cyan
        Copy-Item $OldBuild "$g\xlive.dll" -Force
        $code = Invoke-Setup --status --game-dir $g
        Check 'status: test build recognised as ours' ($script:out -match 'LP2 Netplay \(test build\)')
        $code = Invoke-Setup --install --game-dir $g
        Check 'exit 0' ($code -eq 0)
        Check 'says Updated from a test build' ($script:out -match 'Updated LP2 Netplay from a test build to 0\.2\.0')
        Check 'installed' (Installed $g)
    }

    Write-Host 'Install with --cleanup' -ForegroundColor Cyan
    $code = Invoke-Setup --install --cleanup --game-dir $g
    Check 'exit 0' ($code -eq 0)
    Check 'leftovers moved into LP2Netplay-backup with the same bytes' (($leftovers | Where-Object {
        (Test-Path "$g\$_") -or (Hash "$g\LP2Netplay-backup\$_") -ne $before[$_] }).Count -eq 0)
    Check 'other files kept' ((Test-Path "$g\notes.cfg") -and (Test-Path "$g\LP2DX9.exe.cfgbak") -and (Test-Path "$g\Launcher.exe"))
    $code = Invoke-Setup --status --game-dir $g
    Check 'status lists the backup' ($script:out -match 'Backed up: .*gfwlivesetup\.exe' -and $script:out -match 'GFWL leftovers: none')

    Write-Host 'Uninstall' -ForegroundColor Cyan
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'exit 0' ($code -eq 0)
    Check "Capcom's file back, xlive_capcom.dll gone" (Pristine $g)
    Check 'leftovers back with the same bytes' (($leftovers | Where-Object { (Hash "$g\$_") -ne $before[$_] }).Count -eq 0)
    Check 'backup folder removed' (-not (Test-Path "$g\LP2Netplay-backup"))
    Check 'lp2net.ini kept' (Test-Path "$g\lp2net.ini")
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'second uninstall: exit 0, not installed' ($code -eq 0 -and $script:out -match 'not installed')

    $g = New-Game 'Steam put files back after an update or a file check'
    $code = Invoke-Setup --install --cleanup --game-dir $g
    Copy-Item $CapcomStub "$g\xlive.dll" -Force
    foreach ($f in $leftovers) { Copy-Item "$g\LP2Netplay-backup\$f" "$g\$f" }
    $code = Invoke-Setup --status --game-dir $g
    Check 'status: not active, Steam put it back' ($script:out -match 'not active')
    $code = Invoke-Setup --install --cleanup --game-dir $g
    Check 'install again: exit 0' ($code -eq 0)
    Check 'installed' (Installed $g)
    Check 'identical leftovers removed from the game folder (backup has them)' (($leftovers | Where-Object { Test-Path "$g\$_" }).Count -eq 0)
    foreach ($f in $leftovers) { Copy-Item "$g\LP2Netplay-backup\$f" "$g\$f" }
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'uninstall with leftovers already back: exit 0' ($code -eq 0)
    Check 'pristine, no backup folder' ((Pristine $g) -and -not (Test-Path "$g\LP2Netplay-backup"))
    Copy-Item $CapcomStub "$g\xlive_capcom.dll"
    $code = Invoke-Setup --uninstall --game-dir $g
    Check "duplicate Capcom copy removed" ($code -eq 0 -and (Pristine $g))

    $g = New-Game 'Refuse: unknown xlive.dll'
    [IO.File]::WriteAllBytes("$g\xlive.dll", [byte[]](1..200))
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g
    Check 'install exit 1, says not Capcom and not ours' ($code -eq 1 -and $script:out -match 'not Capcom.s 2025 file and not LP2 Netplay')
    Check 'nothing changed' ((Snapshot $g) -eq $snap)
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'uninstall leaves it alone' ((Snapshot $g) -eq $snap -and $script:out -match 'not installed')

    $g = New-Game "Refuse: xlive_capcom.dll is not Capcom's"
    [IO.File]::WriteAllBytes("$g\xlive_capcom.dll", [byte[]](1..200))
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g
    Check 'install exit 1' ($code -eq 1 -and $script:out -match "xlive_capcom\.dll in the game folder is not Capcom")
    Check 'nothing changed' ((Snapshot $g) -eq $snap)

    $g = New-Game "Refuse: ours installed but Capcom's file missing"
    Copy-Item $ours "$g\xlive.dll" -Force
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g
    Check 'install exit 1, points to Steam verify' ($code -eq 1 -and $script:out -match 'Verify integrity')
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'uninstall exit 1, points to Steam verify' ($code -eq 1 -and $script:out -match 'Verify integrity')
    Check 'nothing changed' ((Snapshot $g) -eq $snap)

    $g = New-Game 'Refuse: bad friend address'
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g --peer '203.0.113.7/24 <x>'
    Check 'exit 1, nothing changed' ($code -eq 1 -and (Snapshot $g) -eq $snap)

    $g = New-Game 'Refuse: not a game folder'
    Remove-Item "$g\LP2DX9.exe"
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g
    Check 'exit 1, nothing changed' ($code -eq 1 -and $script:out -match 'LP2DX9\.exe is missing' -and (Snapshot $g) -eq $snap)

    $g = New-Game 'Refuse: the game is running from this folder'
    Copy-Item "$env:SystemRoot\System32\PING.EXE" "$g\LP2DX9.exe" -Force
    $p = Start-Process "$g\LP2DX9.exe" -ArgumentList '-n', '30', '127.0.0.1' -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500
    $snap = Snapshot $g
    $code = Invoke-Setup --install --game-dir $g
    Check 'install exit 1, says running' ($code -eq 1 -and $script:out -match 'is running')
    $code = Invoke-Setup --uninstall --game-dir $g
    Check 'uninstall exit 1, says running' ($code -eq 1 -and $script:out -match 'is running')
    Check 'nothing changed' ((Snapshot $g) -eq $snap)
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    $p.WaitForExit()

    $g = New-Game 'Locked files'
    $snap = Snapshot $g
    $fs = [IO.File]::Open("$g\xlive.dll", 'Open', 'Read', 'None')
    $code = Invoke-Setup --install --game-dir $g
    $fs.Close()
    Check 'unreadable xlive.dll: exit 1, says it could not read it' ($code -eq 1 -and $script:out -match 'could not read')
    Check 'nothing changed' ((Snapshot $g) -eq $snap)
    $fs = [IO.File]::Open("$g\xlive.dll", 'Open', 'Read', 'Read')
    $code = Invoke-Setup --install --game-dir $g
    $fs.Close()
    Check "in use: exit 1, Capcom's file still in place" ($code -eq 1 -and (Pristine $g))
    $code = Invoke-Setup --install --game-dir $g
    $fs = [IO.File]::Open("$g\xlive.dll", 'Open', 'Read', 'Read')
    $code = Invoke-Setup --repair --game-dir $g
    $fs.Close()
    Check 'repair while in use: exit 1, old file kept, no temp file' ($code -eq 1 -and (Installed $g) -and -not (Test-Path "$g\xlive.dll.lp2new"))

    $g = New-Game 'Firewall dry run (reads rules, changes nothing)'
    $code = Invoke-Setup --firewall --dry-run --game-dir $g
    Check 'exit 0, plans a rule per exe' ($code -eq 0 -and $script:out -match 'would: Add the rule "Lost Planet 2 \(LP2 Netplay\) LP2DX9\.exe"' -and
        $script:out -match 'LP2DX11\.exe"' -and $script:out -match 'nothing was changed')

    Write-Host 'Command line' -ForegroundColor Cyan
    $code = Invoke-Setup --bogus
    Check 'unknown option: exit 2 with usage' ($code -eq 2 -and $script:out -match '--install')
} finally {
    if (-not $Keep) { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

if ($script:fails) { Write-Host "$($script:fails) check(s) FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'All setup checks passed' -ForegroundColor Green
exit 0
