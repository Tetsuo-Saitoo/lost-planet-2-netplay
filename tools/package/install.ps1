# LP2 Netplay - install / uninstall on any PC with Lost Planet 2 (Steam).
#   install.ps1                       install or update (asks for a gamertag and, optionally, a friend's address)
#   install.ps1 -Name Bob -Peer 1.2.3.4   same, without questions
#   install.ps1 -Uninstall            put Capcom's original xlive.dll back
#   install.ps1 -FixFirewall          (as administrator) also allow the game through Windows Firewall
param([switch]$Uninstall, [switch]$FixFirewall, [string]$GameDir, [string]$Name, [string]$Peer)

$ErrorActionPreference = 'Stop'
# Capcom's 2025 "xlive-dummy" (Steam build 17599994). Anything else is not touched.
$CapcomSha = '867BFE461A0562AEE0A43CD832A52B228AC6DA4E4216BDFBB94EF14BECE85366'

Add-Type -Namespace LP2Net -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
[DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
public static extern int GetPrivateProfileString(string section, string key, string def, System.Text.StringBuilder buf, int size, string file);
'@

function Find-GameDir {
    $reg = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Capcom\LostPlanet 2' -ErrorAction SilentlyContinue
    if ($reg -and $reg.installdir -and (Test-Path (Join-Path $reg.installdir 'LP2DX9.exe'))) { return $reg.installdir }
    # fall back to every Steam library folder
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
    if ($steam) {
        $libs = @($steam)
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            $libs += Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }
        }
        foreach ($lib in $libs) {
            $dir = Join-Path $lib 'steamapps\common\Lost Planet 2'
            if (Test-Path (Join-Path $dir 'LP2DX9.exe')) { return $dir }
        }
    }
    return $null
}

function Get-Ini([string]$file, [string]$section, [string]$key) {
    $sb = New-Object System.Text.StringBuilder 512
    [void][LP2Net.Ini]::GetPrivateProfileString($section, $key, '', $sb, $sb.Capacity, $file)
    return $sb.ToString()
}

function Test-Firewall([string]$dir, [bool]$fix) {
    $exes = @('LP2DX9.exe', 'LP2DX11.exe') | ForEach-Object { (Join-Path $dir $_).ToLower() }
    $text = netsh advfirewall firewall show rule name=all dir=in verbose
    $rules = @(); $cur = $null
    foreach ($line in $text) {
        if ($line -match '^Rule Name:\s+(.*)$') { $cur = @{ Name = $Matches[1].Trim() }; $rules += $cur }
        elseif ($cur -and $line -match '^(Enabled|Action|Program):\s+(.*)$') { $cur[$Matches[1]] = $Matches[2].Trim() }
    }
    $game = $rules | Where-Object { $_.Program -and $exes -contains $_.Program.ToLower() -and $_.Enabled -eq 'Yes' }
    $blocked = @($game | Where-Object { $_.Action -eq 'Block' } | ForEach-Object { $_.Name } | Sort-Object -Unique)
    $allowed = @($game | Where-Object { $_.Action -eq 'Allow' })
    if ($blocked.Count -eq 0 -and $allowed.Count -gt 0) {
        Write-Host 'Firewall: the game is allowed.' -ForegroundColor Green
        return
    }
    if (-not $fix) {
        if ($blocked.Count) {
            Write-Host "Firewall: rule(s) BLOCKING the game: $($blocked -join ', ')" -ForegroundColor Yellow
        } else {
            Write-Host 'Firewall: no rule for the game yet. When Windows asks on first launch, click Allow.' -ForegroundColor Yellow
        }
        Write-Host '  To fix it automatically, run "Allow through firewall.bat" (it asks for administrator rights).'
        return
    }
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { throw 'Fixing the firewall needs administrator rights.' }
    foreach ($n in $blocked) { netsh advfirewall firewall set rule name="$n" new action=allow | Out-Null }
    foreach ($exe in @('LP2DX9.exe', 'LP2DX11.exe')) {
        $p = Join-Path $dir $exe
        netsh advfirewall firewall delete rule name="Lost Planet 2 (LP2 Netplay) $exe" | Out-Null
        netsh advfirewall firewall add rule name="Lost Planet 2 (LP2 Netplay) $exe" dir=in action=allow program="$p" protocol=udp enable=yes profile=private,public | Out-Null
    }
    Write-Host 'Firewall: Lost Planet 2 is now allowed (UDP, private and public networks).' -ForegroundColor Green
}

if (-not $GameDir) { $GameDir = Find-GameDir }
if (-not $GameDir) { throw 'Lost Planet 2 was not found. Install it with Steam, or run: install.ps1 -GameDir "<folder with LP2DX9.exe>"' }
Write-Host "Lost Planet 2: $GameDir"
$xlive = Join-Path $GameDir 'xlive.dll'
$capcom = Join-Path $GameDir 'xlive_capcom.dll'
$ini = Join-Path $GameDir 'lp2net.ini'

if (Get-Process -Name lp2dx9, lp2dx11, launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($GameDir) }) {
    throw 'Lost Planet 2 is running. Quit the game first.'
}

if ($FixFirewall) { Test-Firewall $GameDir $true; return }

if ($Uninstall) {
    if (Test-Path $capcom) {
        Remove-Item $xlive -Force -ErrorAction SilentlyContinue
        Move-Item $capcom $xlive
        Write-Host "Removed LP2 Netplay; Capcom's xlive.dll is back. (lp2net.ini and lp2net.log are left in place.)" -ForegroundColor Green
    } else {
        Write-Host 'LP2 Netplay is not installed here.'
    }
    return
}

$ours = Join-Path $PSScriptRoot 'xlive.dll'
if (-not (Test-Path $ours)) { throw "xlive.dll is missing next to this script ($PSScriptRoot)." }
if (-not (Test-Path $capcom)) {
    $sha = (Get-FileHash $xlive -Algorithm SHA256).Hash
    if ($sha -ne $CapcomSha) {
        throw "The game's xlive.dll is not the version this was built for (sha256 $sha). Let Steam update the game (or verify its files) and try again."
    }
    Move-Item $xlive $capcom
    Write-Host "Kept Capcom's original as xlive_capcom.dll"
}
Copy-Item $ours $xlive -Force
if ((Get-FileHash $xlive).Hash -ne (Get-FileHash $ours).Hash) { throw 'Copying xlive.dll failed.' }
Write-Host 'Installed LP2 Netplay xlive.dll' -ForegroundColor Green

# Gamertag: also the name of the save folder, so letters, digits, space, _ and - only
if (-not $PSBoundParameters.ContainsKey('Name')) {
    $current = Get-Ini $ini 'Player' 'Name'
    $default = if ($current) { $current } else { $env:USERNAME }
    Write-Host 'In game you will use your Steam name. This name is only used if Steam is not running.'
    $answer = Read-Host "Fallback gamertag (max 15 letters/digits) [$default]"
    $Name = if ($answer) { $answer } else { $default }
}
$Name = ($Name -replace '[^A-Za-z0-9 _-]', '')
if ($Name.Length -gt 15) { $Name = $Name.Substring(0, 15) }
if (-not $Name) { $Name = 'Player' }
[void][LP2Net.Ini]::WritePrivateProfileString('Player', 'Name', $Name, $ini)

if (-not $PSBoundParameters.ContainsKey('Peer')) {
    $current = Get-Ini $ini 'Network' 'Peers'
    Write-Host 'Steam friends find each other on their own. An address is only needed to play with someone over'
    Write-Host 'the internet without Steam (or if their game does not show up).'
    $answer = Read-Host "Friend's internet address (usually leave empty) [$current]"
    $Peer = if ($answer) { $answer } else { $current }
}
[void][LP2Net.Ini]::WritePrivateProfileString('Network', 'Peers', $Peer, $ini)
[void][LP2Net.Ini]::WritePrivateProfileString('Debug', 'AllowMultipleCopies', '0', $ini)
Write-Host "Gamertag: $Name   Peers: $(if ($Peer) { $Peer } else { '(same network)' })"

Test-Firewall $GameDir $false
Write-Host ''
Write-Host 'Done. Start Lost Planet 2 from Steam as usual.' -ForegroundColor Cyan
Write-Host '  Same house: Campaign > Connection Type: System Link (or Online > System Link for versus).' -ForegroundColor Cyan
Write-Host '  Internet: the Online menu (Player Match, Ranked Match) and Campaign with Xbox LIVE work again,' -ForegroundColor Cyan
Write-Host '  and Steam friends can invite each other (lobby "Game invite") or use Join Game in Steam.' -ForegroundColor Cyan
