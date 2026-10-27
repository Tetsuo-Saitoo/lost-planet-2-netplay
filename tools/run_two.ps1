# Start two copies of Lost Planet 2 for System Link testing and place them side by side (960x570 each).
# Needs [Debug] AllowMultipleCopies=1 in lp2net.ini. PIDs go to re-work\pids.txt (copy 0 left, copy 1 right);
# use focus_lp2.ps1 -Index N to give one of them the keyboard.
param([switch]$Dx11)

$run = Join-Path $PSScriptRoot 'run_lp2.ps1'
$p1 = if ($Dx11) { & $run -Dx11 } else { & $run }
Start-Sleep -Seconds 12
$p2 = if ($Dx11) { & $run -Dx11 } else { & $run }
Set-Content (Join-Path $PSScriptRoot '..\re-work\pids.txt') "$p1 $p2"

Add-Type -Namespace LP2 -Name Place -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
'@
$deadline = (Get-Date).AddSeconds(30)
foreach ($pair in @(@($p1, 0), @($p2, 960))) {
    do {
        $h = (Get-Process -Id $pair[0] -ErrorAction SilentlyContinue).MainWindowHandle
        if (-not $h -or $h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 500 }
    } while ((-not $h -or $h -eq [IntPtr]::Zero) -and (Get-Date) -lt $deadline)
    [LP2.Place]::SetWindowPos($h, [IntPtr]::Zero, $pair[1], 60, 960, 570, 0x0014) | Out-Null
}
"copy 0 pid $p1 (left), copy 1 pid $p2 (right)"
